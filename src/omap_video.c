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

#include <unistd.h>
#include <stdarg.h>

#include "fbdev.h"

#include <X11/Xatom.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/dpmsconst.h>
#include "fourcc.h"
#include "damage.h"

#include "windowstr.h"
#include "xf86Crtc.h"
#include "omap_video.h"
#include "omap_video_formats.h"
#include "omap_sysfs.h"
#include "sgx_xv.h"
#include "omap.h"
#include "extfb.h"

struct omap_video_info {
	int id;
	Bool allocated;
	void *mem;
	size_t mem_len;
	Bool dirty;
	DrawablePtr drawable;
	Pixel ckey;
	Bool autopaint_ckey;
	Bool disable_ckey;
	Bool changed_ckey;
	Bool vsync;

	enum {
		OMAP_STATE_STOPPED,
		OMAP_STATE_ACTIVE,
	} state;

	int fourcc;
	unsigned int width, height, pitch;
	short src_x, src_y, src_w, src_h;
	short dst_x, dst_y, dst_w, dst_h;
	short out_x, out_y, out_w, out_h;

	/* Enable color keying after a while if no frames were pushed. */
	OsTimerPtr ckey_timer;

	FBDevPtr fbdev;

	Bool double_buffer;
	unsigned int buffer;

	CARD8 video_alpha;
	CARD8 overlay_alpha;

	struct omap_fb *fb;
	struct omap_overlay *ovl;
	xf86CrtcPtr crtc;
	xf86CrtcPtr desired_crtc;

	Bool clone;
	/* CRTC for clone video */
	xf86CrtcPtr clone_crtc;
	Bool update_clone;
	enum omap_rotate rotate;
	enum omap_mirror mirror;
	Rotation crtc_rotation;
	Rotation rotation;

	/* For OMAPXV zero copy */
	Bool fbdev_reserved;

	int stacking;
};
#define get_omap_video_info(fbdev, n) ((fbdev)->overlay_adaptor->pPortPrivates[n].ptr)

static XF86VideoEncodingRec DummyEncodings[] = {
	{ 0, "XV_IMAGE",  VIDEO_IMAGE_MAX_WIDTH, VIDEO_IMAGE_MAX_HEIGHT, {1, 1}, },
	{ 0, "XV_OMAPFB", VIDEO_IMAGE_MAX_WIDTH, VIDEO_IMAGE_MAX_HEIGHT, {1, 1}, },
};

static XF86VideoFormatRec xv_formats[] = {
	{12, TrueColor},
	{16, TrueColor},
	{24, TrueColor},
};

static XF86AttributeRec xv_attributes[] = {
	{XvSettable | XvGettable, -1, 1, "XV_CRTC"},
	{XvSettable | XvGettable, 0, 1, "XV_SYNC_TO_VBLANK"},
	{XvSettable | XvGettable, 0, 0xffffff, "XV_COLORKEY"},
	{XvSettable | XvGettable, 0, 1, "XV_AUTOPAINT_COLORKEY"},
	{XvSettable | XvGettable, 0, 1, "XV_DISABLE_COLORKEY"},
	{XvSettable | XvGettable, 0, 1, "XV_DOUBLE_BUFFER"},
	{XvSettable | XvGettable, 0, 255, "XV_OVERLAY_ALPHA"},
	{XvGettable | XvSettable, 0, 1, "XV_CLONE_FULLSCREEN"},
	{XvGettable | XvSettable, 0, 1, "XV_OMAP_FBDEV_RESERVE"},
	{XvGettable             , -1, 7, "XV_OMAP_FBDEV_NUM"},
	{XvSettable             , 0, 0, "XV_OMAP_FBDEV_SYNC"},
	{XvGettable | XvSettable, 0, 2, "XV_STACKING"},
	{XvGettable | XvSettable, 1, 56, "XV_ROTATION"},
};

static Atom xv_ckey, xv_autopaint_ckey, xv_disable_ckey, xv_vsync;
static Atom xv_crtc, xv_overlay_alpha;
static Atom xv_double_buffer, xv_omap_fbdev_num, xv_omap_fbdev_reserve;
static Atom xv_clone_fullscreen, xv_omap_fbdev_sync, xv_stacking, xv_rotation;

/*
 * Window property, not Xv property.
 * Can be used to eg. inform the compositor that the window has
 * colorkeyed content so it should not apply effects that would
 * change the colorkey.
 */
static Atom _omap_video_overlay;

static XF86ImageRec xv_images[] = {
	XVIMAGE_YUY2,
	XVIMAGE_UYVY,
	XVIMAGE_I420,
	XVIMAGE_YV12,
	XVIMAGE_RV12,
	XVIMAGE_RV16,
	XVIMAGE_RV32,
	XVIMAGE_AV12,
	XVIMAGE_AV32,
};

#define OMAP_YV12_PITCH_LUMA(w)   ALIGN(w, 4)
#define OMAP_YV12_PITCH_CHROMA(w) ALIGN((OMAP_YV12_PITCH_LUMA(w) >> 1), 4)
#define OMAP_YUY2_PITCH(w)        (ALIGN(w, 2) << 1)
#define OMAP_RV16_PITCH(w)        ((w) << 1)
#define OMAP_RV32_PITCH(w)        ((w) << 2)

/*
 * Constants for calculating the maximum image
 * size which can fit in our video memory.
 */
enum {
	MAX_PLANES          = 1,
	MAX_BUFFERS         = 6,
	MAX_BYTES_PER_PIXEL = 2,
};

static struct omap_output *get_out_from_crtc(xf86CrtcPtr crtc)
{
	xf86OutputPtr output;
	struct fbdev_output *priv;

	if (!crtc)
		return NULL;

	output = fbdev_crtc_get_output(crtc);
	if (!output)
		return NULL;

	priv = output->driver_private;

	return priv->out;
}

static Bool set_overlay_alpha(struct omap_video_info *video_info)
{
	if (!video_info->ovl)
		return TRUE;

	if (!omap_overlay_global_alpha(video_info->ovl,
				       video_info->overlay_alpha *
				       video_info->video_alpha / 255))
		return FALSE;

	return TRUE;
}

static void sync_gfx(struct omap_video_info *video_info)
{
	struct omap_output *out = get_out_from_crtc(video_info->crtc);

	omap_output_wait_update(out);
}

/**
 * Check if plane attributes have changed.
 */
static _X_INLINE Bool is_dirty(struct omap_video_info *video_info,
			       int fourcc,
			       int src_x, int src_y, int dst_x, int dst_y,
			       int src_w, int src_h, int dst_w, int dst_h)
{
	if (video_info->dirty || video_info->fourcc != fourcc
	    || video_info->src_x != src_x || video_info->src_y != src_y
	    || video_info->dst_x != dst_x || video_info->dst_y != dst_y
	    || video_info->src_w != src_w || video_info->src_h != src_h
	    || video_info->dst_w != dst_w || video_info->dst_h != dst_h)
		return TRUE;
	else
		return FALSE;
}

static void clamp_image_to_scaling_limits(short width, short height,
					 short *dst_w, short *dst_h)
{
	int maxvdownscale = width > 1024 ? 2 : 4;

	/* Respect hardware scaling limits. */
	if (*dst_w > width * 8)
	  *dst_w = width * 8;
	else if (*dst_w < width / 4)
	  *dst_w = width / 4;

	if (*dst_h > height * 8)
	  *dst_h = height * 8;
	else if (*dst_h < height / maxvdownscale)
	  *dst_h = height / maxvdownscale;
}

static void rotate_pre_clip(short *x, short *y,
			    short *w, short *h,
			    short *width, short *height,
			    Rotation rotation)
{
	short tx = *x;
	short ty = *y;
	short tw = *w;
	short th = *h;

	if (rotation & RR_Reflect_X)
		tx = *width - tx - tw;
	if (rotation & RR_Reflect_Y)
		ty = *height - ty - th;

	switch (rotation & 0xf) {
	case RR_Rotate_0:
		*x = tx;
		*w = tw;
		*y = ty;
		*h = th;
		break;
	case RR_Rotate_270:
		*x = *height - th - ty;
		*w = th;
		*y = tx;
		*h = tw;
		SWAP(*width, *height);
		break;
	case RR_Rotate_180:
		*x = *width - tw - tx;
		*w = tw;
		*y = *height - th - ty;
		*h = th;
		break;
	case RR_Rotate_90:
		*x = ty;
		*w = th;
		*y = *width - tw - tx;
		*h = tw;
		SWAP(*width, *height);
		break;
	default:
		break;
	}
}

