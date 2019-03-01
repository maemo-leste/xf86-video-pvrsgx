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

#ifndef FBDEV_H
#define FBDEV_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg-server.h>

#include <xf86.h>
#include <xf86_OSproc.h>

#include <dgaproc.h>

#include <xf86Crtc.h>

#include <fbdevhw.h>

#include <xf86xv.h>

#include "compat-api.h"

#include "omap.h"

#define PVRSGX_VERSION          4000
#define PVRSGX_NAME             "PVRSGX"
#define PVRSGX_DRIVER_NAME      "pvrsgx"

#if 0
#define CALLTRACE(...)		ErrorF(__VA_ARGS__)
#define DBGCOMPOSITE(...)	ErrorF(__VA_ARGS__)
#define DBG(...)		ErrorF(__VA_ARGS__)
#else
#define CALLTRACE(...)		do {} while (0)
#define DBGCOMPOSITE(...)	do {} while (0)
#define DBG(...)		do {} while (0)
#endif

/* Supported options */
enum {
	OPTION_PERF_TIME,
	OPTION_PERF_RESET,
	OPTION_TEST_COPY,
	OPTION_TEST_COPY_ONLY,
	OPTION_SWAP_CONTROL,
	OPTION_RENDER_SYNC,
	OPTION_VSYNC,
	OPTION_PAGE_FLIP_BUFS,
	OPTION_FLIP_STATS_TIME,
	OPTION_FLIP_STATS_RESET,
	OPTION_CAN_CHANGE_SCREEN_SIZE,
	OPTION_POISON_GETBUFFERS,
	OPTION_POISON_SWAPBUFFERS,
};

enum fbdev_overlay_usage {
	FBDEV_OVERLAY_USAGE_NONE,
	FBDEV_OVERLAY_USAGE_CRTC0,
	FBDEV_OVERLAY_USAGE_CRTC1,
	FBDEV_OVERLAY_USAGE_XV, /* any */
	FBDEV_OVERLAY_USAGE_XV1, /* VID1 */
	FBDEV_OVERLAY_USAGE_XV2, /* VID2 */
};

struct fbdev_crtc {
	enum fbdev_overlay_usage usage;
	struct omap_overlay *ovl;
	unsigned int sw, sh, dx, dy, dw, dh;
	int dpms;
};

enum fbdev_output_type {
	FBDEV_OUTPUT_TYPE_LCD,
	FBDEV_OUTPUT_TYPE_TV,
};

struct fbdev_output {
	Bool widescreen;
	unsigned int scale;
	enum omap_tv_standard tv_std;
	enum fbdev_output_type type;
	struct omap_output *out;
	Bool xv_clone_fullscreen;
	unsigned int pixclk;
	unsigned int hdisp, hfp, hsw, hbp;
	unsigned int vdisp, vfp, vsw, vbp;
	int xoffset;
	int yoffset;
	Bool dynamic_aspect;
	Bool alpha_mode;
	unsigned int graphics_alpha;
	unsigned int video_alpha;
};

typedef struct {
	void *fbmem;
	size_t fbmem_len;
	unsigned page_scan_next;
	CreateScreenResourcesProcPtr CreateScreenResources;
	CloseScreenProcPtr CloseScreen;
	EntityInfoPtr pEnt;
	OptionInfoPtr Options;
	ScreenPtr screen;

	/* The following are all used by omap_video.c. */
	int num_video_ports;
	XF86VideoAdaptorPtr overlay_adaptor;
	DestroyWindowProcPtr video_destroy_window;
	DestroyPixmapProcPtr video_destroy_pixmap;

	xf86CrtcPtr crtc_lcd;
	xf86CrtcPtr crtc_tv;
	xf86OutputPtr output_lcd;
	xf86OutputPtr output_tv;
	DisplayModePtr builtin_lcd;
	DisplayModePtr builtin_tv;

	enum fbdev_overlay_usage ovl_usage[3];
	struct omap_overlay *ovl[3];
	struct omap_fb *fb[3];
	struct omap_output *out[2];

	PixmapPtr pixmap;

	struct {
		Bool update_lock;
		DamagePtr damage;
		Bool enabled;
	} extfb;

	/* performance */
	OsTimerPtr perf_timer;

	/* flip statistics */
	OsTimerPtr flip_stats_timer;

	struct {
		unsigned int swap_control;
		Bool render_sync;
		Bool vsync;
		int page_flip_bufs;
		Bool can_change_screen_size;
		Bool poison_getbuffers;
		Bool poison_swapbuffers;
	} conf;
} FBDevRec, *FBDevPtr;

#define FBDEVPTR(p) ((FBDevPtr)((p)->driverPrivate))
#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, TRUE)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (int)(sizeof(a) / sizeof(a[0]))
#endif
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

#define SWAP(a, b) \
do { \
	typeof(a) tmp = a; \
	a = b; \
	b = tmp; \
} while (0)

#define ENTER() DebugF("Enter %s\n", __FUNCTION__)
#define LEAVE() DebugF("Leave %s\n", __FUNCTION__)

#ifndef max
#define max(x, y) (((x) >= (y)) ? (x) : (y))
#endif

#define ClipValue(v,min,max) ((v) < (min) ? (min) : (v) > (max) ? (max) : (v))

#define VIDEO_IMAGE_MAX_WIDTH 2048
#define VIDEO_IMAGE_MAX_HEIGHT 2048

xf86CrtcPtr fbdev_crtc_create(ScrnInfoPtr pScrn,
			      enum fbdev_overlay_usage usage);
xf86OutputPtr fbdev_output_create(ScrnInfoPtr pScrn,
				  const char *name,
				  enum fbdev_output_type type,
				  struct omap_output *out,
				  unsigned int crtc);

struct omap_overlay *fbdev_get_overlay(ScrnInfoPtr pScrn,
				       enum fbdev_overlay_usage usage);
void fbdev_put_overlay(ScrnInfoPtr pScrn, struct omap_overlay *ovl);

xf86OutputPtr fbdev_crtc_get_output(xf86CrtcPtr crtc);

void fbdev_flip_crtcs(ScrnInfoPtr pScrn, unsigned int page_scan_next);
void fbdev_update_outputs(ScrnInfoPtr pScrn,
			  const BoxRec *update_box);

/*
 * Takes the coordinates in box and adjusts them to match
 * the crtc's output coordinates. Takes crtc rotation and mirroring,
 * and output scaling and position into consideration.
 * Input coordinates are in screen coordinate space but they
 * must already be clipped to the area covered by the crtc.
 */
void fbdev_crtc_output_coords(xf86CrtcPtr crtc, const BoxRec *box,
			      unsigned int *ret_x, unsigned int *ret_y,
			      unsigned int *ret_w, unsigned int *ret_h);

enum omap_mirror
fbdev_rr_to_omap_mirror(Rotation rotation);
enum omap_rotate
fbdev_rr_to_omap_rotate(Rotation rotation);

Bool fbdev_output_check_rotation(xf86OutputPtr output,
				 Rotation rotation);

Bool fbdev_output_update_timings(xf86OutputPtr output);

void fbdev_flip(ScrnInfoPtr pScrn, unsigned page_scan_next);

CARD32 fbdev_rgb_to_pixel(ScrnInfoPtr pScrn,
			  CARD8 red, CARD8 green, CARD8 blue);

#endif
