/*
 * Copyright (c) 2010  Nokia Corporation
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
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/fb.h>
#include "linux/omapfb.h"

#include "omap_sysfs.h"

#include "omap.h"

static const char dss2_overlay[] = "/sys/devices/platform/omapdss/overlay%d/%s";
static const char dss2_manager[] = "/sys/devices/platform/omapdss/manager%d/%s";
static const char dss2_display[] = "/sys/devices/platform/omapdss/display%d/%s";
static const char dss2_fb[] = "/sys/devices/platform/omapfb/graphics/fb%d/%s";

struct omap_fb {
	char name[32];

	/* for mem_idx */
	int idx;

	/* mmaped memory ptr/len */
	void *map_ptr;
	size_t map_len;

	/* fb layout */
	unsigned int w;
	unsigned int h;
	enum omap_format format;
	unsigned int num_buffers;

	/* memory alloc info */
	struct omapfb_mem_info mem_info;

	/* overlays scanning out of this fb */
	struct omap_overlay *ovls[3];
};

struct omap_overlay {
	char name[32];

	/* /dev/fb device node */
	int fd;

	/* for mem_idx and sysfs */
	int idx;

	/* fb device setup */
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;

	/* omap plane setup */
	struct omapfb_plane_info plane_info;

	/* fb from where this overlay scans out */
	struct omap_fb *fb;

	/* out to which this overlay sends it's output */
	struct omap_output *out;

	/*
	 * last requested source coordinates
	 * var can't be trusted during memory allocation changes.
	 */
	unsigned int sx, sy, sw, sh;

	unsigned int global_alpha;
	enum omap_mirror mirror;
	bool enabled;
};

struct omap_output {
	char name[32];

	/* for sysfs */
	int idx;

	/* overlays which are sending output to us */
	struct omap_overlay *ovls[3];

	enum omap_rotate rotate;
	enum omap_mirror mirror;
	enum omap_tv_standard tv_std;

	struct omapfb_color_key ckey;

	enum omap_output_update update;

	bool can_mirror;
	bool can_rotate;

	bool alpha_blending;
	bool enabled;
	enum omap_output_tear tear;
	uint32_t wss;
};

static void func_printf(const char *func,
			const char *fmt, ...)
{
	va_list ap;
	ErrorF("%s: ", func);
	va_start(ap, fmt);
	VErrorF(fmt, ap);
	va_end(ap);
}
#define eprintf(fmt ...) func_printf(__func__, fmt)
#ifdef DEBUG
#define dprintf(fmt ...) func_printf(__func__, fmt)
#else
#define dprintf(fmt ...) do { } while (0)
#endif

#undef ENTER
#undef LEAVE
#undef ERROR
#define ENTER() dprintf("ENTER\n")
#define LEAVE() dprintf("LEAVE\n")
#define ERROR() dprintf("ERROR\n")

static void func_printf_var(const char *func,
			    const char *title,
			    const struct fb_var_screeninfo *var)
{
	ErrorF("%s: %s fb_var_screeninfo\n"
		"   xres %u\n"
		"   yres %u\n"
		"   xres_virtual %u\n"
		"   yres_virtual %u\n"
		"   xoffset %u\n"
		"   yoffset %u\n"
		"   bits_per_pixel %u\n"
		"   grayscale %u\n"
		"   red.offset %u\n"
		"   red.length %u\n"
		"   red.msb_right %u\n"
		"   green.offset %u\n"
		"   green.length %u\n"
		"   green.msb_right %u\n"
		"   blue.offset %u\n"
		"   blue.length %u\n"
		"   blue.msb_right %u\n"
		"   transp.offset %u\n"
		"   transp.length %u\n"
		"   transp.msb_right %u\n"
		"   nonstd %u\n"
		"   activate %u\n"
		"   height %u\n"
		"   width %u\n"
		"   accel_flags %u\n"
		"   pixclock %u\n"
		"   left_margin %u\n"
		"   right_margin %u\n"
		"   upper_margin %u\n"
		"   lower_margin %u\n"
		"   hsync_len %u\n"
		"   vsync_len %u\n"
		"   sync %u\n"
		"   vmode %u\n"
		"   rotate %u\n",
		func, title,
		var->xres,
		var->yres,
		var->xres_virtual,
		var->yres_virtual,
		var->xoffset,
		var->yoffset,
		var->bits_per_pixel,
		var->grayscale,
		var->red.offset,
		var->red.length,
		var->red.msb_right,
		var->green.offset,
		var->green.length,
		var->green.msb_right,
		var->blue.offset,
		var->blue.length,
		var->blue.msb_right,
		var->transp.offset,
		var->transp.length,
		var->transp.msb_right,
		var->nonstd,
		var->activate,
		var->height,
		var->width,
		var->accel_flags,
		var->pixclock,
		var->left_margin,
		var->right_margin,
		var->upper_margin,
		var->lower_margin,
		var->hsync_len,
		var->vsync_len,
		var->sync,
		var->vmode,
		var->rotate);
}
#define eprintf_var(title, var) func_printf_var(__func__, title, var)
#ifdef DEBUG
#define dprintf_var(title, var) func_printf_var(__func__, title, var)
#else
#define dprintf_var(title, var) { } while (0)
#endif

static void func_printf_pi(const char *func,
			   const char *title,
			   const struct omapfb_plane_info *pi)
{
	ErrorF("%s: %s omapfb_plane_info\n"
		"   pos_x %u\n"
		"   pos_y %u\n"
		"   enabled %u\n"
		"   channel_out %u\n"
		"   mirror %u\n"
		"   mem_idx 0x%02x\n"
		"   out_width %u\n"
		"   out_height %u\n",
		func, title,
		pi->pos_x,
		pi->pos_y,
		pi->enabled,
		pi->channel_out,
		pi->mirror,
		pi->mem_idx,
		pi->out_width,
		pi->out_height);
}
#define eprintf_pi(title, pi) func_printf_pi(__func__, title, pi)
#ifdef DEBUG
#define dprintf_pi(title, pi) func_printf_pi(__func__, title, pi)
#else
#define dprintf_pi(title, pi) do { } while (0)
#endif

static void func_printf_mi(const char *func,
			   const char *title,
			   const struct omapfb_mem_info *mi)
{
	ErrorF("%s: %s omapfb_mem_info\n"
		"   size %u\n"
		"   type %u\n",
		func, title,
		mi->size,
		mi->type);
}
#define eprintf_mi(title, mi) func_printf_mi(__func__, title, mi)
#ifdef DEBUG
#define dprintf_mi(title, mi) func_printf_mi(__func__, title, mi)
#else
#define dprintf_mi(title, mi) do { } while (0)
#endif

