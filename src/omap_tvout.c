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

#include "fbdev.h"
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <linux/omapfb.h>

#include "omap_video.h"
#include "omap_sysfs.h"
#include "omap_tvout.h"
#include "omap.h"

/* these are used in aspect ratio calculations */
static const struct {
	int aspwidth;
	int aspheight;
} tv_aspect_resolution[] = {
	[OMAP_TV_STANDARD_PAL]  = { 720, 576 },
	[OMAP_TV_STANDARD_NTSC] = { 720, 486 },
};

/* magic offsets to center the picture. */
static const struct {
	int x;
	int y;
} tv_offset[] = {
	[OMAP_TV_STANDARD_PAL]  = { .x =  4, .y = 4, },
	[OMAP_TV_STANDARD_NTSC] = { .x = 28, .y = 2, },
};

static const struct {
	int xaspect;
	int yaspect;
} tv_aspect[2] = {
	{  4, 3, },
	{ 16, 9, },
};

enum {
	WSS_4_3,
	WSS_16_9,
	WSS_LETTERBOX_14_9,
	WSS_LETTERBOX_16_9,
};

/* IEC 61880 */
static unsigned char crc(unsigned short data)
{
	const unsigned char poly = 0x30;
	unsigned char crc = 0x3f;
	int i;

	for (i = 0; i < 14; i++) {
		if ((crc ^ data) & 1)
			crc = (crc >> 1) ^ poly;
		else
			crc = (crc >> 1);
		data >>= 1;
	}

	return crc;
}

/* IEC 61880 */
static unsigned int ntsc_wss(unsigned char aspect)
{
	static const unsigned char ntsc_aspects[] = {
		[WSS_4_3]            = 0x0,
		[WSS_16_9]           = 0x1,
		[WSS_LETTERBOX_14_9] = 0x0, /* doesn't exist for NTSC */
		[WSS_LETTERBOX_16_9] = 0x2,
	};
	unsigned int wss = 0;

	if (aspect >= ARRAY_SIZE(ntsc_aspects))
		return 0;

	/* word 0 */
	wss |= ntsc_aspects[aspect];
	/* word 1 */
	wss |= 0x0 << 2;
	/* word 2 */
	wss |= 0x00 << 6;
	/* crc */
	wss |= crc(wss) << 14;

	return wss;
}

/* ETSI EN 300 294 */
static unsigned int pal_wss(unsigned char aspect)
{
	static const unsigned char pal_aspects[] = {
		[WSS_4_3]            = 0x8,
		[WSS_16_9]           = 0x7,
		[WSS_LETTERBOX_14_9] = 0x1,
		[WSS_LETTERBOX_16_9] = 0xb,
	};
	unsigned int wss = 0;

	if (aspect >= ARRAY_SIZE(pal_aspects))
		return 0;

	/* group 1 */
	wss |= pal_aspects[aspect];
	/* group 2 */
	wss |= 0x0 << 4;
	/* group 3 */
	wss |= 0x0 << 8;
	/* group 4 */
	wss |= 0x0 << 11;

	return wss;
}

/**
 * Calculate the scaled size taking the
 * source aspect ratio, TV out resolution, and the
 * physical TV aspect ratio into account.
 */
void omap_tvout_calc_scaling(Bool widescreen,
			     enum omap_tv_standard tv_std,
			     unsigned int tv_scale,
			     unsigned int in_w,
			     unsigned int in_h,
			     unsigned int *out_x,
			     unsigned int *out_y,
			     unsigned int *out_w,
			     unsigned int *out_h,
			     CARD32 *out_wss,
			     int maxwidth,
			     int maxheight,
			     int xoffset,
			     int yoffset,
			     Bool dynamic_aspect_ratio)
{
	int aspwidth, aspheight;
	int xaspect, yaspect;
	int width, height;
	unsigned char wss_aspect;

	/* Start with the aspect adjusted source size. */
	width = in_w;
	height = in_h;

	if (!widescreen) {
		if (dynamic_aspect_ratio && 9 * width >= 16 * height)
			wss_aspect = WSS_LETTERBOX_16_9;
		else if (dynamic_aspect_ratio && 9 * width >= 14 * height)
			wss_aspect = WSS_LETTERBOX_14_9;
		else
			wss_aspect = WSS_4_3;
	} else {
		/*
		 * This gives more horizontal resolution
		 * without sacrificing vertical resolution.
		 */
		if (dynamic_aspect_ratio && 3 * width <= 4 * height) {
			widescreen = 0;
			wss_aspect = WSS_4_3;
		} else {
			wss_aspect = WSS_16_9;
		}
	}

	switch (tv_std) {
	case OMAP_TV_STANDARD_NTSC:
		*out_wss = ntsc_wss(wss_aspect);
		break;
	case OMAP_TV_STANDARD_PAL:
		*out_wss = pal_wss(wss_aspect);
		break;
	default:
		*out_wss = 0;
		break;
	}

	aspwidth = tv_aspect_resolution[tv_std].aspwidth;
	aspheight = tv_aspect_resolution[tv_std].aspheight;

	xaspect = tv_aspect[widescreen].xaspect;
	yaspect = tv_aspect[widescreen].yaspect;

	/* Make it full-height. */
	if (height != maxheight) {
		width = width * maxheight / height;
		height = maxheight;
	}

	/* Adjust for TV aspect ratio and TV out resolution. */
	width = width * yaspect * aspwidth / (xaspect * aspheight);

	/* Scale it down if it doesn't fit. */
	if (width > maxwidth) {
		height = height * maxwidth / width;
		width = maxwidth;
	}

	/* Apply scaling adjustment. */
	width = width * tv_scale / 100;
	height = height * tv_scale / 100;

	if (xoffset < 0)
		xoffset = tv_offset[tv_std].x;
	if (yoffset < 0)
		yoffset = tv_offset[tv_std].y;

	/*
	 * Try to center the picture visually. Due to the x and y
	 * offsets that may not be possible. In case the centering
	 * fails just keep the the picture as close to the center
	 * as possible.
	 */
	if (width > maxwidth - xoffset)
		*out_x = maxwidth - width;
	else
		*out_x = (maxwidth + xoffset - width) / 2;

	if (height > maxheight - yoffset)
		*out_y = maxheight - height;
	else
		*out_y = (maxheight + yoffset - height) / 2;

	*out_w = width;
	*out_h = height;

	DebugF("omap/video: Cloning %dx%d -> %dx%d (TV: %s %d:%d)\n", in_w,
	       in_h, width, height, tv_std ? "NTSC" : "PAL", xaspect, yaspect);
}