static void rotate_post_clip(short *x, short *y,
			     short *w, short *h,
			     short *width, short *height,
			     Rotation rotation)
{
	short tx = *x;
	short ty = *y;
	short tw = *w;
	short th = *h;

	switch (rotation & 0xf) {
	case RR_Rotate_0:
		*x = tx;
		*w = tw;
		*y = ty;
		*h = th;
		break;
	case RR_Rotate_270:
		*x = ty;
		*w = th;
		*y = *width - tw - tx;
		*h = tw;
		SWAP(*width, *height);
		break;
	case RR_Rotate_180:
		*x = *width - tw - tx;
		*w = tw;
		*y = *height - th - ty;
		*h = th;
		break;
	case RR_Rotate_90:
		*x = *height - th - ty;
		*w = th;
		*y = tx;
		*h = tw;
		SWAP(*width, *height);
		break;
	default:
		break;
	}

	if (rotation & RR_Reflect_X)
		*x = *width - *x - *w;
	if (rotation & RR_Reflect_Y)
		*y = *height - *y - *h;
}

/**
 * Consider the given position (x, y) and dimensions (w, h) and adjust (clip)
 * the final target values to fit the screen dimensions.
 */
static Bool clip_image_to_fit(ScrnInfoPtr pScrn,
			      short *src_x, short *src_y,
			      short *dst_x, short *dst_y,
			      short *src_w, short *src_h,
			      short *dst_w, short *dst_h,
			      short width, short height,
			      Rotation rotation,
			      RegionPtr clip_boxes,
			      xf86CrtcPtr desired_crtc,
			      xf86CrtcPtr *crtc)
{
	BoxRec dstBox;
	long xa, xb, ya, yb;

	dstBox.x1 = *dst_x;
	dstBox.x2 = *dst_x + *dst_w;
	dstBox.y1 = *dst_y;
	dstBox.y2 = *dst_y + *dst_h;

	/*
	 * Source coordinates are oriented with the image
	 * data, destination and clip coordinates are
	 * oriented with the screen. Adjust the source
	 * coordinates to match the screen orientation so
	 * that clipping works correctly.
	 */
	rotate_pre_clip(src_x, src_y, src_w, src_h,
			&width, &height, rotation);

	xa = *src_x;
	xb = *src_x + *src_w;
	ya = *src_y;
	yb = *src_y + *src_h;

#if 0
	DebugF("omap/clip: before %dx%d+%d+%d to %dx%d+%d+%d\n",
	       *src_w, *src_h, *src_x, *src_y,
	       *dst_w, *dst_h, *dst_x, *dst_y);
#endif

	/* Failure here means simply that one is requesting the image to be
	 * positioned entirely outside the screen. Existing video drivers seem to
	 * treat this as implicit success, and hence this behaviour is adopted here
	 * also.
	 */
	if (!xf86_crtc_clip_video_helper(pScrn, crtc, desired_crtc,
					 &dstBox, &xa, &xb, &ya, &yb,
					 clip_boxes, width, height))
		return FALSE;

	/* This should be taken cared of by 'xf86XVClipVideoHelper()', but for
	 * safety's sake, one re-checks.
	 */
	if (dstBox.x2 <= dstBox.x1 || dstBox.y2 <= dstBox.y1)
		return FALSE;

	*dst_x = dstBox.x1;
	*dst_w = dstBox.x2 - dstBox.x1;
	*dst_y = dstBox.y1;
	*dst_h = dstBox.y2 - dstBox.y1;

	*src_x = xa >> 16;
	*src_w = (xb - xa) >> 16;
	*src_y = ya >> 16;
	*src_h = (yb - ya) >> 16;

	/* Undo rotation adjustment. */
	rotate_post_clip(src_x, src_y, src_w, src_h,
			 &width, &height, rotation);

#if 0
	DebugF("omap/clip: after %dx%d+%d+%d to %dx%d+%d+%d\n",
	       *src_w, *src_h, *src_x, *src_y,
	       *dst_w, *dst_h, *dst_x, *dst_y);
#endif

	return TRUE;
}

static CARD32 default_ckey(ScrnInfoPtr pScrn)
{
	return fbdev_rgb_to_pixel(pScrn, 0x00, 0xff, 0x00);
}

