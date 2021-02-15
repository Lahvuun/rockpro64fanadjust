#include <dirent.h>
#include <errno.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HWMON_DIR_PATH "/sys/class/hwmon/"
#define HWMON_NAME_CPU "cpu"
#define HWMON_NAME_FAN "pwmfan"
#define MAX_FAN_SPEED 255.0

static volatile sig_atomic_t got_sigterm = 0;

static void handle_sigterm(int signum)
{
	switch (signum) {
	case SIGTERM:
		got_sigterm = 1;
		break;
	default:
		break;
	}
}

static void log_fail(char *function_name, char *filename, int line)
{
	fprintf(stderr, "%s() failed at %s:%d\n", function_name, filename,
		line);
}

static int read_line(char *file_path, char *out_line, size_t out_line_length)
{
	int status = 0;

	FILE *f = fopen(file_path, "r");
	if (!f) {
		perror("fopen() failed");
		return -1;
	}
	if (getline(&out_line, &out_line_length, f) < 1) {
		perror("getline() failed");
		status = -1;
	}
	if (fclose(f) == EOF) {
		perror("fclose() failed");
		status = -1;
	}

	return status;
}

static int find_hwmon_path(char *name, size_t name_length, char *out_path)
{
	int status = 0;

	DIR *dir = opendir(HWMON_DIR_PATH);
	if (!dir) {
		perror("opendir(" HWMON_DIR_PATH ") failed");
		return -1;
	}

	char *name_path = calloc(PATH_MAX, sizeof(char));
	if (!name_path) {
		log_fail("calloc", __FILE__, __LINE__);
		status = -1;
		goto cleanup_dir;
	}
	char *name_value = calloc(128, sizeof(char));
	if (!name_value) {
		log_fail("calloc", __FILE__, __LINE__);
		status = -1;
		goto cleanup_name_path;
	}

	errno = 0;
	for (struct dirent *dir_entry = readdir(dir); dir_entry;
	     dir_entry = readdir(dir)) {
		if (dir_entry->d_name[0] == '.') {
			continue;
		}
		if (snprintf(out_path, PATH_MAX, HWMON_DIR_PATH "%s",
			     dir_entry->d_name) < 0) {
			log_fail("snprintf", __FILE__, __LINE__);
			status = -1;
			goto cleanup_name_value;
		}
		if (snprintf(name_path, PATH_MAX, "%s/name", out_path) < 0) {
			log_fail("snprintf", __FILE__, __LINE__);
			status = -1;
			goto cleanup_name_value;
		}
		if (read_line(name_path, name_value, 128)) {
			log_fail("read_line", __FILE__, __LINE__);
			status = -1;
			goto cleanup_name_value;
		}
		if (!strncmp(name, name_value, name_length)) {
			goto cleanup_name_value;
		}
	}
	if (errno) {
		perror("readdir() failed");
	}
	status = -1;

cleanup_name_value:
	free(name_value);
cleanup_name_path:
	free(name_path);
cleanup_dir:
	if (closedir(dir)) {
		perror("closedir(" HWMON_DIR_PATH ") failed");
		status = -1;
	}

	return status;
}

static FILE *fopen_hwmon(char *name, size_t name_symbols, char *node,
			 char *modes)
{
	FILE *file = NULL;

	char *hwmon_dir = calloc(PATH_MAX, sizeof(char));
	if (!hwmon_dir) {
		log_fail("calloc", __FILE__, __LINE__);
		return NULL;
	}
	if (find_hwmon_path(name, name_symbols, hwmon_dir)) {
		log_fail("find_hwmon_path", __FILE__, __LINE__);
		goto cleanup_hwmon_dir;
	}
	char *hwmon_node = calloc(PATH_MAX, sizeof(char));
	if (!hwmon_node) {
		log_fail("calloc", __FILE__, __LINE__);
		goto cleanup_hwmon_dir;
	}
	if (snprintf(hwmon_node, PATH_MAX, "%s/%s", hwmon_dir, node) < 0) {
		log_fail("snprintf", __FILE__, __LINE__);
		goto cleanup_hwmon_node;
	}
	file = fopen(hwmon_node, modes);
	if (!file) {
		perror("fopen() failed");
	}

cleanup_hwmon_node:
	free(hwmon_node);
cleanup_hwmon_dir:
	free(hwmon_dir);

	return file;
}

static int write_fan_speed(double value, FILE *file)
{
	if (value < 0.0) {
		fprintf(stderr,
			"can't set fan speed lower than 0, setting 0\n");
		value = 0.0;
	}
	if (value > MAX_FAN_SPEED) {
		fprintf(stderr,
			"can't set fan speed higher than %f, setting %f\n",
			MAX_FAN_SPEED, MAX_FAN_SPEED);
		value = MAX_FAN_SPEED;
	}

	char value_str[5] = "";
	snprintf(value_str, 5, "%.0f\n", value);
	size_t items_written = 0;
	do {
		items_written += fwrite(value_str, sizeof(char), 5, file);
		if (ferror(file)) {
			fprintf(stderr, "fwrite() failed\n");
			return -1;
		}
	} while (items_written < 5);
	if (fflush(file) == EOF) {
		perror("fflush() failed");
		return -1;
	}

	return 0;
}

