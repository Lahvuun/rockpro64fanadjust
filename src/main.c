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

static int open_hwmon(char *name, size_t name_symbols, char *node, int oflag)
{
	int fd = -1;

	char *hwmon_dir = calloc(PATH_MAX, sizeof(char));
	if (!hwmon_dir) {
		log_fail("calloc", __FILE__, __LINE__);
		return -1;
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
	fd = open(hwmon_node, oflag);
	if (fd < 0) {
		perror("open() failed");
	}

cleanup_hwmon_node:
	free(hwmon_node);
cleanup_hwmon_dir:
	free(hwmon_dir);

	return fd;
}

static int write_fan_speed(int fd, double value)
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
	// TODO: handle partial writes.
	// TODO: handle EINTR.
	if (write(fd, value_str, 5) < 0) {
		perror("fwrite() failed");
		return -1;
	}

	return 0;
}

static int read_double(int fd, char *double_str, size_t double_str_length,
		       double *out_value)
{
	// TODO: handle partial reads.
	// TODO: handle EINTR.
	ssize_t r = pread(fd, double_str, double_str_length, 0);
	if (r < 0) {
		perror("read() failed");
		return -1;
	}
	*out_value = strtod(double_str, NULL);
	if (errno) {
		perror("strtod() failed");
		return -1;
	}

	return 0;
}

static int set_fan_speed_from_temp(int fan_fd, int cpu_fd, double min_temp,
				   double max_temp, double min_fan_speed)
{
	int status = 0;

	char *hwmon_value_str = calloc(16, sizeof(char));
	if (!hwmon_value_str) {
		log_fail("calloc", __FILE__, __LINE__);
		return -1;
	}
	double speed_old = MAX_FAN_SPEED;
	if (read_double(fan_fd, hwmon_value_str, 16, &speed_old)) {
		log_fail("read_double", __FILE__, __LINE__);
		status = -1;
		goto cleanup_hwmon_value_str;
	}
	double temp = 0.0;
	double speed_new = 0.0;
	double multiplier =
		(MAX_FAN_SPEED - min_fan_speed) / (max_temp - min_temp);
	double speed_diff = 0.0;
	while (!got_sigterm) {
		if (read_double(cpu_fd, hwmon_value_str, 16, &temp)) {
			log_fail("read_double", __FILE__, __LINE__);
			status = -1;
			break;
		}
		if (temp <= min_temp) {
			speed_new = min_fan_speed;
		} else if (temp >= max_temp) {
			speed_new = MAX_FAN_SPEED;
		} else {
			speed_new =
				min_fan_speed + multiplier * (temp - min_temp);
		}
		speed_diff = speed_old - speed_new;
		if (speed_diff <= -1 || speed_diff >= 1) {
			if (write_fan_speed(fan_fd, speed_new)) {
				log_fail("write_fan_speed", __FILE__, __LINE__);
				status = -1;
				break;
			}
			speed_old = speed_new;
		}
		sleep(10);
	}
	write_fan_speed(fan_fd, MAX_FAN_SPEED);

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

	if (argc != 4) {
		if (argc < 1) {
			fprintf(stderr, "argc is < 1\n");
			return EXIT_FAILURE;
		}
		fprintf(stderr, "usage: %s min_temp max_temp min_fan_speed\n",
			argv[0]);
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
	double min_fan_speed = strtod(argv[3], NULL);
	if (errno) {
		perror("strtod() failed\n");
		return EXIT_FAILURE;
	}

	int cpu_fd = open_hwmon(HWMON_NAME_CPU, sizeof(HWMON_NAME_CPU) - 1,
				"temp1_input", O_RDONLY);
	if (!cpu_fd) {
		log_fail("open_hwmon", __FILE__, __LINE__);
		return EXIT_FAILURE;
	}
	int fan_fd = open_hwmon(HWMON_NAME_FAN, sizeof(HWMON_NAME_FAN) - 1,
				"pwm1", O_RDWR);
	if (!fan_fd) {
		log_fail("open_hwmon", __FILE__, __LINE__);
		status = EXIT_FAILURE;
		goto cleanup_cpu_fd;
	}

	if (set_fan_speed_from_temp(fan_fd, cpu_fd, min_temp, max_temp,
				    min_fan_speed)) {
		log_fail("set_fan_speed_from_temp", __FILE__, __LINE__);
		status = EXIT_FAILURE;
	}

	if (close(fan_fd) < 0) {
		perror("close() failed");
		status = EXIT_FAILURE;
	}
cleanup_cpu_fd:
	if (close(cpu_fd) < 0) {
		perror("close() failed");
		status = EXIT_FAILURE;
	}

	return status;
}
