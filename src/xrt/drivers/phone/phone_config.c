// Copyright 2026, Tom F.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Configuration for Phone HMD driver.
 * @author Tom F. <tom.fucik@email.cz>
 * @ingroup drv_phone
 */

#include "phone_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *path = "/home/tom/config.cfg";

// Create config file if it doesn't exist
void
config_init(void)
{
	FILE *file = fopen(path, "r");
	if (file) {
		fclose(file);
		return;
	}
	FILE *tmp = fopen(path, "w");
	if (!tmp) {
		return;
	}
	fprintf(tmp,
	        "port=5500\n"
	        "multicast_addr=239.1.1.1\n"
	        "config_port=5501\n"
	        "stream_port=5502\n"
	        "pose_port=5503\n"
	        "stream_w=1280\n"
	        "stream_h=720\n"
	        "screen_w=2400\n"
	        "screen_h=1080\n"
	        "screen_w_m=0.16\n"
	        "screen_h_m=0.07\n"
	        "k1=0.12\n"
	        "k2=0.12\n"
	        "inter_lens=0.060\n"
	        "screen_to_lens=0.050\n"
	        "tray_to_lens=0.035\n");
	fclose(tmp);
}

// Set config value
int
config_set(const char *key, const char *value)
{
	FILE *file = fopen(path, "r");
	FILE *tmp;
	char line[1024];
	char *buffer = NULL;
	size_t size = 0;
	int found = 0;

	if (file) {
		while (fgets(line, sizeof(line), file)) {
			char *eq = strchr(line, '=');

			if (!eq) {
				size_t len = strlen(line);
				char *new_buffer = realloc(buffer, size + len + 1);
				if (!new_buffer) {
					free(buffer);
					fclose(file);
					return -1;
				}

				buffer = new_buffer;
				memcpy(buffer + size, line, len);
				size += len;
				buffer[size] = '\0';
				continue;
			}

			*eq = '\0';

			if (strcmp(line, key) == 0) {
				size_t len = strlen(key) + strlen(value) + 2;
				char *new_buffer = realloc(buffer, size + len + 1);

				if (!new_buffer) {
					free(buffer);
					fclose(file);
					return -1;
				}

				buffer = new_buffer;
				size += sprintf(buffer + size, "%s=%s\n", key, value);
				found = 1;
			} else {
				*eq = '=';
				size_t len = strlen(line);
				char *new_buffer = realloc(buffer, size + len + 1);

				if (!new_buffer) {
					free(buffer);
					fclose(file);
					return -1;
				}

				buffer = new_buffer;
				memcpy(buffer + size, line, len);
				size += len;
				buffer[size] = '\0';
			}
		}

		fclose(file);
	}

	if (!found) {
		size_t len = strlen(key) + strlen(value) + 2;
		char *new_buffer = realloc(buffer, size + len + 1);

		if (!new_buffer) {
			free(buffer);
			return -1;
		}

		buffer = new_buffer;
		size += sprintf(buffer + size, "%s=%s\n", key, value);
	}

	tmp = fopen(path, "w");

	if (!tmp) {
		free(buffer);
		return -1;
	}

	if (buffer)
		fputs(buffer, tmp);

	fclose(tmp);
	free(buffer);

	return 0;
}

// Get config value
char *
config_get(const char *key)
{
	FILE *file = fopen(path, "r");
	char line[1024];

	if (!file)
		return NULL;

	while (fgets(line, sizeof(line), file)) {
		char *eq = strchr(line, '=');

		if (!eq)
			continue;

		*eq = '\0';

		if (strcmp(line, key) == 0) {
			char *value = eq + 1;
			value[strcspn(value, "\r\n")] = '\0';

			char *result = strdup(value);
			fclose(file);

			return result;
		}
	}

	fclose(file);
	return NULL;
}
