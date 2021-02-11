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

static int read_file_contents(char *name)
{
	int status = 0;

	FILE *file = fopen(name, "r");
	if (!file) {
		perror("fopen() failed");
		return -1;
	}
	size_t name_length = PATH_MAX;
	if (getline(&name, &name_length, file) < 0 && errno) {
		perror("getline() failed");
		status = -1;
	}
	if (fclose(file) == EOF) {
		perror("fclose() failed");
		status = -1;
	}

	return status;
}

static int find_hwmon_paths(DIR *hwmons_dir, char *cpu_hwmon_path,
			    char *fan_hwmon_path)
{
	int status = 0;

	char *current_name = calloc(PATH_MAX, sizeof(char));
	for (struct dirent *current = readdir(hwmons_dir); current && !status;
	     current = readdir(hwmons_dir)) {
		if (current->d_name[0] == '.') {
			continue;
		}
		if (snprintf(current_name, PATH_MAX, HWMON_DIR_PATH "%s/name",
			     current->d_name) < 0) {
			fprintf(stderr, "snprintf() failed\n");
			status = -1;
			goto cleanup_current_name;
		}
		if (read_file_contents(current_name)) {
			fprintf(stderr, "read_file_contents() failed\n");
			status = -1;
			goto cleanup_current_name;
		}
		if (!strcmp(current_name, "cpu\n")) {
			if (snprintf(cpu_hwmon_path, PATH_MAX,
				     HWMON_DIR_PATH "%s",
				     current->d_name) < 0) {
				fprintf(stderr, "snprintf() failed");
				status = -1;
				goto cleanup_current_name;
			}
		} else if (!strcmp(current_name, "pwmfan\n")) {
			if (snprintf(fan_hwmon_path, PATH_MAX,
				     HWMON_DIR_PATH "%s",
				     current->d_name) < 0) {
				fprintf(stderr, "snprintf() failed");
				status = -1;
				goto cleanup_current_name;
			}
		}
	}
	if (errno) {
		perror("readdir() failed");
		return -1;
	}

cleanup_current_name:
	free(current_name);

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
	FILE *fan_file = fopen(fan_pwm_path, "w");
	if (!fan_file) {
		perror("fopen(cpu_temp_path, \"r\") failed");
		status = -1;
		goto cleanup_cpu_file;
	}

	size_t temperature_length = 128;
	char *temperature = calloc(temperature_length, sizeof(char));
	double multiplier = MAX_FAN_SPEED / (max_temp - min_temp);
	double temp = 0.0;
	double old_fan_speed = 255.0;
	double new_fan_speed = 0.0;
	double speed_diff = 0.0;
	while (!got_sigterm) {
		if (getline(&temperature, &temperature_length, cpu_file) < 0 &&
		    errno) {
			perror("getline() failed");
			status = -1;
			break;
		}
		temp = strtod(temperature, NULL);
		if (errno) {
			perror("strtol() failed");
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
	free(temperature);

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

	if (find_hwmon_paths(hwmons_dir, cpu_hwmon_path, fan_hwmon_path)) {
		fprintf(stderr, "find_hwmon_paths() failed\n");
		status = EXIT_FAILURE;
		goto cleanup_hwmons_dir;
	}

	if (!strncmp(cpu_hwmon_path, "", PATH_MAX)) {
		fprintf(stderr, "couldn't find cpu hwmon\n");
		status = EXIT_FAILURE;
		goto cleanup_hwmons_dir;
	}
	if (!strncmp(fan_hwmon_path, "", PATH_MAX)) {
		fprintf(stderr, "couldn't find fan hwmon\n");
		status = EXIT_FAILURE;
		goto cleanup_hwmons_dir;
	}

	set_fan_speed_from_temp(cpu_hwmon_path, fan_hwmon_path, min_temp,
				max_temp);

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
