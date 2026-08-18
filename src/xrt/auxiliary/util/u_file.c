// Copyright 2019-2025, Collabora, Ltd.
// Copyright 2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Very simple file opening functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Pete Black <pblack@collabora.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_windows.h"
#include "util/u_file.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_OSX
// For PATH_MAX
#include <sys/syslimits.h>
#endif

#if defined(XRT_OS_WINDOWS) && !defined(XRT_ENV_MINGW)
#define PATH_MAX 4096
#endif

#ifdef XRT_OS_LINUX
#include <sys/stat.h>
#include <linux/limits.h>

static int
mkpath(const char *path)
{
	char tmp[PATH_MAX];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp) - 1;
	if (tmp[len] == '/') {
		tmp[len] = 0;
	}

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, S_IRWXU) < 0 && errno != EEXIST) {
				return -1;
			}
			*p = '/';
		}
	}

	if (mkdir(tmp, S_IRWXU) < 0 && errno != EEXIST) {
		return -1;
	}

	return 0;
}

static bool
is_dir(const char *path)
{
	struct stat st = {0};
	if (!stat(path, &st)) {
		return S_ISDIR(st.st_mode);
	} else {
		return false;
	}
}

int
u_file_get_config_dir(char *out_path, size_t out_path_size)
{
	const char *xdg_home = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg_home != NULL) {
		return snprintf(out_path, out_path_size, "%s/monado", xdg_home);
	}
	if (home != NULL) {
		return snprintf(out_path, out_path_size, "%s/.config/monado", home);
	}
	return -1;
}

int
u_file_get_path_in_config_dir(const char *suffix, char *out_path, size_t out_path_size)
{
	char tmp[PATH_MAX];
	int i = u_file_get_config_dir(tmp, sizeof(tmp));
	if (i <= 0) {
		return i;
	}

	return snprintf(out_path, out_path_size, "%s/%s", tmp, suffix);
}

FILE *
u_file_open_file_in_config_dir(const char *filename, const char *mode)
{
	char tmp[PATH_MAX];
	int i = u_file_get_config_dir(tmp, sizeof(tmp));
	if (i <= 0) {
		return NULL;
	}

	char file_str[PATH_MAX + 15];
	i = snprintf(file_str, sizeof(file_str), "%s/%s", tmp, filename);
	if (i <= 0) {
		return NULL;
	}

	FILE *file = fopen(file_str, mode);
	if (file != NULL) {
		return file;
	}

	// Try creating the path.
	mkpath(tmp);

	// Do not report error.
	return fopen(file_str, mode);
}

FILE *
u_file_open_file_in_config_dir_subpath(const char *subpath, const char *filename, const char *mode)
{
	char tmp[PATH_MAX];
	int i = u_file_get_config_dir(tmp, sizeof(tmp));
	if (i < 0 || i >= (int)sizeof(tmp)) {
		return NULL;
	}

	char fullpath[PATH_MAX];
	i = snprintf(fullpath, sizeof(fullpath), "%s/%s", tmp, subpath);
	if (i < 0 || i >= (int)sizeof(fullpath)) {
		return NULL;
	}

	char file_str[PATH_MAX + 15];
	i = snprintf(file_str, sizeof(file_str), "%s/%s", fullpath, filename);
	if (i < 0 || i >= (int)sizeof(file_str)) {
		return NULL;
	}

	FILE *file = fopen(file_str, mode);
	if (file != NULL) {
		return file;
	}

	// Try creating the path.
	mkpath(fullpath);

	// Do not report error.
	return fopen(file_str, mode);
}

int
u_file_get_hand_tracking_models_dir(char *out_path, size_t out_path_size)
{
	const char *suffix = "/monado/hand-tracking-models";
	const char *xdg_data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	int ret = 0;

	if (xdg_data_home != NULL) {
		ret = snprintf(out_path, out_path_size, "%s%s", xdg_data_home, suffix);
		if (ret > 0 && is_dir(out_path)) {
			return ret;
		}
	}

	if (home != NULL) {
		ret = snprintf(out_path, out_path_size, "%s/.local/share%s", home, suffix);
		if (ret > 0 && is_dir(out_path)) {
			return ret;
		}
	}

	ret = snprintf(out_path, out_path_size, "/usr/local/share%s", suffix);
	if (ret > 0 && is_dir(out_path)) {
		return ret;
	}

	ret = snprintf(out_path, out_path_size, "/usr/share%s", suffix);
	if (ret > 0 && is_dir(out_path)) {
		return ret;
	}

	if (out_path_size > 0) {
		out_path[0] = '\0';
	}

	return ret;
}

#endif /* XRT_OS_LINUX */

int
u_file_get_runtime_dir(char *out_path, size_t out_path_size)
{
	const char *xdg_rt = getenv("XDG_RUNTIME_DIR");
	if (xdg_rt != NULL) {
		return snprintf(out_path, out_path_size, "%s", xdg_rt);
	}

	const char *xdg_cache = getenv("XDG_CACHE_HOME");
	if (xdg_cache != NULL) {
		return snprintf(out_path, out_path_size, "%s", xdg_cache);
	}

#ifdef XRT_OS_WINDOWS
#ifndef UNICODE     // If Unicode support is disabled, use ANSI functions directly into out_path
#ifdef GetTempPath2 // GetTempPath2 is only available on Windows 11 >= 22000, fallback to GetTempPath for older versions
	return (int)GetTempPath2A(out_path_size, out_path);
#else
	return (int)GetTempPathA(out_path_size, out_path);
#endif
#else
	WCHAR temp[MAX_PATH] = {0};
#ifdef GetTempPath2 // GetTempPath2 is only available on Windows 11 >= 22000, fallback to GetTempPath for older versions
	GetTempPath2W(sizeof(temp), temp);
#else               // GetTempPath2
	GetTempPathW(sizeof(temp), temp);
#endif
	return wcstombs(out_path, temp, out_path_size);
#endif // UNICODE
#else
	const char *cache = "~/.cache";
	return snprintf(out_path, out_path_size, "%s", cache);
#endif
}

int
u_file_get_path_in_runtime_dir(const char *suffix, char *out_path, size_t out_path_size)
{
	char tmp[PATH_MAX];
	int i = u_file_get_runtime_dir(tmp, sizeof(tmp));
	if (i <= 0) {
		return i;
	}

	return snprintf(out_path, out_path_size, "%s/%s", tmp, suffix);
}

char *
u_file_read_content(FILE *file, size_t *out_file_size)
{
	// Go to the end of the file.
	fseek(file, 0L, SEEK_END);
	size_t file_size = ftell(file);

	// Return back to the start of the file.
	fseek(file, 0L, SEEK_SET);

	char *buffer = (char *)calloc(file_size + 1, sizeof(char));
	if (buffer == NULL) {
		return NULL;
	}

	// Do the actual reading.
	size_t ret = fread(buffer, sizeof(char), file_size, file);
	if (ret != file_size) {
		free(buffer);
		return NULL;
	}

	if (out_file_size) {
		*out_file_size = file_size;
	}

	return buffer;
}

char *
u_file_read_content_from_path(const char *path, size_t *out_file_size)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		return NULL;
	}
	char *file_content = u_file_read_content(file, out_file_size);
	int ret = fclose(file);
	// We don't care about the return value since we're just reading
	(void)ret;

	// Either valid non-null or null
	return file_content;
}