static const struct {
	struct fb_bitfield red;
	struct fb_bitfield green;
	struct fb_bitfield blue;
	struct fb_bitfield transp;
	unsigned int bits_per_pixel;
	unsigned int nonstd;
} fb_formats[] = {
	[OMAP_FORMAT_XRGB4444] = {
		.red    = { .length =  4, .offset =  8, },
		.green  = { .length =  4, .offset =  4, },
		.blue   = { .length =  4, .offset =  0, },
		.bits_per_pixel = 16,
	},
	[OMAP_FORMAT_ARGB4444] = {
		.transp = { .length =  4, .offset = 12, },
		.red    = { .length =  4, .offset =  8, },
		.green  = { .length =  4, .offset =  4, },
		.blue   = { .length =  4, .offset =  0, },
		.bits_per_pixel = 16,
	},
	[OMAP_FORMAT_RGB565] = {
		.red    = { .length =  5, .offset = 11, },
		.green  = { .length =  6, .offset =  5, },
		.blue   = { .length =  5, .offset =  0, },
		.bits_per_pixel = 16,
	},
	[OMAP_FORMAT_RGB888] = {
		.red    = { .length =  8, .offset = 16, },
		.green  = { .length =  8, .offset =  8, },
		.blue   = { .length =  8, .offset =  0, },
		.bits_per_pixel = 24,
	},
	[OMAP_FORMAT_XRGB8888] = {
		.red    = { .length =  8, .offset = 16, },
		.green  = { .length =  8, .offset =  8, },
		.blue   = { .length =  8, .offset =  0, },
		.bits_per_pixel = 32,
	},
	[OMAP_FORMAT_ARGB8888] = {
		.transp = { .length =  8, .offset = 24, },
		.red    = { .length =  8, .offset = 16, },
		.green  = { .length =  8, .offset =  8, },
		.blue   = { .length =  8, .offset =  0, },
		.bits_per_pixel = 32,
	},
	[OMAP_FORMAT_RGBX8888] = {
		.red    = { .length =  8, .offset = 24, },
		.green  = { .length =  8, .offset = 16, },
		.blue   = { .length =  8, .offset =  8, },
		.bits_per_pixel = 32,
	},
	[OMAP_FORMAT_RGBA8888] = {
		.red    = { .length =  8, .offset = 24, },
		.green  = { .length =  8, .offset = 16, },
		.blue   = { .length =  8, .offset =  8, },
		.transp = { .length =  8, .offset =  0, },
		.bits_per_pixel = 32,
	},
	[OMAP_FORMAT_YUY2] = {
		.bits_per_pixel = 16,
		.nonstd = OMAPFB_COLOR_YUY422,
	},
	[OMAP_FORMAT_UYVY] = {
		.bits_per_pixel = 16,
		.nonstd = OMAPFB_COLOR_YUV422,
	},
};

static struct omap_overlay *fb_get_overlay(struct omap_fb *fb)
{
	int i;

	/* Ugh. Can only be done via an ovl :( */
	for (i = 0; i < ARRAY_SIZE(fb->ovls); i++) {
		if (!fb->ovls[i])
			continue;

		return fb->ovls[i];
	}

	return NULL;
}

static struct omap_overlay *output_get_overlay(struct omap_output *out)
{
	int i;

	/* Ugh. Can only be done via an ovl :( */
	for (i = 0; i < ARRAY_SIZE(out->ovls); i++) {
		if (!out->ovls[i])
			continue;

		return out->ovls[i];
	}

	return NULL;
}

static void fb_init(struct omap_fb *fb)
{
	ENTER();

	memset(fb, 0, sizeof *fb);

	LEAVE();
}

struct omap_fb *omap_fb_new(void)
{
	struct omap_fb *fb;

	ENTER();

	fb = malloc(sizeof *fb);
	if (!fb)
		goto error;

	fb_init(fb);

	LEAVE();
	return fb;

 error:
	ERROR();
	return NULL;
}

void omap_fb_del(struct omap_fb *fb)
{
	ENTER();

	assert(fb != NULL);

	free(fb);

	LEAVE();
}

bool omap_fb_open(struct omap_fb *fb,
		  const char *device)
{
	int idx;
	int fd;
	int r;
	struct omapfb_mem_info mem_info = { .size = 0 };

	ENTER();

	assert(fb != NULL);
	assert(device != NULL);

	dprintf(" fb = %s\n", device);

	if (sscanf(device, "/dev/fb%d", &idx) != 1)
		goto error;

	fd = open(device, O_RDWR | O_SYNC);
	if (fd < 0)
		goto error;

	/* This assumes omapfb mem_idx is 0 */

	r = ioctl(fd, OMAPFB_QUERY_MEM, &mem_info);
	if (r)
		goto error_close;

	close(fd);

	strncpy(fb->name, device, sizeof fb->name);
	fb->name[sizeof fb->name - 1] = '\0';
	fb->idx = idx;
	fb->mem_info = mem_info;

	LEAVE();
	return true;

 error_close:
	close(fd);
 error:
	ERROR();
	return false;
}

bool omap_fb_close(struct omap_fb *fb)
{
	ENTER();

	assert(fb != NULL);

	dprintf(" fb = %s\n", fb->name);

	fb_init(fb);

	LEAVE();
	return true;
}

static bool overlay_refresh(struct omap_overlay *ovl)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	int r;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	r = ioctl(ovl->fd, FBIOGET_VSCREENINFO, &var);
	if (r) {
		eprintf("ioctl(FBIOGET_VSCREENINFO) failed %d:%s\n",
			errno, strerror(errno));
		goto error;
	}

	r = ioctl(ovl->fd, FBIOGET_FSCREENINFO, &fix);
	if (r) {
		eprintf("ioctl(FBIOGET_FSCREENINFO) failed %d:%s\n",
			errno, strerror(errno));
		goto error;
	}
	ovl->var = var;
	ovl->fix = fix;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

static bool overlay_update(struct omap_overlay *ovl,
			   unsigned int w,
			   unsigned int h,
			   unsigned int num_buffers,
			   enum omap_format format)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	unsigned int sx, sy, sw, sh;
	int r;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	/*
	 * memory reallocation may have forced changes upon var,
	 * read it from the device instad of trusting the cached data.
	 */
	r = ioctl(ovl->fd, FBIOGET_VSCREENINFO, &var);
	if (r) {
		eprintf("ioctl(FBIOGET_FSCREENINFO) failed %d:%s\n",
			errno, strerror(errno));
		goto error;
	}
	ovl->var = var;

	switch (format) {
	case OMAP_FORMAT_XRGB4444:
	case OMAP_FORMAT_ARGB4444:
	case OMAP_FORMAT_RGB565:
	case OMAP_FORMAT_RGB888:
	case OMAP_FORMAT_XRGB8888:
	case OMAP_FORMAT_ARGB8888:
	case OMAP_FORMAT_RGBX8888:
	case OMAP_FORMAT_RGBA8888:
	case OMAP_FORMAT_YUY2:
	case OMAP_FORMAT_UYVY:
		var.red    = fb_formats[format].red;
		var.green  = fb_formats[format].green;
		var.blue   = fb_formats[format].blue;
		var.transp = fb_formats[format].transp;
		var.bits_per_pixel = fb_formats[format].bits_per_pixel;
		var.nonstd = fb_formats[format].nonstd;
		break;
	default:
		/* FIXME CLUT formats? */
		assert(0);
	}

	var.xres_virtual = w;
	var.yres_virtual = h * num_buffers;
	var.activate = FB_ACTIVATE_NOW;

	if (ovl->sx + ovl->sw > w) {
		sx = 0;
		sw = ovl->sw > w ? w : ovl->sw;
	} else {
		sx = ovl->sx;
		sw = ovl->sw;
	}
	if (ovl->sy + ovl->sh > h) {
		sy = 0;
		sh = ovl->sh > h ? h : ovl->sh;
	} else {
		sy = ovl->sy;
		sh = ovl->sh;
	}

	var.xoffset = sx;
	var.xres    = sw;
	var.yoffset = sy;
	var.yres    = sh;

	dprintf_var("putting", &var);

	r = ioctl(ovl->fd, FBIOPUT_VSCREENINFO, &var);
	if (r) {
		eprintf("ioctl(FBIOPUT_VSCREENINFO) failed %d:%s\n",
			errno, strerror(errno));
		eprintf_var("failed", &var);
		goto error;;
	}

	/* did the driver round it down? */
	if (var.xres < sw || var.yres < sh) {
		eprintf("var.xres or var.yres rounded down\n");
		eprintf_var("failed", &var);
		goto error_putv;
	}

	/* did the driver round it down? */
	if (var.xres_virtual < w || var.yres_virtual / num_buffers < h) {
		eprintf("var.xres_virtual or var.yres_virtual rounded down\n");
		eprintf_var("failed", &var);
		goto error_putv;
	}

	r = ioctl(ovl->fd, FBIOGET_FSCREENINFO, &fix);
	if (r) {
		eprintf("ioctl(FBIOGET_FSCREENINFO) failed %d:%s\n",
			errno, strerror(errno));
		goto error_putv;
	}

	ovl->var = var;
	ovl->fix = fix;

	LEAVE();
	return true;

 error_putv:
	ioctl(ovl->fd, FBIOPUT_VSCREENINFO, &ovl->var);
 error:
	ERROR();
	return false;
}