static int setup_colorkey(struct omap_video_info *video_info, Bool enable)
{
	ScreenPtr screen = video_info->fbdev->screen;
	ScrnInfoPtr pScrn = xf86Screens[screen->myNum];
	uint32_t key = video_info->ckey & (pScrn->mask.red |
					   pScrn->mask.green |
					   pScrn->mask.blue);
	struct omap_output *out = get_out_from_crtc(video_info->crtc);

	if (!out)
		return TRUE;

	if (!omap_output_color_key(out, enable, key)) {
		ErrorF("omap/video: couldn't set colorkey\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * Enable color keying after a while so that a misbehaving client
 * can't prevent popup notifications indefinitely.
 */
static CARD32 ckey_timer(OsTimerPtr timer, CARD32 now, pointer data)
{
	struct omap_video_info *video_info = data;

	DebugF("omap/video: ckey_timer: timeout on plane %d\n",
	       video_info->id);

	TimerFree(video_info->ckey_timer);
	video_info->ckey_timer = NULL;

	/* Enable color keying */
	setup_colorkey(video_info, TRUE);

	/*
	 * Make sure color keying gets disabled again
	 * when the client resumes sending frames.
	 */
	video_info->dirty = TRUE;

	return 0;
}

static void cancel_ckey_timer(struct omap_video_info *video_info)
{
	if (!video_info->ckey_timer)
		return;

	TimerFree(video_info->ckey_timer);
	video_info->ckey_timer = NULL;
}

static void set_ckey_timer(struct omap_video_info *video_info)
{
	video_info->ckey_timer = TimerSet(video_info->ckey_timer, 0, 1000,
					  ckey_timer, video_info);
}

static void rearm_ckey_timer(struct omap_video_info *video_info)
{
	cancel_ckey_timer(video_info);

	if (video_info->disable_ckey)
		set_ckey_timer(video_info);
}

static void unmap_video_mem(struct omap_video_info *video_info)
{
	if (!video_info->mem)
		return;

	omap_fb_unmap(video_info->fb);

	video_info->mem = NULL;
	video_info->mem_len = 0;
}

static Bool map_video_mem(struct omap_video_info *video_info)
{
	if (video_info->mem)
		return TRUE;

	if (!omap_fb_map(video_info->fb,
			 &video_info->mem,
			 &video_info->mem_len)) {
		ErrorF("omap/video: couldn't mmap plane memory %d\n",
		       video_info->id);
		return FALSE;
	}

	return TRUE;
}

static enum omap_format get_omap_format(CARD32 fourcc)
{
	switch (fourcc) {
	case FOURCC_I420:
	case FOURCC_YV12:
	case FOURCC_YUY2:
		return OMAP_FORMAT_YUY2;
	case FOURCC_UYVY:
		return OMAP_FORMAT_UYVY;
	case FOURCC_RV16:
		return OMAP_FORMAT_RGB565;
	case FOURCC_RV12:
		return OMAP_FORMAT_XRGB4444;
	case FOURCC_AV12:
		return OMAP_FORMAT_ARGB4444;
	case FOURCC_RV32:
		return OMAP_FORMAT_XRGB8888;
	case FOURCC_AV32:
		return OMAP_FORMAT_ARGB8888;
	default:
		assert(0);
	}
}

/**
 * Allocates video memory to satisfy the specified image size and format.
 */
static Bool alloc_mem(struct omap_video_info *video_info,
		      int fourcc, int width, int height)
{
	enum omap_format format = get_omap_format(fourcc);
	unsigned int pitch;

	if (video_info->allocated &&
	    omap_fb_check_size(video_info->fb, width, height,
			       format, 2, 1, 1))
		return TRUE;

	/* Disable plane so that reallocation will work. */
	if (video_info->state == OMAP_STATE_ACTIVE) {
		omap_overlay_disable(video_info->ovl);
		unmap_video_mem(video_info);
	}

	if (!omap_fb_alloc(video_info->fb, width, height,
			   format, 2, 1, 1)) {
		ErrorF("omap/video: couldn't allocate memory for video plane!\n");
		return FALSE;
	}

	if (!omap_fb_get_info(video_info->fb, NULL, NULL, &pitch)) {
		ErrorF("omap/video: couldn't get memory info!\n");
		omap_fb_free(video_info->fb);
		video_info->allocated = FALSE;
		return FALSE;
	}

	video_info->allocated = TRUE;
	video_info->dirty = TRUE;
	video_info->pitch = pitch;

	return TRUE;
}

/*
 * Free the video memory allocated for this plane.
 */
static void free_mem(struct omap_video_info *video_info)
{
	if (!video_info->allocated)
		return;

	if (!omap_fb_free(video_info->fb)) {
		ErrorF("omap/video: couldn't deallocate plane\n");
		return;
	}

	video_info->allocated = FALSE;
}

/**
 * Fully sets up a video plane.
 * Configures the image size, source area, destination area, mmaps the video memory.
 * Used by PutImage
 */
static Bool setup_plane(struct omap_video_info *video_info)
{
	unsigned int buffer =
		video_info->double_buffer ? video_info->buffer : 0;

	omap_overlay_setup(video_info->ovl, buffer,
			   video_info->src_x, video_info->src_y,
			   video_info->src_w, video_info->src_h,
			   video_info->out_x, video_info->out_y,
			   video_info->out_w, video_info->out_h,
			   video_info->mirror, video_info->rotate);

	if (!map_video_mem(video_info))
		return FALSE;

	video_info->dirty = FALSE;

	return TRUE;
}

/**
 * Partially sets up a video plane.
 * Configures the source area, destination area.
 * Used by PutVideo
 */
static Bool
position_plane(struct omap_video_info *video_info)
{
	video_info->buffer = 0;

	omap_overlay_setup(video_info->ovl, 0,
			   video_info->src_x, video_info->src_y,
			   video_info->src_w, video_info->src_h,
			   video_info->out_x, video_info->out_y,
			   video_info->out_w, video_info->out_h,
			   video_info->mirror, video_info->rotate);

	video_info->dirty = FALSE;

	return TRUE;
}

/*
 * Calculates the pointer to the video memory.
 * Takes the source offset and buffer index into account.
 */
static void *calc_mem(struct omap_video_info *video_info)
{
	unsigned int next_buffer =
		video_info->double_buffer ? !video_info->buffer : 0;
	unsigned int buffer_offset = video_info->height * video_info->pitch;

	return video_info->mem + next_buffer * buffer_offset;
}

/*
 * Flips to the alternate buffer.
 */
static Bool flip_plane(struct omap_video_info *video_info)
{
	unsigned int next_buffer =
		video_info->double_buffer ? !video_info->buffer : 0;

	if (!omap_overlay_setup(video_info->ovl, next_buffer,
				video_info->src_x, video_info->src_y,
				video_info->src_w, video_info->src_h,
				video_info->out_x, video_info->out_y,
				video_info->out_w, video_info->out_h,
				video_info->mirror, video_info->rotate))
		return FALSE;

	video_info->buffer = next_buffer;
	video_info->update_clone = TRUE;

	return TRUE;
}

static void drawable_destroyed(FBDevPtr fbdev, DrawablePtr drawable)
{
	int i;
	struct omap_video_info *video_info;

	for (i = 0; i < fbdev->num_video_ports; i++) {
		video_info = get_omap_video_info(fbdev, i);
		if (video_info->drawable == drawable)
			video_info->drawable = NULL;
	}
}

static Bool destroy_pixmap_hook(PixmapPtr pixmap)
{
	Bool ret;
	ScreenPtr screen = pixmap->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86Screens[screen->myNum];
	FBDevPtr fbdev = pScrn->driverPrivate;

	drawable_destroyed(fbdev, (DrawablePtr) pixmap);

	screen->DestroyPixmap = fbdev->video_destroy_pixmap;
	ret = screen->DestroyPixmap(pixmap);
	screen->DestroyPixmap = destroy_pixmap_hook;

	return ret;
}

static Bool destroy_window_hook(WindowPtr window)
{
	Bool ret;
	ScreenPtr screen = window->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86Screens[screen->myNum];
	FBDevPtr fbdev = pScrn->driverPrivate;

	drawable_destroyed(fbdev, (DrawablePtr) window);

	screen->DestroyWindow = fbdev->video_destroy_window;
	ret = screen->DestroyWindow(window);
	screen->DestroyWindow = destroy_window_hook;

	return ret;
}

static void change_overlay_property(struct omap_video_info *video_info, int val)
{
	WindowPtr window;
	int err;

	if (!video_info->drawable) {
		DebugF("change_overlay_property: not changing property: no overlay\n");
		return;
	}

	if (video_info->drawable->type != DRAWABLE_WINDOW) {
		DebugF("change_overlay_property: not changing property: type is %d\n",
		       video_info->drawable->type);
		return;
	}

	/* Walk the tree to get the top-level window. */
	for (window = (WindowPtr) video_info->drawable;
	     window && window->parent; window = window->parent) {
		err = ChangeWindowProperty(window, _omap_video_overlay,
					   XA_INTEGER, 8, PropModeReplace,
					   1, &val, TRUE);
		if (err != Success)
			ErrorF("change_overlay_property: failed to change property\n");
	}
}

static void push_update(ScrnInfoPtr pScrn,
			struct omap_video_info *video_info,
			RegionPtr old);

/**
 * Stop the video overlay.
 */
static void stop_video(struct omap_video_info *video_info)
{
	if (video_info->state == OMAP_STATE_ACTIVE)
		change_overlay_property(video_info, 0);

	if (video_info->state == OMAP_STATE_ACTIVE) {
		ScreenPtr pScreen = video_info->fbdev->screen;
		ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

		omap_overlay_disable(video_info->ovl);
		unmap_video_mem(video_info);

		/* Make sure we update the old position */
		push_update(pScrn, video_info, NULL);

		/* The old position is now clean */
		video_info->dst_x = 0;
		video_info->dst_y = 0;
		video_info->dst_w = 0;
		video_info->dst_h = 0;
	}

	free_mem(video_info);

	video_info->state = OMAP_STATE_STOPPED;

	video_info->crtc = NULL;

	cancel_ckey_timer(video_info);

	DebugF("omap stop_video: stopped plane %d\n", video_info->id);
}

/* Make a note of the current overlay screen coordinates */
static void init_update_region(ScrnInfoPtr pScrn,
			       RegionPtr reg,
			       struct omap_video_info *video_info)
{
	BoxRec box = {
		.x1 = video_info->dst_x,
		.x2 = video_info->dst_x + video_info->dst_w,
		.y1 = video_info->dst_y,
		.y2 = video_info->dst_y + video_info->dst_h,
	};

	RegionInit(reg, &box, 0);
}

/*
 * Update the display area covered by the old and
 * current overlay screen coordinates.
 */
static void push_update(ScrnInfoPtr pScrn,
			struct omap_video_info *video_info,
			RegionPtr old)
{
	RegionRec reg;
	BoxRec box = {
		.x1 = video_info->dst_x,
		.x2 = video_info->dst_x + video_info->dst_w,
		.y1 = video_info->dst_y,
		.y2 = video_info->dst_y + video_info->dst_h,
	};

	RegionInit(&reg, &box, 0);

	if (old)
		RegionAppend(&reg, old);

	ExtFBDamage(pScrn, &reg);

	RegionUninit(&reg);
}

static void put_overlay(struct omap_video_info *video_info)
{
	FBDevPtr fbdev = video_info->fbdev;
	ScreenPtr pScreen = fbdev->screen;
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

	if (!video_info->ovl)
		return;

	fbdev_put_overlay(pScrn, video_info->ovl);
	video_info->ovl = NULL;
}

static Bool get_overlay(struct omap_video_info *video_info)
{
	FBDevPtr fbdev = video_info->fbdev;
	ScreenPtr pScreen = fbdev->screen;
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	enum fbdev_overlay_usage usage;

	if (video_info->ovl)
		return TRUE;

	switch (video_info->stacking) {
	case 0:
		usage = FBDEV_OVERLAY_USAGE_XV;
		break;
	case 1:
		usage = FBDEV_OVERLAY_USAGE_XV1;
		break;
	case 2:
		usage = FBDEV_OVERLAY_USAGE_XV2;
		break;
	default:
		assert(0);
	}

	video_info->ovl = fbdev_get_overlay(pScrn, usage);
	if (!video_info->ovl)
		return FALSE;

	if (!omap_fb_assign_overlay(video_info->fb, video_info->ovl)) {
		put_overlay(video_info);
		return FALSE;
	}

	video_info->dirty = TRUE;

	return TRUE;
}

static struct omap_video_info *
get_clone_info(ScrnInfoPtr pScrn)
{
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	int i;

	/*
	 * FIXME should check if cloning is even allowed
	 * Would avoid doing unnecesary work in clone_update().
	 */
	for (i = 0; i < fbdev->num_video_ports; i++) {
		struct omap_video_info *video_info =
			get_omap_video_info(fbdev, i);
		if (video_info->clone &&
		    video_info->state == OMAP_STATE_ACTIVE)
			return video_info;
	}

	return NULL;
}

static void clone_sync(ScrnInfoPtr pScrn)
{
	struct omap_video_info *video_info;
	xf86CrtcPtr crtc;
	struct fbdev_crtc *priv;

	video_info = get_clone_info(pScrn);
	if (!video_info)
		return;

	crtc = video_info->clone_crtc;
	if (!crtc)
		return;

	priv = crtc->driver_private;

	if (!crtc->enabled || priv->dpms != DPMSModeOn)
		return;

	omap_overlay_wait(priv->ovl);
}

/**
 * Xv attributes get/set support.
 */
static int omap_video_get_attribute(ScrnInfoPtr pScrn, Atom attribute,
				    INT32 * value, pointer data)
{
	struct omap_video_info *video_info = data;

	ENTER();

	if (attribute == xv_crtc) {
		xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
		int i;
		for (i = 0; i < config->num_crtc; i++)
			if (config->crtc[i] == video_info->desired_crtc)
				break;
		if (i == config->num_crtc)
			i = -1;
		*value = i;
		LEAVE();
		return Success;
	} else if (attribute == xv_vsync) {
		*value = video_info->vsync;
		LEAVE();
		return Success;
	} else if (attribute == xv_double_buffer) {
		*value = video_info->double_buffer;
		LEAVE();
		return Success;
	} else if (attribute == xv_ckey) {
		*value = video_info->ckey;
		LEAVE();
		return Success;
	} else if (attribute == xv_autopaint_ckey) {
		*value = video_info->autopaint_ckey;
		LEAVE();
		return Success;
	} else if (attribute == xv_disable_ckey) {
		*value = video_info->disable_ckey;
		LEAVE();
		return Success;
	} else if (attribute == xv_overlay_alpha) {
		*value = video_info->overlay_alpha;
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_fbdev_num) {
		if (video_info->fbdev_reserved)
			*value = omap_overlay_id(video_info->ovl);
		else
			*value = -1;
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_fbdev_reserve) {
		*value = video_info->fbdev_reserved;
		LEAVE();
		return Success;
	} else if (attribute == xv_clone_fullscreen) {
		*value = video_info->clone;
		LEAVE();
		return Success;
	} else if (attribute == xv_stacking) {
		*value = video_info->stacking;
		LEAVE();
		return Success;
	} else if (attribute == xv_rotation) {
		*value = video_info->rotation;
		LEAVE();
		return Success;
	}

	LEAVE();
	return BadMatch;
}

static void update_rotation(struct omap_video_info *video_info);
static void update_alpha(struct omap_video_info *video_info);

static int omap_video_set_attribute(ScrnInfoPtr pScrn, Atom attribute,
				    INT32 value, pointer data)
{
	struct omap_video_info *video_info = data;

	ENTER();

	if (attribute == xv_crtc) {
		xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);

		if (value < -1 || value >= config->num_crtc) {
			LEAVE();
			return BadValue;
		}

		if (value < 0)
			video_info->desired_crtc = NULL;
		else
			video_info->desired_crtc = config->crtc[value];

		LEAVE();
		return Success;
	} else if (attribute == xv_vsync) {
		if (value < 0 || value > 1) {
			LEAVE();
			return BadValue;
		}

		video_info->vsync = value;
		LEAVE();
		return Success;
	} else if (attribute == xv_double_buffer) {
		if (value != 0 && value != 1) {
			LEAVE();
			return BadValue;
		}

		video_info->double_buffer = value;
		video_info->dirty = TRUE;
		LEAVE();
		return Success;
	} else if (attribute == xv_ckey) {
		if (value < 0 || value > 0xffffff) {
			LEAVE();
			return BadValue;
		}

		video_info->ckey = value;
		setup_colorkey(video_info, !video_info->disable_ckey);
		video_info->changed_ckey = TRUE;
		LEAVE();
		return Success;
	} else if (attribute == xv_autopaint_ckey) {
		if (value != 0 && value != 1) {
			LEAVE();
			return BadValue;
		}

		video_info->autopaint_ckey = value;
		video_info->dirty = TRUE;
		LEAVE();
		return Success;
	} else if (attribute == xv_disable_ckey) {
		if (value != 0 && value != 1) {
			LEAVE();
			return BadValue;
		}
		if (value == video_info->disable_ckey) {
			LEAVE();
			return Success;
		}

		video_info->disable_ckey = value;
		setup_colorkey(video_info, !video_info->disable_ckey);
		rearm_ckey_timer(video_info);
		LEAVE();
		return Success;
	} else if (attribute == xv_overlay_alpha) {
		if (value < 0 || value > 255) {
			LEAVE();
			return BadValue;
		}
		if (value == video_info->overlay_alpha) {
			LEAVE();
			return Success;
		}
		video_info->overlay_alpha = value;
		update_alpha(video_info);
		set_overlay_alpha(video_info);
		LEAVE();
		return Success;
	} else if (attribute == xv_clone_fullscreen) {
		if (value < 0 || value > 1) {
			LEAVE();
			return BadValue;
		}

		if (video_info->clone != value) {
			video_info->clone = value;
			video_info->crtc = NULL;
		}
		LEAVE();
		return Success;
	} else if (attribute == xv_omap_fbdev_reserve) {
		if (value < 0 || value > 1) {
			LEAVE();
			return BadValue;
		}

		if (!video_info->fbdev_reserved && value) {
			if (video_info->state == OMAP_STATE_ACTIVE) {
				LEAVE();
				return BadAlloc;
			}

			assert(!video_info->ovl);

			if (!get_overlay(video_info)) {
				LEAVE();
				return BadAlloc;
			}
		} else if (video_info->fbdev_reserved && !value) {
			if (video_info->state == OMAP_STATE_ACTIVE) {
				LEAVE();
				return BadAlloc;
			}

			assert(video_info->ovl);

			omap_fb_free(video_info->fb);
			put_overlay(video_info);
		}
		video_info->fbdev_reserved = value;

		LEAVE();
		return Success;
	} else if (attribute == xv_omap_fbdev_sync) {
		if (value != 0) {
			LEAVE();
			return BadValue;
		}

		if (!video_info->fbdev_reserved) {
			LEAVE();
			return BadAlloc;
		}

		if (video_info->state == OMAP_STATE_ACTIVE &&
		    video_info->vsync) {
			clone_sync(pScrn);
			omap_overlay_wait(video_info->ovl);
		}

		LEAVE();
		return Success;
	} else if (attribute == xv_stacking) {
		if (value < 0 || value > 2) {
			LEAVE();
			return BadValue;
		}

		video_info->stacking = value;
		LEAVE();
		return Success;
	} else if (attribute == xv_rotation) {
		switch (value & 0xf) {
		case RR_Rotate_0:
		case RR_Rotate_90:
		case RR_Rotate_180:
		case RR_Rotate_270:
			break;
		default:
			LEAVE();
			return BadValue;
		}

		switch (value & ~0xf) {
		case 0:
		case RR_Reflect_X:
		case RR_Reflect_Y:
		case RR_Reflect_X | RR_Reflect_Y:
			break;
		default:
			LEAVE();
			return BadValue;
		}

		if (value != video_info->rotation) {
			video_info->rotation = value;
			update_rotation(video_info);
		}

		LEAVE();
		return Success;
	}

	LEAVE();
	return BadMatch;
}

/**
 * Clip the image size to the visible screen.
 */
static void omap_video_query_best_size(ScrnInfoPtr pScrn, Bool motion,
				       short vid_w, short vid_h, short dst_w,
				       short dst_h, unsigned int *p_w,
				       unsigned int *p_h, pointer data)
{
	/* Clip the image size to the visible screen. */
	if (dst_w > pScrn->virtualX)
		dst_w = pScrn->virtualX;

	if (dst_h > pScrn->virtualY)
		dst_h = pScrn->virtualY;

	clamp_image_to_scaling_limits(vid_w, vid_h, &dst_w, &dst_h);

	*p_w = dst_w;
	*p_h = dst_h;
}

/**
 * Start the video overlay; relies on data in video_info being sensible for
 * the current frame.
 */
static Bool start_video(struct omap_video_info *video_info)
{
	if (video_info->fourcc) {
		/* PutImage */
		if (!setup_plane(video_info)) {
			DebugF("omap/start_video: couldn't setup plane %d\n",
			       video_info->id);
			return FALSE;
		}
	} else {
		/* PutVideo */
		if (!position_plane(video_info)) {
			DebugF("omap/start_video: couldn't position plane %d\n",
			       video_info->id);
			return FALSE;
		}
	}

	if (video_info->state == OMAP_STATE_STOPPED)
		change_overlay_property(video_info, 1);

	video_info->state = OMAP_STATE_ACTIVE;

	return TRUE;
}

Bool omap_video_clone(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
		      unsigned int *buffer,
		      unsigned int *sx, unsigned int *sy,
		      unsigned int *sw, unsigned int *sh,
		      unsigned int *dw, unsigned int *dh)
{
	struct omap_video_info *video_info = get_clone_info(pScrn);
	struct fbdev_crtc *priv = crtc->driver_private;

	DebugF("Cloning for video_info %p\n", video_info);

	if (!video_info)
		return FALSE;

	if (crtc != video_info->clone_crtc)
		return FALSE;

	if (omap_overlay_get_fb(priv->ovl) != video_info->fb) {
		/*
		 * Trick to make Xv<->CRTC clone switching smoother.
		 * set_mode_major will re-enable the overlay later.
		 */
		omap_overlay_disable(priv->ovl);

		if (!omap_fb_assign_overlay(video_info->fb, priv->ovl))
			return FALSE;
	}

	*buffer = video_info->double_buffer ? video_info->buffer : 0;
	*sx = video_info->src_x;
	*sy = video_info->src_y;
	*sw = video_info->src_w;
	*sh = video_info->src_h;
	*dw = video_info->dst_w;
	*dh = video_info->dst_h;

	return TRUE;
}

Bool omap_video_clone_active(ScrnInfoPtr pScrn, xf86CrtcPtr crtc)
{
	struct omap_video_info *video_info = get_clone_info(pScrn);

	return video_info && crtc == video_info->clone_crtc;
}

static void clone_update(ScrnInfoPtr pScrn)
{
	struct omap_video_info *video_info;
	xf86CrtcPtr crtc;
	struct fbdev_crtc *priv;

	video_info = get_clone_info(pScrn);
	if (!video_info)
		return;

	if (!video_info->update_clone)
		return;

	crtc = video_info->clone_crtc;
	if (!crtc)
		return;

	priv = crtc->driver_private;

	if (!crtc->enabled || priv->dpms != DPMSModeOn)
		return;

	/*
	 * FIXME refactor the code a bit to avoid having
	 * to call set_mode_major() for this.
	 */
	crtc->funcs->set_mode_major(crtc, &crtc->mode,
				    crtc->rotation,
				    crtc->x, crtc->y);
	video_info->update_clone = FALSE;
}

static void clone_stop(struct omap_video_info *video_info)
{
	xf86CrtcPtr crtc = video_info->clone_crtc;
	struct fbdev_crtc *priv;

	if (!crtc)
		return;

	video_info->clone_crtc = NULL;
	priv = crtc->driver_private;

	if (!crtc->enabled || priv->dpms != DPMSModeOn)
		return;

	crtc->funcs->set_mode_major(crtc, &crtc->mode,
				    crtc->rotation,
				    crtc->x, crtc->y);
}

/**
 * Stop an overlay.  exit is whether or not the client's exiting.
 */
static void omap_video_stop(ScrnInfoPtr pScrn, pointer data, Bool exit)
{
	struct omap_video_info *video_info = data;

	ENTER();

	clone_stop(video_info);

	stop_video(video_info);

	video_info->dirty = TRUE;
	video_info->update_clone = TRUE;

	if (exit) {
		video_info->double_buffer = TRUE;
		video_info->vsync = TRUE;
		video_info->state = OMAP_STATE_STOPPED;
		video_info->ckey = default_ckey(pScrn);
		video_info->autopaint_ckey = TRUE;
		video_info->disable_ckey = FALSE;
		video_info->overlay_alpha = 255;
		video_info->clone = TRUE;
		video_info->stacking = 0;
		video_info->rotation = RR_Rotate_0;

		/* Disable color keying */
		setup_colorkey(video_info, FALSE);

		/* Make sure XvPutVideo users can't leak resources */
		omap_fb_free(video_info->fb);
		video_info->fbdev_reserved = FALSE;
	}

	video_info->drawable = NULL;

	if (!video_info->fbdev_reserved)
		put_overlay(video_info);

	clone_update(pScrn);

	LEAVE();
}

/**
 * Set up video_info with the specified parameters, and start the overlay.
 */
static Bool setup_overlay(ScrnInfoPtr pScrn,
			  struct omap_video_info *video_info,
			  int fourcc, int width, int height,
			  int src_x, int src_y, int dst_x, int dst_y,
			  int src_w, int src_h, int dst_w, int dst_h,
			  DrawablePtr drawable)
{
	xf86CrtcPtr crtc = video_info->crtc;
	unsigned int out_x, out_y, out_w, out_h;
	const BoxRec box = {
		.x1 = dst_x,
		.x2 = dst_x + dst_w,
		.y1 = dst_y,
		.y2 = dst_y + dst_h,
	};

	ENTER();

	video_info->src_x = src_x;
	video_info->src_y = src_y;
	video_info->src_w = src_w;
	video_info->src_h = src_h;
	video_info->dst_w = dst_w;
	video_info->dst_h = dst_h;
	video_info->dst_x = dst_x;
	video_info->dst_y = dst_y;

	fbdev_crtc_output_coords(crtc, &box, &out_x, &out_y, &out_w, &out_h);

	video_info->out_w = out_w;
	video_info->out_h = out_h;
	video_info->out_x = out_x;
	video_info->out_y = out_y;

	LEAVE();

	return start_video(video_info);
}

static xf86CrtcPtr get_clone_crtc(ScrnInfoPtr pScrn,
				  struct omap_video_info *video_info)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	int i;

	if (!video_info->crtc || !video_info->clone)
		return NULL;

	for (i = 0; i < config->num_crtc; i++) {
		xf86CrtcPtr crtc = config->crtc[i];

		if (crtc != video_info->crtc)
			return crtc;
	}

	return NULL;
}

static void update_alpha(struct omap_video_info *video_info)
{
	xf86OutputPtr output = NULL;
	struct fbdev_output *priv;

	if (video_info->crtc)
		output = fbdev_crtc_get_output(video_info->crtc);

	if (!output) {
		video_info->video_alpha = 255;
		return;
	}

	priv = output->driver_private;

	video_info->video_alpha = priv->video_alpha;
}

static void update_rotation(struct omap_video_info *video_info)
{
	xf86CrtcPtr crtc = video_info->crtc;
	Rotation rotation = RR_Rotate_0;

	if (crtc) {
		xf86OutputPtr output = fbdev_crtc_get_output(crtc);
		/* If the display can't handle it we must */
		if (!fbdev_output_check_rotation(output, crtc->rotation))
			rotation = crtc->rotation;
	}
	video_info->crtc_rotation = rotation;

	/* Add up Xv and CRTC rotations */
	rotation = (video_info->rotation & 0xf) *
		(video_info->crtc_rotation & 0xf);
	while (!(rotation & 0xf))
		rotation >>= 4;

	/* Xv reflection */
	rotation |= video_info->rotation & ~0xf;

	/* CRTC reflection */
	switch (video_info->rotation & 0xf) {
	case RR_Rotate_0:
	case RR_Rotate_180:
		if (video_info->crtc_rotation & RR_Reflect_X)
			rotation ^= RR_Reflect_X;
		if (video_info->crtc_rotation & RR_Reflect_Y)
			rotation ^= RR_Reflect_Y;
		break;
	case RR_Rotate_90:
	case RR_Rotate_270:
		if (video_info->crtc_rotation & RR_Reflect_X)
			rotation ^= RR_Reflect_Y;
		if (video_info->crtc_rotation & RR_Reflect_Y)
			rotation ^= RR_Reflect_X;
		break;
	}

	video_info->rotate = fbdev_rr_to_omap_rotate(rotation);
	video_info->mirror = fbdev_rr_to_omap_mirror(rotation);

	video_info->dirty = TRUE;
}

void omap_video_update_crtc(ScrnInfoPtr pScrn,
			    xf86CrtcPtr crtc)
{
	FBDevPtr fbdev = pScrn->driverPrivate;
	struct omap_video_info *video_info;
	int i;

	for (i = 0; i < fbdev->num_video_ports; i++) {
		video_info = get_omap_video_info(fbdev, i);

		if (video_info->crtc != crtc)
			continue;

		update_rotation(video_info);
		update_alpha(video_info);
		set_overlay_alpha(video_info);
		video_info->dirty = TRUE;
		video_info->update_clone = TRUE;
	}
}

static void omap_fill_color_key(ScrnInfoPtr pScrn, DrawablePtr drawable,
		struct omap_video_info *video_info, RegionPtr clip_boxes)
{
	if (!video_info->autopaint_ckey)
		return;

	/* Display update will be schedule next */
	extfb_lock_display_update(pScrn);
	xf86XVFillKeyHelperPort(drawable, video_info, video_info->ckey,
				clip_boxes, video_info->changed_ckey);
	extfb_unlock_display_update(pScrn);
}

/**
 * XvPutImage hook.  This does not deal with rotation or partial updates.
 *
 * Calls out to omapCopyPlanarData (unobscured planar video),
 * omapExpandPlanarData (downscaled planar),
 * omapCopyPackedData (downscaled packed), xf86XVCopyPlanarData (obscured planar),
 * or xf86XVCopyPackedData (packed).
 */
static int omap_video_putimage(ScrnInfoPtr pScrn,
			       short src_x, short src_y,
			       short dst_x, short dst_y,
			       short src_w, short src_h,
			       short dst_w, short dst_h,
			       int id, unsigned char *buf,
			       short width, short height, Bool sync,
			       RegionPtr clip_boxes, pointer data,
			       DrawablePtr drawable)
{
	struct omap_video_info *video_info = (struct omap_video_info *)data;
	Bool enable = FALSE;
	int ret = Success;
	CARD8 *mem;
	xf86CrtcPtr crtc;
	RegionRec old;

	/*
	 * FIXME client's might not be prepared for errors
	 * and the whole Xv design doesn't really fit our
	 * shared overlays approach. What to do?
	 * - return an error?
	 * - steal the overlay from the CRTC?
	 * - tell the client everything is fine but skip the output?
	 * - get rid of the second Xv port?
	 */
	if (!get_overlay(video_info)) {
		/* FIXME error value? */
		ret = BadAlloc;
		goto out;
	}

	/* Allocate video memory */
	if (!alloc_mem(video_info, id, width, height)) {
		stop_video(video_info);
		put_overlay(video_info);
		ret = BadAlloc;
		goto out;
	}

	video_info->fourcc = id;
	video_info->width = width;
	video_info->height = height;
	video_info->drawable = drawable;

	/* Failure here means simply that there is nothing to draw */
	if (!clip_image_to_fit(pScrn,
			       &src_x, &src_y, &dst_x, &dst_y,
			       &src_w, &src_h, &dst_w, &dst_h,
			       width, height, video_info->rotation, clip_boxes,
			       video_info->desired_crtc, &crtc)) {
		DebugF("omap/putimage: skipping -\n"
		       " src %dx%d+%d+%d\n"
		       " dst %dx%d+%d+%d\n"
		       " img %dx%d\n",
		       src_w, src_h, src_x, src_y,
		       dst_w, dst_h, dst_x, dst_y,
		       width, height);
		crtc = NULL;
	}

	if (crtc != video_info->crtc) {
		clone_stop(video_info);
		omap_overlay_disable(video_info->ovl);
		video_info->dirty = TRUE;
		video_info->update_clone = TRUE;
		if (crtc && !omap_output_assign_overlay(get_out_from_crtc(crtc),
							video_info->ovl)) {
			stop_video(video_info);
			put_overlay(video_info);
			ret = BadImplementation;
			goto out;
		}
		video_info->crtc = crtc;
		update_rotation(video_info);
		update_alpha(video_info);
	}

	video_info->clone_crtc = get_clone_crtc(pScrn, video_info);

	if (!crtc) {
		/* Make sure we update the old position */
		push_update(pScrn, video_info, NULL);

		/* The old position is now clean */
		video_info->dst_x = 0;
		video_info->dst_y = 0;
		video_info->dst_w = 0;
		video_info->dst_h = 0;

		goto out;
	}

	init_update_region(pScrn, &old, video_info);

	if (is_dirty(video_info, id,
		     src_x, src_y, dst_x, dst_y,
		     src_w, src_h, dst_w, dst_h)) {
		if (!setup_overlay(pScrn, video_info,
				   id, width, height,
				   src_x, src_y, dst_x, dst_y,
				   src_w, src_h, dst_w, dst_h, drawable)) {
			ErrorF("omap/putimage: failed to set up overlay: "
			       "from %dx%d+%d+%d to %dx%d+%d+%d on plane %d\n",
			       src_w, src_h, src_x, src_y,
			       dst_w, dst_h, dst_x, dst_y, video_info->id);
			stop_video(video_info);
			put_overlay(video_info);
			/* FIXME error value? */
			ret = BadValue;
			goto out;
		}

#if 0
		DebugF("omap/putimage: "
		       "from %dx%d+%d+%d to %dx%d+%d+%d on plane %d\n",
		       src_w, src_h, src_x, src_y,
		       dst_w, dst_h, dst_x, dst_y, video_info->id);
#endif

		video_info->changed_ckey = TRUE;
		enable = TRUE;
		video_info->update_clone = TRUE;
	}

	/*
	 * If buf==NULL this is a ReputImage,
	 * so the fb already has the correct data.
	 */
	if (buf) {
		mem = calc_mem(video_info);

		if (video_info->vsync) {
			if (video_info->double_buffer) {
				clone_sync(pScrn);
				omap_overlay_wait(video_info->ovl);
			} else {
				sync_gfx(video_info);
			}
		}

		switch (id) {
		case FOURCC_RV32:
		case FOURCC_AV32:
			omap_copy_32(buf, mem,
				     OMAP_RV32_PITCH(width),
				     video_info->pitch,
				     width, height, 0, 0,
				     width, height);
			break;

		case FOURCC_RV12:
		case FOURCC_RV16:
		case FOURCC_AV12:
			omap_copy_16(buf, mem,
				     OMAP_RV16_PITCH(width),
				     video_info->pitch,
				     width, height, 0, 0,
				     width, height);
			break;

		case FOURCC_UYVY:
		case FOURCC_YUY2:
			omap_copy_packed(buf, mem,
					 OMAP_YUY2_PITCH(width),
					 video_info->pitch,
					 width, height, 0, 0,
					 width, height);
			break;

		case FOURCC_YV12:
		case FOURCC_I420:
			omap_copy_planar(buf, mem,
					 OMAP_YV12_PITCH_LUMA(width),
					 OMAP_YV12_PITCH_CHROMA(width),
					 video_info->pitch,
					 width, height, 0, 0,
					 width, height, id);
			break;
		}

		if (video_info->double_buffer)
			flip_plane(video_info);
	}

	rearm_ckey_timer(video_info);

	if (enable) {
		setup_colorkey(video_info, !video_info->disable_ckey);
		set_overlay_alpha(video_info);
		omap_overlay_enable(video_info->ovl);
	}

	omap_fill_color_key(pScrn, drawable, video_info, clip_boxes);

	push_update(pScrn, video_info, &old);
 out:
	clone_update(pScrn);

	return ret;
}

/**
 * ReputImage hook
 */
static int omap_video_reputimage(ScrnInfoPtr pScrn,
				 short src_x, short src_y,
				 short dst_x, short dst_y,
				 short src_w, short src_h,
				 short dst_w, short dst_h,
				 RegionPtr clip_boxes, pointer data,
				 DrawablePtr drawable)
{
	struct omap_video_info *video_info = (struct omap_video_info *)data;

	return omap_video_putimage(pScrn,
				   src_x, src_y, dst_x, dst_y,
				   src_w, src_h, dst_w, dst_h,
				   video_info->fourcc, NULL,
				   video_info->width, video_info->height,
				   FALSE, clip_boxes, data, drawable);
}

/**
 * XvPutVideo hook.
 */
static int omap_video_putvideo(ScrnInfoPtr pScrn,
			       short src_x, short src_y,
			       short dst_x, short dst_y,
			       short src_w, short src_h,
			       short dst_w, short dst_h,
			       RegionPtr clip_boxes, pointer data,
			       DrawablePtr drawable)
{
	struct omap_video_info *video_info = (struct omap_video_info *)data;
	Bool enable = FALSE;
	int ret = Success;
	unsigned int width, height;
	xf86CrtcPtr crtc;
	RegionRec old;

	if (!video_info->fbdev_reserved) {
		/* FIXME error value? */
		ret = BadAlloc;
		goto out;
	}

	if (!omap_fb_refresh_info(video_info->fb, video_info->ovl)) {
		stop_video(video_info);
		ret = BadImplementation;
		goto out;
	}

	if (!omap_fb_get_info(video_info->fb, &width, &height, NULL)) {
		stop_video(video_info);
		ret = BadImplementation;
		goto out;
	}

	video_info->fourcc = 0;
	video_info->width = width;
	video_info->height = height;
	video_info->drawable = drawable;

	if (video_info->dirty)
		enable = TRUE;

	/* Failure here means simply that there is nothing to draw */
	if (!clip_image_to_fit(pScrn,
			       &src_x, &src_y, &dst_x, &dst_y,
			       &src_w, &src_h, &dst_w, &dst_h,
			       width, height, video_info->rotation, clip_boxes,
			       video_info->desired_crtc, &crtc)) {
		DebugF("omap/putvideo: skipping -\n"
		       " src %dx%d+%d+%d\n"
		       " dst %dx%d+%d+%d\n"
		       " img %ux%u\n",
		       src_w, src_h, src_x, src_y,
		       dst_w, dst_h, dst_x, dst_y,
		       width, height);
		crtc = NULL;
	}

	if (crtc != video_info->crtc) {
		clone_stop(video_info);
		omap_overlay_disable(video_info->ovl);
		video_info->dirty = TRUE;
		video_info->update_clone = TRUE;
		enable = TRUE;
		if (crtc && !omap_output_assign_overlay(get_out_from_crtc(crtc),
							video_info->ovl)) {
			stop_video(video_info);
			ret = BadImplementation;
			goto out;
		}
		video_info->crtc = crtc;
		update_rotation(video_info);
		update_alpha(video_info);
	}

	video_info->clone_crtc = get_clone_crtc(pScrn, video_info);

	if (!crtc) {
		/* Make sure we update the old position */
		push_update(pScrn, video_info, NULL);

		/* The old position is now clean */
		video_info->dst_x = 0;
		video_info->dst_y = 0;
		video_info->dst_w = 0;
		video_info->dst_h = 0;

		goto out;
	}

	init_update_region(pScrn, &old, video_info);

	if (is_dirty(video_info, 0,
		     src_x, src_y, dst_x, dst_y,
		     src_w, src_h, dst_w, dst_h)) {
		if (!setup_overlay(pScrn, video_info,
				   0, width, height,
				   src_x, src_y, dst_x, dst_y,
				   src_w, src_h, dst_w, dst_h, drawable)) {
			ErrorF("omap/putvideo: failed to set up overlay: "
			       "from %dx%d+%d+%d to %dx%d+%d+%d on plane %d\n",
			       src_w, src_h, src_x, src_y,
			       dst_w, dst_h, dst_x, dst_y, video_info->id);
			stop_video(video_info);
			/* FIXME error value? */
			ret = BadValue;
			goto out;
		}
#if 0
		DebugF("omap/putvideo: "
		       "from %dx%d+%d+%d to %dx%d+%d+%d on plane %d\n",
		       src_w, src_h, src_x, src_y,
		       dst_w, dst_h, dst_x, dst_y, video_info->id);
#endif
		video_info->update_clone = TRUE;
	}

	rearm_ckey_timer(video_info);

	if (enable) {
		setup_colorkey(video_info, !video_info->disable_ckey);
		set_overlay_alpha(video_info);
		omap_overlay_enable(video_info->ovl);
	}

	omap_fill_color_key(pScrn, drawable, video_info, clip_boxes);

	push_update(pScrn, video_info, &old);
 out:
	clone_update(pScrn);

	return ret;
}

/**
 * Give image size and pitches.
 */
static int omap_video_query_attributes(ScrnInfoPtr pScrn, int id,
				       unsigned short *w_out,
				       unsigned short *h_out, int *pitches,
				       int *offsets)
{
	int size = 0, tmp = 0;
	int w, h;

	if (*w_out > DummyEncodings[0].width)
		*w_out = DummyEncodings[0].width;
	if (*h_out > DummyEncodings[0].height)
		*h_out = DummyEncodings[0].height;

	w = *w_out;
	h = *h_out;

	if (offsets)
		offsets[0] = 0;

	switch (id) {
	case FOURCC_I420:
	case FOURCC_YV12:
		w = ALIGN(w, 4);
		h = ALIGN(h, 2);
		size = w;
		if (pitches)
			pitches[0] = size;
		size *= h;
		if (offsets)
			offsets[1] = size;
		tmp = w >> 1;
		tmp = ALIGN(tmp, 4);
		if (pitches)
			pitches[1] = pitches[2] = tmp;
		tmp *= h >> 1;
		size += tmp;
		if (offsets)
			offsets[2] = size;
		size += tmp;
		break;
	case FOURCC_UYVY:
	case FOURCC_YUY2:
		w = ALIGN(w, 2);
		size = w << 1;
		if (pitches)
			pitches[0] = size;
		size *= h;
		break;
	case FOURCC_RV12:
	case FOURCC_RV16:
	case FOURCC_AV12:
		size = w << 1;
		if (pitches)
			pitches[0] = size;
		size *= h;
		break;
	case FOURCC_RV32:
	case FOURCC_AV32:
		size = w << 2;
		if (pitches)
			pitches[0] = size;
		size *= h;
		break;
	default:
		return 0;
	}

	return size;
}

static Bool omap_video_modify_encoding(FBDevPtr fbdev)
{
	bool r;
	unsigned int vram, max_w, max_h;
	struct omap_video_info *video_info;

	ENTER();

	video_info = get_omap_video_info(fbdev, 0);

	if (!get_overlay(video_info))
		return FALSE;

	r = omap_fb_get_size_info(video_info->fb,
				    NULL, NULL, &vram);

	put_overlay(video_info);

	if (!r)
		return FALSE;

	/* Calculate the max image size (of a certain aspect ratio) which will
	 * fit into the available memory. */

	/* FIXME this should be made configurable. */

	vram /= MAX_PLANES;
	vram -= vram % getpagesize();
	vram /= MAX_BYTES_PER_PIXEL * MAX_BUFFERS;

	/* Try 16:9 first. */
	max_h = sqrt(9 * vram / 16);
	max_w = 16 * max_h / 9;

	/* Only use 16:9 if 720p is possible, otherwise fall back to 4:3. */
	if (max_w < 1280 || max_h < 720) {
		max_h = sqrt(3 * vram / 4);
		max_w = 4 * max_h / 3;
	}

	/* Hardware limits */
	if (max_w > VIDEO_IMAGE_MAX_WIDTH) {
		max_h =
			min(VIDEO_IMAGE_MAX_WIDTH, vram / VIDEO_IMAGE_MAX_WIDTH);
		max_w = VIDEO_IMAGE_MAX_WIDTH;
	}
	if (max_h > VIDEO_IMAGE_MAX_HEIGHT) {
		max_h = VIDEO_IMAGE_MAX_HEIGHT;
		max_w =
			min(VIDEO_IMAGE_MAX_HEIGHT, vram / VIDEO_IMAGE_MAX_HEIGHT);
	}

	/* Only full macropixels */
	max_w &= ~1;

	DummyEncodings[0].width = max_w;
	DummyEncodings[0].height = max_h;

	/* FIXME should clip max_h * MAX_BUFFERS w/ VRFB */
	DummyEncodings[1].width = max_w;
	DummyEncodings[1].height = max_h * MAX_BUFFERS;

	LEAVE();

	return TRUE;
}

static Bool omap_video_setup_private(ScreenPtr pScreen,
				     struct omap_video_info *video_info,
				     int i, struct omap_fb *fb)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	FBDevPtr fbdev = pScrn->driverPrivate;

	ENTER();

	video_info->fb = fb;

	video_info->id = i;
	video_info->state = OMAP_STATE_STOPPED;
	video_info->fbdev = fbdev;

	video_info->double_buffer = TRUE;
	video_info->vsync = TRUE;
	video_info->autopaint_ckey = TRUE;
	video_info->disable_ckey = FALSE;
	video_info->ckey = default_ckey(pScrn);
	video_info->overlay_alpha = 255;
	video_info->clone = TRUE;
	video_info->rotation = RR_Rotate_0;

	LEAVE();

	return TRUE;
}

