/*
 * Copyright (c) 2008, 2009  Nokia Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "fbdev.h"
#include "omap_sysfs.h"

static int sysfs_write(const char *path, const char *value, size_t len)
{
	int fd;
	ssize_t r;

	DebugF("omap/sysfs: writing '%s' to '%s'\n", value, path);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		xf86DrvMsg(0, X_WARNING,
			"omap/sysfs: can't open '%s' for writing: %d:%s\n",
			path, errno, strerror(errno));
		return fd;
	}

	for (;;) {
		r = write(fd, value, len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
		}
		break;
	}

	close(fd);

	if (r < 0) {
		xf86DrvMsg(0, X_WARNING,
			"omap/sysfs: can't write to '%s': %d:%s\n",
			path, errno, strerror(errno));
		return -1;
	}

	return 0;
}

static int sysfs_read(const char *path, char *value, size_t len)
{
	int fd;
	ssize_t r;

	DebugF("omap/sysfs: reading '%s'\n", path);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		xf86DrvMsg(0, X_WARNING,
			"omap/sysfs: can't open '%s' for reading: %d:%s\n",
			path, errno, strerror(errno));
		return fd;
	}

	for (;;) {
		r = read(fd, value, len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
		}
		break;
	}

	close(fd);

	if (r < 0) {
		xf86DrvMsg(0, X_WARNING,
			"omap/sysfs: can't read from '%s': %d:%s\n",
			path, errno, strerror(errno));
		return -1;
	}

	if (r > len - 1)
		r = len - 1;
	value[r] = '\0';
	if (r > 0 && value[r - 1] == '\n')
		value[r - 1] = '\0';

	DebugF("omap/sysfs: read '%s' from '%s'\n", value, path);

	return 0;
}

int dss2_write_str(const char *fmt, int index, const char *option,
		   const char *str)
{
	char path[PATH_MAX];
	int ret;

	snprintf(path, sizeof path, fmt, index, option);
	path[sizeof path - 1] = '\0';

	ret = sysfs_write(path, str, strlen(str) + 1);
	if (ret)
		return -1;

	return 0;
}

int dss2_read_str(const char *fmt, int index, const char *option,
		  char *str, size_t len)
{
	char path[PATH_MAX];
	int ret;

	snprintf(path, sizeof path, fmt, index, option);
	path[sizeof path - 1] = '\0';

	ret = sysfs_read(path, str, len);
	if (ret)
		return -1;

	return 0;
}

int dss2_read_int(const char *fmt, int index, const char *option,
		  int *ret_a)
{
	char buf[32];
	int ret;

	ret = dss2_read_str(fmt, index, option, buf, sizeof buf);
	if (ret)
		return -1;

	*ret_a = atoi(buf);

	return 0;
}

int dss2_write_int(const char *fmt, int index, const char *option, int a)
{
	char buf[32];
	int ret;

	ret = snprintf(buf, sizeof buf, "%d", a);
	if (ret >= sizeof buf)
		return -1;
	buf[sizeof buf - 1] = '\0';

	return dss2_write_str(fmt, index, option, buf);
}