static bool fb_update_overlays(struct omap_fb *fb,
			       unsigned int w,
			       unsigned int h,
			       unsigned int num_buffers,
			       enum omap_format format)
{
	int i;

	ENTER();

	assert(fb != NULL);

	dprintf(" fb = %s\n", fb->name);

	for (i = 0; i < ARRAY_SIZE(fb->ovls); i++) {
		struct omap_overlay *ovl = fb->ovls[i];

		if (!ovl)
			continue;

		assert(ovl->fd >= 0);
		assert(ovl->fb == fb);

		/* FIXME should undo the changes */
		if (!overlay_update(ovl, w, h, num_buffers, format))
			goto error;
	}

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_fb_free(struct omap_fb *fb)
{
	int r;
	struct omapfb_mem_info mem_info;
	struct omap_overlay *ovl;

	ENTER();

	assert(fb != NULL);

	dprintf(" fb = %s\n", fb->name);

	/* Ugh. Can only be done via an ovl :( */
	ovl = fb_get_overlay(fb);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	mem_info = fb->mem_info;

	mem_info.size = 0;

	/* Skip SETUP_MEM if the size already matches */
	if (memcmp(&mem_info, &fb->mem_info, sizeof mem_info)) {
		dprintf_mi("setup", &mem_info);

		r = ioctl(ovl->fd, OMAPFB_SETUP_MEM, &mem_info);
		if (r) {
			eprintf("ioctl(OMAPFB_SETUP_MEM) failed %d:%s\n",
				errno, strerror(errno));
			eprintf_mi("failed", &mem_info);
			goto error;
		}
	}

	fb->mem_info = mem_info;
	fb->w = 0;
	fb->h = 0;
	fb->format = 0;
	fb->num_buffers = 0;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

static unsigned int gcd(unsigned int a, unsigned int b)
{
	while (b) {
		unsigned int tmp = b;
		b = a % b;
		a = tmp;
	}

	return a;
}

static unsigned int lcm(unsigned int a, unsigned int b)
{
	return a * b / gcd(a, b);
}

static unsigned int div_round_up(unsigned int num, unsigned int den)
{
	return (num + den - 1) / den;
}

/*
 * omapfb has a hardcoded 8x8 minimum for some reason.
 */
static void adjust_fb_size(unsigned int *w,
			   unsigned int *h)
{
	if (*w < 8)
		*w = 8;
	if (*h < 8)
		*h = 8;
}

/*
 * Takes 'format', 'pitch_align', and 'w' as paramters.
 * Calculates 'pitch' and adjusts 'w' to match it.
 */
static void align_for_format(enum omap_format format,
			     unsigned int pitch_align,
			     unsigned int *w,
			     unsigned int *pitch)
{
	unsigned int bpp;

	switch (format) {
	case OMAP_FORMAT_XRGB4444:
	case OMAP_FORMAT_ARGB4444:
	case OMAP_FORMAT_RGB565:
	case OMAP_FORMAT_YUY2:
	case OMAP_FORMAT_UYVY:
	case OMAP_FORMAT_RGB888:
	case OMAP_FORMAT_XRGB8888:
	case OMAP_FORMAT_ARGB8888:
	case OMAP_FORMAT_RGBX8888:
	case OMAP_FORMAT_RGBA8888:
		bpp = fb_formats[format].bits_per_pixel;
		break;
	default:
		assert(0);
	}

	switch (format) {
	case OMAP_FORMAT_YUY2:
	case OMAP_FORMAT_UYVY:
		pitch_align = lcm(pitch_align, 4);
		break;
	case OMAP_FORMAT_RGB888:
		pitch_align = lcm(pitch_align, 12);
		break;
	default:
		break;
	}

	/* FIXME should handle VRFB somehow... */

	*pitch = *w * bpp >> 3;
	*pitch = div_round_up(*pitch, pitch_align) * pitch_align;

	*w = (*pitch << 3) / bpp;
}

bool omap_fb_alloc(struct omap_fb *fb,
		   unsigned int w,
		   unsigned int h,
		   enum omap_format format,
		   unsigned int num_buffers,
		   unsigned int buffer_align,
		   unsigned int pitch_align)
{
	struct omap_overlay *ovl;
	int r;
	struct omapfb_mem_info mem_info;
	unsigned int pitch;

	ENTER();

	assert(fb != NULL);
	assert(w != 0);
	assert(h != 0);
	assert(num_buffers != 0);
	assert(buffer_align != 0);
	assert(pitch_align != 0);

	dprintf(" fb = %s\n", fb->name);

	/* Ugh. Can only be done via an ovl :( */
	ovl = fb_get_overlay(fb);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	mem_info = fb->mem_info;

	adjust_fb_size(&w, &h);

	align_for_format(format, pitch_align, &w, &pitch);

	/* No need to align when there's only one buffer */
	if (num_buffers == 1)
		buffer_align = 1;

	/*
	 * Modify h so that the buffer boundary
	 * is aligned to buffer_align bytes.
	 */
	buffer_align = lcm(pitch, buffer_align);
	h = div_round_up(pitch * h, buffer_align) * buffer_align / pitch;

	mem_info.size = pitch * h * num_buffers;

	/* Skip SETUP_MEM if the size already matches */
	if (memcmp(&mem_info, &fb->mem_info, sizeof mem_info)) {
		dprintf_mi("setup", &mem_info);

		r = ioctl(ovl->fd, OMAPFB_SETUP_MEM, &mem_info);
		if (r) {
			eprintf("ioctl(OMAPFB_SETUP_MEM) failed %d:%s\n",
				errno, strerror(errno));
			eprintf_mi("failed", &mem_info);
			goto error;
		}
	}

	if (!fb_update_overlays(fb, w, h, num_buffers, format))
		goto error_realloc;

	fb->mem_info = mem_info;
	fb->w = w;
	fb->h = h;
	fb->format = format;
	fb->num_buffers = num_buffers;

	LEAVE();
	return true;

 error_realloc:
	ioctl(ovl->fd, OMAPFB_SETUP_MEM, &fb->mem_info);
 error:
	ERROR();
	return false;
}

bool omap_fb_check_size(struct omap_fb *fb,
			unsigned int w,
			unsigned int h,
			enum omap_format format,
			unsigned int num_buffers,
			unsigned int buffer_align,
			unsigned int pitch_align)
{
	unsigned int pitch;

	assert(fb != NULL);
	assert(w != 0);
	assert(h != 0);
	assert(num_buffers != 0);
	assert(buffer_align != 0);
	assert(pitch_align != 0);

	adjust_fb_size(&w, &h);

	align_for_format(format, pitch_align, &w, &pitch);

	if (fb->w != w ||
	    fb->h != h ||
	    fb->format != format ||
	    fb->num_buffers != num_buffers ||
	    pitch % pitch_align)
		return false;

	/* No need to align when there's only one buffer */
	if (num_buffers != 1 &&
	    fb->mem_info.size / num_buffers % buffer_align)
		return false;

	return true;
}

static bool var_to_format(const struct fb_var_screeninfo *var,
			  enum omap_format *ret_format)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fb_formats); i++) {
		if (!memcmp(&var->red, &fb_formats[i].red,
			    sizeof var->red) &&
		    !memcmp(&var->green, &fb_formats[i].green,
			    sizeof var->green) &&
		    !memcmp(&var->blue, &fb_formats[i].blue,
			    sizeof var->blue) &&
		    !memcmp(&var->transp, &fb_formats[i].transp,
			    sizeof var->transp) &&
		    var->bits_per_pixel == fb_formats[i].bits_per_pixel &&
		    var->nonstd == fb_formats[i].nonstd) {
			*ret_format = i;
			return true;
		}
	}

	return false;
}