static void omap_video_free_adaptor(FBDevPtr fbdev, XF86VideoAdaptorPtr adapt)
{
	int i;
	struct omap_video_info *video_info;

	if (adapt->pPortPrivates)
		for (i = 0; i < fbdev->num_video_ports; ++i) {
			video_info = adapt->pPortPrivates[i].ptr;
			free(video_info);
		}
	free(adapt->pPortPrivates);
	free(adapt);
}

/**
 * Set up all our internal structures.
 */
static XF86VideoAdaptorPtr
omap_video_setup_adaptor(ScreenPtr pScreen,
			 unsigned int num_video_ports, va_list ap)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	FBDevPtr fbdev = pScrn->driverPrivate;
	XF86VideoAdaptorPtr adapt;
	int i;
	struct omap_video_info *video_info;

	/* No usable video overlays. */
	if (!num_video_ports)
		return NULL;

	if (!(adapt = calloc(1, sizeof(XF86VideoAdaptorRec))))
		return NULL;

	adapt->type = XvWindowMask | XvInputMask | XvImageMask | XvVideoMask;
	adapt->flags = VIDEO_OVERLAID_IMAGES;
	/* VIDEO_CLIP_TO_VIEWPORT is not compatible with multiple CRTCs */
	adapt->name = "OMAP Video Overlay";
	adapt->nEncodings = ARRAY_SIZE(DummyEncodings);
	adapt->pEncodings = DummyEncodings;

	adapt->nFormats = ARRAY_SIZE(xv_formats);
	adapt->pFormats = xv_formats;

	adapt->nAttributes = ARRAY_SIZE(xv_attributes);
	adapt->pAttributes = xv_attributes;

	adapt->nImages = ARRAY_SIZE(xv_images);
	adapt->pImages = xv_images;

	adapt->PutImage = omap_video_putimage;
	adapt->ReputImage = omap_video_reputimage;
	adapt->PutVideo = omap_video_putvideo;
	adapt->StopVideo = omap_video_stop;
	adapt->GetPortAttribute = omap_video_get_attribute;
	adapt->SetPortAttribute = omap_video_set_attribute;
	adapt->QueryBestSize = omap_video_query_best_size;
	adapt->QueryImageAttributes = omap_video_query_attributes;

	adapt->pPortPrivates = (DevUnion *)
		calloc(num_video_ports, sizeof(DevUnion));
	if (!adapt->pPortPrivates)
		goto unwind;

	for (i = 0; i < num_video_ports; i++) {
		struct omap_fb *fb = va_arg(ap, struct omap_fb *);
		if (!fb)
			goto unwind;

		video_info = calloc(1, sizeof(struct omap_video_info));
		if (!video_info)
			goto unwind;

		if (!omap_video_setup_private(pScreen, video_info, i, fb))
			goto unwind;

		adapt->pPortPrivates[i].ptr = (pointer) video_info;
		adapt->nPorts++;
	}

	fbdev->num_video_ports = num_video_ports;
	fbdev->overlay_adaptor = adapt;

	/* Modify the encoding to contain our real min/max values. */
	if (!omap_video_modify_encoding(fbdev))
		goto unwind;

	xv_crtc = MAKE_ATOM("XV_CRTC");
	xv_ckey = MAKE_ATOM("XV_COLORKEY");
	xv_autopaint_ckey = MAKE_ATOM("XV_AUTOPAINT_COLORKEY");
	xv_disable_ckey = MAKE_ATOM("XV_DISABLE_COLORKEY");
	xv_double_buffer = MAKE_ATOM("XV_DOUBLE_BUFFER");
	xv_vsync = MAKE_ATOM("XV_SYNC_TO_VBLANK");
	xv_omap_fbdev_num = MAKE_ATOM("XV_OMAP_FBDEV_NUM");
	xv_overlay_alpha = MAKE_ATOM("XV_OVERLAY_ALPHA");
	xv_clone_fullscreen = MAKE_ATOM("XV_CLONE_FULLSCREEN");
	xv_omap_fbdev_reserve = MAKE_ATOM("XV_OMAP_FBDEV_RESERVE");
	xv_omap_fbdev_sync = MAKE_ATOM("XV_OMAP_FBDEV_SYNC");
	xv_stacking = MAKE_ATOM("XV_STACKING");
	xv_rotation = MAKE_ATOM("XV_ROTATION");
	_omap_video_overlay = MAKE_ATOM("_OMAP_VIDEO_OVERLAY");

	return adapt;

 unwind:
	omap_video_free_adaptor(fbdev, adapt);

	fbdev->num_video_ports = 0;
	fbdev->overlay_adaptor = NULL;

	return NULL;
}