static int read_double(FILE *file, char *double_str, size_t double_str_length,
		       double *out_value)
{
	if (getline(&double_str, &double_str_length, file) < 0 && errno) {
		perror("getline() failed");
		return -1;
	}
	*out_value = strtod(double_str, NULL);
	if (errno) {
		perror("strtod() failed");
		return -1;
	}

	return 0;
}

static int set_fan_speed_from_temp(FILE *fan_file, FILE *cpu_file,
				   double min_temp, double max_temp)
{
	int status = 0;

	char *hwmon_value_str = calloc(16, sizeof(char));
	if (!hwmon_value_str) {
		log_fail("calloc", __FILE__, __LINE__);
		return -1;
	}
	double speed_old = MAX_FAN_SPEED;
	if (read_double(fan_file, hwmon_value_str, 16, &speed_old)) {
		log_fail("read_double", __FILE__, __LINE__);
		status = -1;
		goto cleanup_hwmon_value_str;
	}
	double temp = 0.0;
	double speed_new = 0.0;
	double multiplier = MAX_FAN_SPEED / (max_temp - min_temp);
	double speed_diff = 0.0;
	while (!got_sigterm) {
		if (read_double(cpu_file, hwmon_value_str, 16, &temp)) {
			log_fail("read_double", __FILE__, __LINE__);
			status = -1;
			break;
		}
		if (temp <= min_temp) {
			speed_new = 0.0;
		} else if (temp >= max_temp) {
			speed_new = MAX_FAN_SPEED;
		} else {
			speed_new = multiplier * (temp - min_temp);
		}
		speed_diff = speed_old - speed_new;
		if (speed_diff <= -1 || speed_diff >= 1) {
			if (write_fan_speed(speed_new, fan_file)) {
				log_fail("write_fan_speed", __FILE__, __LINE__);
				status = -1;
				break;
			}
		}
		sleep(10);
	}
	write_fan_speed(MAX_FAN_SPEED, fan_file);

cleanup_hwmon_value_str:
	free(hwmon_value_str);

	return status;
}

int main(int argc, char **argv)
{
	int status = EXIT_SUCCESS;

	sigset_t sigterm_set;
	if (sigemptyset(&sigterm_set)) {
		perror("sigemptyset() failed");
		return EXIT_FAILURE;
	}
	struct sigaction sigterm_handler = {
		.sa_handler = handle_sigterm,
		.sa_mask = sigterm_set,
		.sa_flags = 0,
	};
	if (sigaction(SIGTERM, &sigterm_handler, NULL)) {
		perror("sigaction() failed");
		return EXIT_FAILURE;
	}

	if (argc != 3) {
		if (argc < 1) {
			fprintf(stderr, "argc is < 1\n");
			return EXIT_FAILURE;
		}
		fprintf(stderr, "usage: %s min_temp max_temp\n", argv[0]);
		return EXIT_FAILURE;
	}
	double min_temp = strtod(argv[1], NULL);
	if (errno) {
		perror("strtod() failed\n");
		return EXIT_FAILURE;
	}
	double max_temp = strtod(argv[2], NULL);
	if (errno) {
		perror("strtod() failed\n");
		return EXIT_FAILURE;
	}

	FILE *cpu_file = fopen_hwmon(HWMON_NAME_CPU, sizeof(HWMON_NAME_CPU) - 1,
				     "temp1_input", "r");
	if (!cpu_file) {
		log_fail("fopen_hwmon", __FILE__, __LINE__);
		return EXIT_FAILURE;
	}
	FILE *fan_file = fopen_hwmon(HWMON_NAME_FAN, sizeof(HWMON_NAME_FAN) - 1,
				     "pwm1", "r+");
	if (!fan_file) {
		log_fail("fopen_hwmon", __FILE__, __LINE__);
		status = EXIT_FAILURE;
		goto cleanup_cpu_file;
	}

	if (set_fan_speed_from_temp(fan_file, cpu_file, min_temp, max_temp)) {
		log_fail("set_fan_speed_from_temp", __FILE__, __LINE__);
		status = EXIT_FAILURE;
	}

	if (fclose(fan_file) == EOF) {
		perror("fclose() failed");
		status = EXIT_FAILURE;
	}
cleanup_cpu_file:
	if (fclose(cpu_file) == EOF) {
		perror("fclose() failed");
		status = EXIT_FAILURE;
	}

	return status;
}