bool omap_fb_refresh_info(struct omap_fb *fb, struct omap_overlay *ovl)
{
	int i;
	int r;
	struct omapfb_mem_info mem_info;

	ENTER();

	assert(fb != NULL);

	dprintf(" fb = %s\n", fb->name);

	/* Ugh. Can only be done via an ovl :( */

	/* Check that the passed overlay is assigned to this fb */
	for (i = 0; i < ARRAY_SIZE(fb->ovls); i++) {
		if (fb->ovls[i] == ovl)
			break;
	}
	if (i == ARRAY_SIZE(fb->ovls))
		goto error;

	r = ioctl(ovl->fd, OMAPFB_QUERY_MEM, &mem_info);
	if (r)
		goto error;

	if (!overlay_refresh(ovl))
		goto error;

	if (!var_to_format(&ovl->var, &fb->format)) {
		dprintf_var("bad format", &ovl->var);
		goto error;
	}

	fb->mem_info = mem_info;
	fb->w = ovl->var.xres_virtual;
	fb->h = ovl->var.yres_virtual;
	fb->num_buffers = 1;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_fb_get_info(struct omap_fb *fb,
		      unsigned int *ret_w,
		      unsigned int *ret_h,
		      unsigned int *ret_pitch)
{
	struct omap_overlay *ovl;

	ENTER();

	assert(fb != NULL);

	dprintf(" fb = %s\n", fb->name);

	/* Ugh. Can only be done via an ovl :( */
	ovl = fb_get_overlay(fb);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	if (ret_w)
		*ret_w = fb->w;
	if (ret_h)
		*ret_h = fb->h;
	if (ret_pitch)
		*ret_pitch = ovl->fix.line_length;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_fb_get_size_info(struct omap_fb *fb,
			   unsigned int *ret_total,
			   unsigned int *ret_free,
			   unsigned int *ret_largest_free_block)
{
	struct omap_overlay *ovl;
	struct omapfb_vram_info vram_info = { .total = 0 };
	int r;

	ENTER();

	assert(fb != NULL);

	dprintf(" fb = %s\n", fb->name);

	/* Ugh. Can only be done via an ovl :( */
	ovl = fb_get_overlay(fb);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	r = ioctl(ovl->fd, OMAPFB_GET_VRAM_INFO, &vram_info);

	if (r) {
		eprintf("ioctl(OMAPFB_GET_VRAM_INFO) failed %d:%s\n",
			errno, strerror(errno));
		goto error;
	}

	if (ret_total)
		*ret_free = vram_info.total;
	if (ret_free)
		*ret_free = vram_info.free;
	if (ret_largest_free_block)
		*ret_largest_free_block = vram_info.largest_free_block;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

static bool overlay_set_output(struct omap_overlay *ovl,
			       struct omap_output *out)
{
	bool enabled;
	int r;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->idx >= 0);
	assert(out != NULL);
	assert(out->idx >= 0);

	/*
	 * FIXME should use the omapfb_plane_info.channel_out instead.
	 * Need to implement it in the kernel first...
	 */

	/*
	 * Argh. DSS2 doesn't allow live manager changes.
	 * Should be fixed. For now add this workaround.
	 */
	enabled = omap_overlay_enabled(ovl);
	if (enabled)
		omap_overlay_disable(ovl);

	r = dss2_write_str(dss2_overlay, ovl->idx,
			   "manager", out->name);
	if (r)
		goto error;

	if (enabled)
		omap_overlay_enable(ovl);

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

static bool overlay_set_fb(struct omap_overlay *ovl,
			   struct omap_fb *fb)
{
	int r = 0;
	struct omapfb_plane_info plane_info;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);
	assert(fb != NULL);

	dprintf(" ovl = %s\n", ovl->name);

	plane_info = ovl->plane_info;

	if (ovl->idx == fb->idx)
		plane_info.mem_idx = 0;
	else
		plane_info.mem_idx = OMAPFB_MEM_IDX_ENABLED | fb->idx;

	if (memcmp(&plane_info, &ovl->plane_info, sizeof plane_info)) {
		dprintf_pi("setup", &plane_info);
		r = ioctl(ovl->fd, OMAPFB_SETUP_PLANE, &plane_info);
	}
	if (r) {
		eprintf("ioctl(OMAPFB_SETUP_PLANE) failed %d:%s\n",
			errno, strerror(errno));
		eprintf_pi("failed", &plane_info);
		goto error;
	}

	if (fb->w && fb->h && fb->num_buffers &&
	    !overlay_update(ovl,
			    fb->w, fb->h,
			    fb->num_buffers,
			    fb->format))
		goto error_setup_plane;

	ovl->plane_info = plane_info;

	LEAVE();
	return true;

 error_setup_plane:
	ioctl(ovl->fd, OMAPFB_SETUP_PLANE, &ovl->plane_info);
 error:
	ERROR();
	return false;
}

bool omap_fb_assign_overlay(struct omap_fb *fb,
			    struct omap_overlay *ovl)
{
	int i;

	ENTER();

	assert(fb != NULL);
	assert(ovl != NULL);

	dprintf(" fb = %s\n", fb->name);
	dprintf(" ovl = %s\n", ovl->name);

	if (ovl->fb != NULL) {
		if (ovl->fb != fb) {
			struct omap_fb *old_fb = ovl->fb;

			/* Remove the old_fb<->ovl assiciation  */
			for (i = 0; i < ARRAY_SIZE(old_fb->ovls); i++) {
				if (old_fb->ovls[i] != ovl)
					continue;

				old_fb->ovls[i] = NULL;
				ovl->fb = NULL;
				break;
			}
			assert(i != ARRAY_SIZE(old_fb->ovls));
		} else {
			/* Just sanity check that everything is OK */
			for (i = 0; i < ARRAY_SIZE(fb->ovls); i++) {
				if (fb->ovls[i] == ovl) {
					assert(ovl->fb == fb);
					LEAVE();
					return true;
				}
			}
			assert(0);
		}
	}

	assert(ovl->fb == NULL);

	/* Make the new fb<->ovl association */
	for (i = 0; i < ARRAY_SIZE(fb->ovls); i++) {
		if (fb->ovls[i] != NULL)
			continue;

		if (!overlay_set_fb(ovl, fb))
			goto error;

		fb->ovls[i] = ovl;
		ovl->fb = fb;

		LEAVE();
		return true;
	}

	/* no room for this ovl? */
	/* should not happen */
	assert(0);

 error:
	ERROR();
	return false;
}

bool omap_output_assign_overlay(struct omap_output *out,
				struct omap_overlay *ovl)
{
	int i;

	ENTER();

	assert(out != NULL);
	assert(ovl != NULL);

	dprintf(" out = %s\n", out->name);
	dprintf(" ovl = %s\n", ovl->name);

	if (ovl->out != NULL) {
		if (ovl->out != out) {
			struct omap_output *old_out = ovl->out;

			/* Remove the old_out<->ovl assiciation  */
			for (i = 0; i < ARRAY_SIZE(old_out->ovls); i++) {
				if (old_out->ovls[i] != ovl)
					continue;

				old_out->ovls[i] = NULL;
				ovl->out = NULL;
				break;
			}
			assert(i != ARRAY_SIZE(old_out->ovls));
		} else {
			/* Just sanity check that everything is OK */
			for (i = 0; i < ARRAY_SIZE(out->ovls); i++) {
				if (out->ovls[i] == ovl) {
					assert(ovl->out == out);
					LEAVE();
					return true;
				}
			}
			assert(0);
		}
	}

	assert(ovl->out == NULL);

	/* Make the new out<->ovl association */
	for (i = 0; i < ARRAY_SIZE(out->ovls); i++) {
		if (out->ovls[i] != NULL)
			continue;

		if (!overlay_set_output(ovl, out))
			/* FIXME undo the damage */
			goto error;

		out->ovls[i] = ovl;
		ovl->out = out;

		LEAVE();
		return true;
	}

	/* no room for this ovl? */
	/* should not happen */
	assert(0);

 error:
	ERROR();
	return false;
}

static void overlay_init(struct omap_overlay *ovl)
{
	ENTER();

	assert(ovl != NULL);

	memset(ovl, 0, sizeof *ovl);
	ovl->fd = -1;
	ovl->idx = -1;

	LEAVE();
}

struct omap_overlay *omap_overlay_new(void)
{
	struct omap_overlay *ovl;

	ENTER();

	ovl = malloc(sizeof *ovl);
	if (!ovl)
		goto error;

	overlay_init(ovl);

	LEAVE();
	return ovl;

 error:
	ERROR();
	return NULL;
}

void omap_overlay_del(struct omap_overlay *ovl)
{
	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd < 0);

	free(ovl);

	LEAVE();
}

bool omap_overlay_open(struct omap_overlay *ovl,
		       const char *device,
		       const char *name)
{
	char buf[8];
	int idx;
	int fd;
	int r;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	struct omapfb_plane_info plane_info = { .pos_x = 0 };
	int global_alpha;
	int mirror;

	ENTER();

	assert(ovl != NULL);
	assert(device != NULL);

	assert(ovl->fd < 0);

	if (sscanf(device, "/dev/fb%d", &idx) != 1)
		goto error;

	if (dss2_read_str(dss2_overlay, idx, "name", buf, sizeof buf))
		goto error;

	if (strcmp(name, buf))
		goto error;

	fd = open(device, O_RDWR | O_SYNC);
	if (fd < 0)
		goto error;

	r = ioctl(fd, FBIOGET_VSCREENINFO, &var);
	if (r)
		goto error_close;

	r = ioctl(fd, FBIOGET_FSCREENINFO, &fix);
	if (r)
		goto error_close;

	r = dss2_read_int(dss2_overlay, idx, "global_alpha", &global_alpha);
	if (r)
		goto error_close;

	r = dss2_read_int(dss2_fb, idx, "mirror", &mirror);
	if (r)
		goto error_close;

	r = ioctl(fd, OMAPFB_QUERY_PLANE, &plane_info);
	if (r)
		goto error_close;

	/* Disable the overlay initially */
	plane_info.enabled = 0;
	plane_info.mem_idx = 0;

	dprintf_pi("setup", &plane_info);

	r = ioctl(fd, OMAPFB_SETUP_PLANE, &plane_info);
	if (r) {
		eprintf("ioctl(OMAPFB_SETUP_PLANE) failed %d:%s\n",
			errno, strerror(errno));
		eprintf_pi("failed", &plane_info);
		goto error_close;
	}

	strncpy(ovl->name, device, sizeof ovl->name);
	ovl->name[sizeof ovl->name - 1] = '\0';
	ovl->fd = fd;
	ovl->idx = idx;
	ovl->var = var;
	ovl->fix = fix;
	ovl->plane_info = plane_info;
	ovl->global_alpha = global_alpha;
	ovl->mirror = mirror;

	LEAVE();
	return true;

 error_close:
	close(fd);
 error:
	ERROR();
	return false;
}

bool omap_overlay_close(struct omap_overlay *ovl)
{
	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	close(ovl->fd);

	overlay_init(ovl);

	LEAVE();
	return true;
}

unsigned int omap_overlay_id(struct omap_overlay *ovl)
{
	unsigned int id;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	id = ovl->idx;

	LEAVE();

	return id;
}

bool omap_fb_map(struct omap_fb *fb,
		 void **ret_ptr, size_t *ret_len)
{
	struct omap_overlay *ovl;
	void *data;

	ENTER();

	assert(fb != NULL);
	assert(fb->map_ptr == NULL);
	assert(fb->map_len == 0);
	assert(ret_ptr != NULL);
	assert(ret_len != NULL);

	dprintf(" fb = %s\n", fb->name);

	/* Ugh. Can only be done via an ovl :( */
	ovl = fb_get_overlay(fb);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);
	assert(ovl->fix.smem_len != 0);

	dprintf(" ovl = %s\n", ovl->name);

	data = mmap(NULL, ovl->fix.smem_len,
		    PROT_READ | PROT_WRITE, MAP_SHARED, ovl->fd, 0);
	if (data == MAP_FAILED)
		goto error;

	*ret_ptr = fb->map_ptr = data;
	*ret_len = fb->map_len = ovl->fix.smem_len;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

void omap_fb_unmap(struct omap_fb *fb)
{
	ENTER();

	assert(fb != NULL);
	assert(fb->map_ptr != NULL);
	assert(fb->map_len != 0);

	dprintf(" fb = %s\n", fb->name);

	munmap(fb->map_ptr, fb->map_len);

	fb->map_ptr = NULL;
	fb->map_len = 0;

	LEAVE();
}

bool omap_overlay_pan(struct omap_overlay *ovl,
			unsigned int buffer,
			unsigned int sx, unsigned int sy,
			unsigned int sw, unsigned int sh)
{
	struct omap_fb *fb;
	int r = 0;
	struct fb_var_screeninfo var;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);
	assert(ovl->fb != NULL);

	dprintf(" ovl = %s\n", ovl->name);

	fb = ovl->fb;

	assert(fb->idx >= 0);

	dprintf(" fb = %s\n", fb->name);

	var = ovl->var;

	var.xres = sw;
	var.yres = sh;

	var.xoffset = sx;
	var.yoffset = sy + fb->h * buffer;

	if (!memcmp(&var, &ovl->var, sizeof var))
		goto done;

	var.activate = FB_ACTIVATE_NOW;

	dprintf_var("panning", &var);
	r = ioctl(ovl->fd, FBIOPAN_DISPLAY, &var);

	if (r) {
		eprintf("ioctl(FBIOPAN_DISPLAY) failed %d:%s\n",
			errno, strerror(errno));
		eprintf_var("failed", &var);
		goto error_pan;
	}

	/* did the driver round it down? */
	if (var.xres < sw || var.yres < sh) {
		eprintf("var.xres or var.yres rounded down\n");
		eprintf_var("failed", &var);
		goto error_pan;
	}

	ovl->sx = var.xoffset;
	ovl->sy = var.yoffset;
	ovl->sw = var.xres;
	ovl->sh = var.yres;

	ovl->var = var;

 done:
	LEAVE();
	return true;

 error_pan:
	ioctl(ovl->fd, FBIOPUT_VSCREENINFO, &ovl->var);
	ERROR();
	return false;
}

