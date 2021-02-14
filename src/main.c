#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

static sig_atomic_t got_sigterm = 0;

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

static int find_hwmon_path(char *name, size_t name_length, DIR *dir,
			   char *out_path)
{
	int status = 0;

	char *name_path = calloc(PATH_MAX, sizeof(char));
	if (!name_path) {
		log_fail("calloc", __FILE__, __LINE__);
		return -1;
	}
	char *name_value = calloc(128, sizeof(char));
	if (!name_value) {
		log_fail("calloc", __FILE__, __LINE__);
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
	return status;
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

static int read_double_from_file(FILE *file, double *out_value)
{
	int status = 0;

	size_t fan_speed_length = 128;
	char *fan_speed = calloc(fan_speed_length, sizeof(char));
	if (!fan_speed) {
		log_fail("calloc", __FILE__, __LINE__);
		return -1;
	}
	if (getline(&fan_speed, &fan_speed_length, file) < 0 && errno) {
		perror("getline() failed");
		status = -1;
		goto cleanup_fan_speed;
	}
	*out_value = strtod(fan_speed, NULL);
	if (errno) {
		perror("strtod() failed");
		status = -1;
	}
cleanup_fan_speed:
	free(fan_speed);

	return status;
}

static int set_fan_speed_from_temp(char *cpu_hwmon_path, char *fan_hwmon_path,
				   double min_temp, double max_temp)
{
	int status = 0;

	char *cpu_temp_path = calloc(PATH_MAX, sizeof(char));
	if (!cpu_temp_path) {
		perror("calloc() failed");
		return -1;
	}
	char *fan_pwm_path = calloc(PATH_MAX, sizeof(char));
	if (!fan_pwm_path) {
		perror("calloc() failed");
		status = -1;
		goto cleanup_cpu_temp_path;
	}

	if (snprintf(cpu_temp_path, PATH_MAX, "%s/temp1_input",
		     cpu_hwmon_path) < 0) {
		fprintf(stderr, "snprintf() failed");
		status = -1;
		goto cleanup_fan_pwm_path;
	}
	FILE *cpu_file = fopen(cpu_temp_path, "r");
	if (!cpu_file) {
		perror("fopen(cpu_temp_path, \"r\") failed");
		status = -1;
		goto cleanup_fan_pwm_path;
	}
	if (snprintf(fan_pwm_path, PATH_MAX, "%s/pwm1", fan_hwmon_path) < 0) {
		fprintf(stderr, "snprintf() failed");
		status = -1;
		goto cleanup_cpu_file;
	}
	FILE *fan_file = fopen(fan_pwm_path, "r+");
	if (!fan_file) {
		perror("fopen(cpu_temp_path, \"r\") failed");
		status = -1;
		goto cleanup_cpu_file;
	}

	double old_fan_speed = 0.0;
	if (read_double_from_file(fan_file, &old_fan_speed)) {
		log_fail("read_double_from_file", __FILE__, __LINE__);
		status = -1;
		goto cleanup_cpu_file;
	}
	double multiplier = MAX_FAN_SPEED / (max_temp - min_temp);
	double temp = 0.0;
	double new_fan_speed = 0.0;
	double speed_diff = 0.0;
	while (!got_sigterm) {
		if (read_double_from_file(cpu_file, &temp)) {
			log_fail("read_double_from_file", __FILE__, __LINE__);
			status = -1;
			break;
		}
		if (temp < -30000.0 || temp > 150000.0) {
			fprintf(stderr, "temperature outside expected range\n");
			status = -1;
			break;
		}
		if (temp < min_temp) {
			new_fan_speed = 0.0;
		} else if (temp > max_temp) {
			new_fan_speed = MAX_FAN_SPEED;
		} else {
			new_fan_speed = multiplier * (temp - min_temp);
		}
		speed_diff = new_fan_speed - old_fan_speed;
		if (speed_diff >= 1 || speed_diff <= -1) {
			if (write_fan_speed(new_fan_speed, fan_file)) {
				fprintf(stderr, "write_fan_speed() failed\n");
				status = -1;
				break;
			}
			old_fan_speed = new_fan_speed;
		}
		sleep(10);
	}
	write_fan_speed(MAX_FAN_SPEED, fan_file);

	if (fclose(fan_file) == EOF) {
		perror("fclose(fan_file) failed");
		status = -1;
	}
cleanup_cpu_file:
	if (fclose(cpu_file) == EOF) {
		perror("fclose(cpu_file) failed");
		status = -1;
	}
cleanup_fan_pwm_path:
	free(fan_pwm_path);
cleanup_cpu_temp_path:
	free(cpu_temp_path);

	return status;
}

int main(int argc, char **argv)
{
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

	char *cpu_hwmon_path = calloc(PATH_MAX, sizeof(char));
	if (!cpu_hwmon_path) {
		perror("calloc() failed for cpu_hwmon_path");
		return EXIT_FAILURE;
	}

	int status = EXIT_SUCCESS;
	char *fan_hwmon_path = calloc(PATH_MAX, sizeof(char));
	if (!fan_hwmon_path) {
		perror("calloc() failed for fan_hwmon_path");
		status = EXIT_FAILURE;
		goto cleanup_cpu_hwmon_path;
	}

	DIR *hwmons_dir = opendir(HWMON_DIR_PATH);
	if (!hwmons_dir) {
		perror("opendir(" HWMON_DIR_PATH ") failed");
		status = EXIT_FAILURE;
		goto cleanup_fan_hwmon_path;
	}

	if (find_hwmon_path(HWMON_NAME_CPU, sizeof(HWMON_NAME_CPU) - 1,
			    hwmons_dir, cpu_hwmon_path)) {
		log_fail("find_hwmon_path", __FILE__, __LINE__);
		status = EXIT_FAILURE;
		goto cleanup_hwmons_dir;
	}
	rewinddir(hwmons_dir);
	if (find_hwmon_path(HWMON_NAME_FAN, sizeof(HWMON_NAME_FAN) - 1,
			    hwmons_dir, fan_hwmon_path)) {
		log_fail("find_hwmon_path", __FILE__, __LINE__);
		status = EXIT_FAILURE;
		goto cleanup_hwmons_dir;
	}

	if (set_fan_speed_from_temp(cpu_hwmon_path, fan_hwmon_path, min_temp,
				    max_temp)) {
		log_fail("set_fan_speed_from_temp", __FILE__, __LINE__);
		status = EXIT_FAILURE;
	}

cleanup_hwmons_dir:
	if (closedir(hwmons_dir)) {
		perror("closedir(" HWMON_DIR_PATH ") failed");
		status = EXIT_FAILURE;
	}
cleanup_fan_hwmon_path:
	free(fan_hwmon_path);
cleanup_cpu_hwmon_path:
	free(cpu_hwmon_path);

	return status;
}