Bool fbdev_init_video(ScreenPtr pScreen, unsigned int num_video_ports, ...)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	FBDevPtr fbdev = pScrn->driverPrivate;
	XF86VideoAdaptorPtr *adaptors = NULL, adaptor;
	int i = 0;
	va_list ap;

	fbdev->screen = pScreen;

	adaptors = realloc(adaptors, (i + 1) * sizeof(XF86VideoAdaptorPtr));
	if (!adaptors)
		return FALSE;

	va_start(ap, num_video_ports);

	adaptor = omap_video_setup_adaptor(pScreen, num_video_ports, ap);
	if (adaptor) {
		adaptors[i] = adaptor;
		i++;
	}

	va_end(ap);

	adaptors = realloc(adaptors, (i + 1) * sizeof(XF86VideoAdaptorPtr));
	if (!adaptors)
		return FALSE;

	if ((adaptor = pvr2dSetupTexturedVideo(pScreen))) {
		adaptors[i] = adaptor;
		i++;
	}

	xf86XVScreenInit(pScreen, adaptors, i);

	/* Hook drawable destruction, so we can ignore them if they go away. */
	fbdev->video_destroy_pixmap = pScreen->DestroyPixmap;
	pScreen->DestroyPixmap = destroy_pixmap_hook;
	fbdev->video_destroy_window = pScreen->DestroyWindow;
	pScreen->DestroyWindow = destroy_window_hook;

	if (adaptors)
		free(adaptors);

	return TRUE;
}

/**
 * Shut down Xv, also used on regeneration.  All videos should be stopped
 * by the time we get here.
 */
void fbdev_fini_video(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	FBDevPtr fbdev = pScrn->driverPrivate;

	pScreen->DestroyPixmap = fbdev->video_destroy_pixmap;
	pScreen->DestroyWindow = fbdev->video_destroy_window;
	fbdev->video_destroy_pixmap = NULL;
	fbdev->video_destroy_window = NULL;

	omap_video_free_adaptor(fbdev, fbdev->overlay_adaptor);
	fbdev->overlay_adaptor = NULL;
	fbdev->num_video_ports = 0;
}