bool omap_overlay_setup(struct omap_overlay *ovl,
			unsigned int buffer,
			unsigned int sx, unsigned int sy,
			unsigned int sw, unsigned int sh,
			unsigned int dx, unsigned int dy,
			unsigned int dw, unsigned int dh,
			enum omap_mirror mirror,
			enum omap_rotate rotate)
{
	struct omap_fb *fb;
	int r = 0;
	struct fb_var_screeninfo var;
	struct omapfb_plane_info plane_info;
	bool put_var = false;
	bool pan_display = false;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);
	assert(ovl->fb != NULL);

	dprintf(" ovl = %s\n", ovl->name);

	fb = ovl->fb;

	assert(fb->idx >= 0);

	dprintf(" fb = %s\n", fb->name);

	var = ovl->var;
	plane_info = ovl->plane_info;

	var.xres = sw;
	var.yres = sh;

	/* vert mirror == 180 degree rotation + horz mirror */
	if (mirror & OMAP_MIRROR_VERT) {
		rotate = (rotate + 2) & 3;
		mirror ^= OMAP_MIRROR_BOTH;
	}

	switch (rotate) {
	case OMAP_ROTATE_0:
		var.rotate = FB_ROTATE_UR;
		break;
	case OMAP_ROTATE_90:
		var.rotate = FB_ROTATE_CW;
		break;
	case OMAP_ROTATE_180:
		var.rotate = FB_ROTATE_UD;
		break;
	case OMAP_ROTATE_270:
		var.rotate = FB_ROTATE_CCW;
		break;
	default:
		assert(0);
	}

	if (memcmp(&var, &ovl->var, sizeof var))
		put_var = true;

	var.xoffset = sx;
	var.yoffset = sy + fb->h * buffer;

	if (!put_var && memcmp(&var, &ovl->var, sizeof var))
		pan_display = true;

	var.activate = FB_ACTIVATE_NOW;

	if (put_var) {
		/*
		 * FBIOPUT_VSCREENINFO's overwrites the overlay destination
		 * size for non-scaled overlays, so we may have to move it
		 * first to make enough room for the new size.
		 */
		if (plane_info.pos_x > dx)
			plane_info.pos_x = dx;
		if (plane_info.pos_y > dy)
			plane_info.pos_y = dy;

		if (memcmp(&plane_info, &ovl->plane_info, sizeof plane_info)) {
			dprintf_pi("setup", &plane_info);
			r = ioctl(ovl->fd, OMAPFB_SETUP_PLANE, &plane_info);
		}
		if (r) {
			eprintf("ioctl(OMAPFB_SETUP_PLANE) failed %d:%s\n",
				errno, strerror(errno));
			eprintf_pi("failed", &plane_info);
			goto error;
		}

		dprintf_var("putting", &var);
		r = ioctl(ovl->fd, FBIOPUT_VSCREENINFO, &var);
	} else if (pan_display) {
		dprintf_var("panning", &var);
		r = ioctl(ovl->fd, FBIOPAN_DISPLAY, &var);
	} else {
		dprintf_var("unchanged", &var);
	}
	if (r) {
		eprintf("ioctl(%s) failed %d:%s\n",
			put_var ? "FBIOPUT_VSCREENINFO" : "FBIOPAN_DISPLAY",
			errno, strerror(errno));
		eprintf_var("failed", &var);
		goto error_move_plane;
	}

	/* did the driver round it down? */
	if (var.xres < sw || var.yres < sh) {
		eprintf("var.xres or var.yres rounded down\n");
		eprintf_var("failed", &var);
		goto error_putv;
	}

	plane_info.enabled = ovl->enabled && dw && dh;
	plane_info.pos_x = dx;
	plane_info.pos_y = dy;
	plane_info.out_width = dw;
	plane_info.out_height = dh;

	if (memcmp(&plane_info, &ovl->plane_info, sizeof plane_info)) {
		dprintf_pi("setup", &plane_info);
		r = ioctl(ovl->fd, OMAPFB_SETUP_PLANE, &plane_info);
	}
	if (r) {
		eprintf("ioctl(OMAPFB_SETUP_PLANE) failed %d:%s\n",
			errno, strerror(errno));
		eprintf_pi("failed", &plane_info);
		goto error_putv;
	}

#if 1
	if (mirror != ovl->mirror) {
		/*
		 * FIXME should use the omapfb_plane_info.mirror instead.
		 * Need to implement it in the kernel first...
		 */
		r = dss2_write_int(dss2_fb, ovl->idx,
				   "mirror", !!(mirror & OMAP_MIRROR_HORZ));
		if (r) {
			eprintf("write to 'mirror' failed\n");
			goto error_setup_plane;
		}
	}
#endif

	ovl->sx = var.xoffset;
	ovl->sy = var.yoffset;
	ovl->sw = var.xres;
	ovl->sh = var.yres;

	ovl->var = var;
	ovl->plane_info = plane_info;
	ovl->mirror = mirror;

	LEAVE();
	return true;

#if 1
 error_setup_plane:
	ioctl(ovl->fd, OMAPFB_SETUP_PLANE, &ovl->plane_info);
#endif
 error_putv:
	ioctl(ovl->fd, FBIOPUT_VSCREENINFO, &ovl->var);
 error_move_plane:
	ioctl(ovl->fd, OMAPFB_SETUP_PLANE, &ovl->plane_info);
 error:
	ERROR();
	return false;
}

struct omap_output *omap_overlay_get_output(struct omap_overlay *ovl)
{
	return ovl->out;
}

struct omap_fb *omap_overlay_get_fb(struct omap_overlay *ovl)
{
	return ovl->fb;
}

bool omap_overlay_enabled(struct omap_overlay *ovl)
{
	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	LEAVE();
	return ovl->enabled;
}

bool omap_overlay_wait(struct omap_overlay *ovl)
{
	int timeout = 5;
	int r;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	do {
		r = ioctl(ovl->fd, OMAPFB_WAITFORGO);
	} while (r && errno == EINTR && --timeout > 0);
	if (r && errno == EINTR)
		eprintf(" wait for overlay timed out\n");
	if (r)
		goto error;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_overlay_enable(struct omap_overlay *ovl)
{
	int r;
	struct omapfb_plane_info plane_info;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	if (ovl->enabled || ovl->plane_info.enabled)
		goto done;

	/* Do not actually enable while invisible */
	if (!ovl->plane_info.out_width || !ovl->plane_info.out_height)
		goto done;

	plane_info = ovl->plane_info;

	plane_info.enabled = 1;

	dprintf_pi("setup", &plane_info);
	r = ioctl(ovl->fd, OMAPFB_SETUP_PLANE, &plane_info);
	if (r) {
		eprintf("ioctl(OMAPFB_SETUP_PLANE) failed %d:%s\n",
			errno, strerror(errno));
		eprintf_pi("failed", &plane_info);
		goto error;
	}

	ovl->plane_info = plane_info;

 done:
	ovl->enabled = true;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_overlay_disable(struct omap_overlay *ovl)
{
	int r;
	struct omapfb_plane_info plane_info;

	ENTER();

	assert(ovl != NULL);
	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	if (!ovl->enabled || !ovl->plane_info.enabled)
		goto done;

	plane_info = ovl->plane_info;

	plane_info.enabled = 0;

	dprintf_pi("setup", &plane_info);
	r = ioctl(ovl->fd, OMAPFB_SETUP_PLANE, &plane_info);
	if (r) {
		eprintf("ioctl(OMAPFB_SETUP_PLANE) failed %d:%s\n",
			errno, strerror(errno));
		eprintf_pi("failed", &plane_info);
		goto error;
	}

	ovl->plane_info = plane_info;

 done:
	ovl->enabled = false;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_overlay_global_alpha(struct omap_overlay *ovl,
			       unsigned int alpha)
{
	ENTER();

	assert(alpha < 256);
	assert(ovl != NULL);
	assert(ovl->idx >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	if (ovl->global_alpha == alpha)
		goto done;

	if (dss2_write_int(dss2_overlay, ovl->idx, "global_alpha", alpha))
		goto error;

	ovl->global_alpha = alpha;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

static void output_init(struct omap_output *out)
{
	ENTER();

	memset(out, 0, sizeof *out);
	out->idx = -1;

	LEAVE();
}

struct omap_output *omap_output_new(void)
{
	struct omap_output *out;

	ENTER();

	out = malloc(sizeof *out);
	if (!out)
		goto error;

	output_init(out);

	LEAVE();
	return out;

 error:
	ERROR();
	return NULL;
}

void omap_output_del(struct omap_output *out)
{
	ENTER();

	assert(out != NULL);
	assert(out->idx < 0);

	free(out);

	LEAVE();
}

bool omap_output_open(struct omap_output *out,
		      const char *name)
{
	int idx;
	int r;
	char buf[64];
	enum omap_tv_standard tv_std = OMAP_TV_STANDARD_PAL;
	int mirror = 0;
	int rotate = 0;
	bool can_rotate;
	bool can_mirror;
	int alpha_blending;
	int enabled;
	int tear_elim;
	int update_mode;

	ENTER();

	assert(out != NULL);
	assert(name != NULL);
	assert(out->idx < 0);

	dprintf(" out = %s\n", name);

	for (idx = 0; idx < 2; idx++) {
		r = dss2_read_str(dss2_display, idx, "name", buf, sizeof buf);
		if (r)
			continue;

		if (!strcmp(name, buf))
			break;
	}

	if (idx == 2)
		goto error;

	if (!strcmp(name, "tv")) {
		r = dss2_read_str(dss2_display, idx,
				  "timings", buf, sizeof buf);
		if (r)
			goto error;
		if (strstr(buf, ",574/"))
			tv_std = OMAP_TV_STANDARD_PAL;
		else if (strstr(buf, ",482/"))
			tv_std = OMAP_TV_STANDARD_NTSC;
		else
			goto error;
	}

	r = dss2_read_int(dss2_display, idx, "mirror", &mirror);
	if (r && errno != ENOENT)
		goto error;
	can_mirror = !r;

	r = dss2_read_int(dss2_display, idx, "rotate", &rotate);
	if (r && errno != ENOENT)
		goto error;
	can_rotate = !r;

	r = dss2_read_int(dss2_manager, idx,
			  "alpha_blending_enabled", &alpha_blending);
	if (r)
		goto error;

	r = dss2_read_int(dss2_display, idx, "enabled", &enabled);
	if (r)
		goto error;

	r = dss2_read_int(dss2_display, idx, "tear_elim", &tear_elim);
	if (r)
		goto error;

	r = dss2_read_int(dss2_display, idx, "update_mode", &update_mode);
	if (r)
		goto error;

	strncpy(out->name, name, sizeof out->name);
	out->name[sizeof out->name - 1] = '\0';

	out->idx = idx;
	out->tv_std = tv_std;
	out->rotate = rotate;
	out->mirror = mirror;
	out->can_mirror = can_mirror;
	out->can_rotate = can_rotate;
	out->alpha_blending = alpha_blending;
	out->enabled = enabled;
	out->tear = tear_elim ? OMAP_OUTPUT_TEAR_SYNC : OMAP_OUTPUT_TEAR_NONE;
	out->update = update_mode == 2 ?
		OMAP_OUTPUT_UPDATE_MANUAL : OMAP_OUTPUT_UPDATE_AUTO;
	out->wss = 0xffffffff; /* kernel may reset WSS */

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_close(struct omap_output *out)
{
	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	output_init(out);

	LEAVE();
	return true;
}

bool omap_output_enable(struct omap_output *out)
{
	int r;

	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	if (out->enabled)
		goto done;

	r = dss2_write_int(dss2_display, out->idx, "enabled", 1);
	if (r)
		goto error;

	out->enabled = true;
	out->wss = 0xffffffff; /* kernel may reset WSS */

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_disable(struct omap_output *out)
{
	int r;

	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	if (!out->enabled)
		goto done;

	r = dss2_write_int(dss2_display, out->idx, "enabled", 0);
	if (r)
		goto error;

	out->enabled = false;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_setup(struct omap_output *out,
		       enum omap_mirror mirror,
		       enum omap_rotate rotate)
{
	int r;

	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	/* vert mirror == 180 degree rotation + horz mirror */
	if (mirror & OMAP_MIRROR_VERT) {
		rotate = (rotate + 2) & 3;
		mirror ^= OMAP_MIRROR_BOTH;
	}

	if (mirror & OMAP_MIRROR_HORZ && !out->can_mirror)
		goto error;
	if (rotate != OMAP_ROTATE_0 && !out->can_rotate)
		goto error;


	if (out->can_rotate && rotate != out->rotate) {
		r = dss2_write_int(dss2_display, out->idx, "rotate", rotate);
		if (r)
			goto error;
	}

	if (out->can_mirror && mirror != out->mirror) {
		r = dss2_write_int(dss2_display, out->idx,
				   "mirror", !!(mirror & OMAP_MIRROR_HORZ));
		if (r)
			goto error_rotate;
	}

	out->rotate = rotate;
	out->mirror = mirror;

	LEAVE();
	return true;

 error_rotate:
	if (out->can_rotate)
		dss2_write_int(dss2_display, out->idx, "rotate", out->rotate);
 error:
	ERROR();
	return false;
}

bool omap_output_get_setup(struct omap_output *out,
			   enum omap_mirror *ret_mirror,
			   enum omap_rotate *ret_rotate)
{
	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	if (ret_rotate)
		*ret_rotate = out->rotate;
	if (ret_mirror)
		*ret_mirror = out->mirror;

	LEAVE();
	return true;
}

bool omap_output_check(struct omap_output *out,
		       enum omap_mirror mirror,
		       enum omap_rotate rotate)
{
	ENTER();

	assert(out != NULL);

	dprintf(" out = %s\n", out->name);

	/* vert mirror == 180 degree rotation + horz mirror */
	if (mirror & OMAP_MIRROR_VERT) {
		rotate = (rotate + 2) & 3;
		mirror ^= OMAP_MIRROR_BOTH;
	}

	if (mirror & OMAP_MIRROR_HORZ && !out->can_mirror)
		goto error;
	if (rotate != OMAP_ROTATE_0 && !out->can_rotate)
		goto error;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_wss(struct omap_output *out,
		     uint32_t wss)
{
	int r;

	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	if (out->wss == wss)
		goto done;

	r = dss2_write_int(dss2_display, out->idx, "wss", wss);
	if (r)
		goto error;

	out->wss = wss;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_tv_standard(struct omap_output *out,
			     enum omap_tv_standard std)
{
	int r;
	const char *timings;

	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	if (strcmp(out->name, "tv"))
		goto error;

	switch (std) {
	case OMAP_TV_STANDARD_PAL:
		timings = "pal";
		break;
	case OMAP_TV_STANDARD_NTSC:
		timings = "ntsc";
		break;
	default:
		assert(0);
	}

	if (out->tv_std == std)
		goto done;

	r = dss2_write_str(dss2_display, out->idx,
			   "timings", timings);
	if (r)
		goto error;

	out->tv_std = std;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool
omap_output_set_tearsync(struct omap_output *out,
			 enum omap_output_tear tear)
{
	struct omapfb_tearsync_info tearsync_info = { .enabled = 0 };
	struct omap_overlay *ovl;
	int r;

	ENTER();

	assert(out);

	if (out->tear == tear)
		goto done;

	ovl = output_get_overlay(out);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	tearsync_info.enabled = tear == OMAP_OUTPUT_TEAR_SYNC;

	dprintf(" ovl = %s, tearsync = %d\n", ovl->name, tearsync_info.enabled);

	r = ioctl(ovl->fd, OMAPFB_SET_TEARSYNC, &tearsync_info);
	if (r)
		goto error;

	out->tear = tear;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;


}

bool omap_output_set_update_mode(struct omap_output *out,
				 enum omap_output_update update)
{
	int r;
	struct omap_overlay *ovl;
	enum omapfb_update_mode update_mode =
		update == OMAP_OUTPUT_UPDATE_MANUAL ?
		OMAPFB_MANUAL_UPDATE : OMAPFB_AUTO_UPDATE;

	ENTER();

	assert(out != NULL);

	dprintf(" out = %s\n", out->name);

	if (out->update == update)
		goto done;

	/* Ugh. Can only be done via an ovl :( */
	ovl = output_get_overlay(out);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	r = ioctl(ovl->fd, OMAPFB_SET_UPDATE_MODE, &update_mode);
	if (r)
		goto error;

	out->update = update;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_get_update_mode(struct omap_output *out,
				 enum omap_output_update *ret_update)
{
	ENTER();

	assert(out != NULL);

	dprintf(" out = %s\n", out->name);

	if (ret_update)
		*ret_update = out->update;

	LEAVE();
	return true;
}

bool omap_output_get_caps(struct omap_output *out,
			  enum omap_output_update *ret_update,
			  enum omap_output_tear *ret_tear)
{
	int r;
	struct omapfb_caps caps = { .ctrl = 0 };
	struct omap_overlay *ovl;

	ENTER();

	assert(out != NULL);

	dprintf(" out = %s\n", out->name);

	/* Ugh. Can only be done via an ovl :( */
	ovl = output_get_overlay(out);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	r = ioctl(ovl->fd, OMAPFB_GET_CAPS, &caps);
	if (r)
		goto error;

	if (ret_update)
		*ret_update = (caps.ctrl & OMAPFB_CAPS_MANUAL_UPDATE) ?
			OMAP_OUTPUT_UPDATE_MANUAL : OMAP_OUTPUT_UPDATE_AUTO;
	if (ret_tear)
		*ret_tear = (caps.ctrl & OMAPFB_CAPS_TEARSYNC) ?
			OMAP_OUTPUT_TEAR_SYNC : OMAP_OUTPUT_TEAR_NONE;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool
omap_output_update(struct omap_output *out,
		   unsigned int x,
		   unsigned int y,
		   unsigned int w,
		   unsigned int h)
{
	int r;
	struct omapfb_update_window update_window = {
		.x = x,
		.y = y,
		.width = w,
		.height = h,
		.out_x = x,
		.out_y = y,
		.out_width = w,
		.out_height = h,
	};
	struct omap_overlay *ovl;

	ENTER();

	assert(out != NULL);

	dprintf(" out = %s\n", out->name);

	if (out->update != OMAP_OUTPUT_UPDATE_MANUAL)
		goto done;

	/* Ugh. Can only be done via an ovl :( */
	ovl = output_get_overlay(out);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	r = ioctl(ovl->fd, OMAPFB_UPDATE_WINDOW, &update_window);
	if (r)
		goto error;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_wait_update(struct omap_output *out)
{
	int timeout = 5;
	int r;
	struct omap_overlay *ovl;

	ENTER();

	assert(out != NULL);

	dprintf(" out = %s\n", out->name);

	if (out->update != OMAP_OUTPUT_UPDATE_MANUAL)
		goto done;

	/* Ugh. Can only be done via an ovl :( */
	ovl = output_get_overlay(out);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	do {
		r = ioctl(ovl->fd, OMAPFB_SYNC_GFX);
	} while (r && errno == EINTR && --timeout > 0);
	if (r && errno == EINTR)
		eprintf(" wait for output timed out\n");
	if (r)
		goto error;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_color_key(struct omap_output *out,
			   bool enable, uint32_t colorkey)
{
	int r;
	struct omapfb_color_key ckey;
	struct omap_overlay *ovl;

	ENTER();

	assert(out != NULL);

	dprintf(" out = %s\n", out->name);

	/* Ugh. Can only be done via an ovl :( */
	ovl = output_get_overlay(out);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	ckey = out->ckey;

	/* FIXME go through sysfs instead to avoid the need for an ovl? */
	ckey.trans_key = colorkey;
	ckey.key_type = enable ?
		OMAPFB_COLOR_KEY_GFX_DST : OMAPFB_COLOR_KEY_DISABLED;

	if (!memcmp(&ckey, &out->ckey, sizeof ckey))
		goto done;

	r = ioctl(ovl->fd, OMAPFB_SET_COLOR_KEY, &ckey);
	if (r)
		goto error;

	out->ckey = ckey;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_alpha_blending(struct omap_output *out, bool enable)
{
	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	if (out->alpha_blending == enable)
		goto done;

	if (dss2_write_int(dss2_manager, out->idx,
			   "alpha_blending_enabled", enable))
		goto error;

	out->alpha_blending = enable;

 done:
	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

static bool must_swap_timings(enum omap_rotate rotate,
			      unsigned int xres,
			      unsigned int yres,
			      unsigned int hdisp,
			      unsigned int vdisp)
{
	switch (rotate) {
	case OMAP_ROTATE_0:
	case OMAP_ROTATE_180:
		return xres == vdisp && yres == hdisp;
	case OMAP_ROTATE_90:
	case OMAP_ROTATE_270:
		return xres == hdisp && yres == vdisp;
	default:
		assert(0);
	}
}

bool omap_output_get_timings(struct omap_output *out,
			     unsigned int *ret_pixclk,
			     unsigned int *ret_hdisp,
			     unsigned int *ret_hfp,
			     unsigned int *ret_hsw,
			     unsigned int *ret_hbp,
			     unsigned int *ret_vdisp,
			     unsigned int *ret_vfp,
			     unsigned int *ret_vsw,
			     unsigned int *ret_vbp)
{
	unsigned int xres, yres;
	char buf[64];

	ENTER();

	assert(out != NULL);
	assert(out->idx >= 0);

	dprintf(" out = %s\n", out->name);

	if (dss2_read_str(dss2_display, out->idx, "timings", buf, sizeof buf))
		goto error;

	if (!omap_output_get_size(out, &xres, &yres, NULL, NULL))
		goto error;

	if (sscanf(buf, "%u,%u/%u/%u/%u,%u/%u/%u/%u",
		   ret_pixclk,
		   ret_hdisp, ret_hfp, ret_hsw, ret_hbp,
		   ret_vdisp, ret_vfp, ret_vsw, ret_vbp) != 9)
		goto error;

	/* HACK for kernel's portrait->landscape scan kludge */
	if (must_swap_timings(out->rotate, xres, yres,
			      *ret_hdisp, *ret_vdisp)) {
		SWAP(*ret_hdisp, *ret_vdisp);
		SWAP(*ret_hfp, *ret_vfp);
		SWAP(*ret_hsw, *ret_vsw);
		SWAP(*ret_hbp, *ret_vbp);
	}

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}

bool omap_output_get_size(struct omap_output *out,
			  unsigned int *ret_xres,
			  unsigned int *ret_yres,
			  unsigned int *ret_width_um,
			  unsigned int *ret_height_um)
{
	struct omapfb_display_info display_info = { .xres = 0 };
	int r;
	struct omap_overlay *ovl;

	ENTER();

	dprintf(" out = %s\n", out->name);

	/* Ugh. Can only be done via an ovl :( */
	ovl = output_get_overlay(out);
	if (!ovl)
		goto error;

	assert(ovl->fd >= 0);

	dprintf(" ovl = %s\n", ovl->name);

	r = ioctl(ovl->fd, OMAPFB_GET_DISPLAY_INFO, &display_info);
	if (r)
		goto error;

	if (ret_xres)
		*ret_xres = display_info.xres;
	if (ret_yres)
		*ret_yres = display_info.yres;
	if (ret_width_um)
		*ret_width_um = display_info.width;
	if (ret_height_um)
		*ret_height_um = display_info.height;

	LEAVE();
	return true;

 error:
	ERROR();
	return false;
}
