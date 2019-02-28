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

#include <xf86.h>
#include <xf86Crtc.h>
#include <X11/extensions/dpmsconst.h>

#include "fbdev.h"
#include "omap_tvout.h"
#include "omap_video.h"
#include "omap.h"

void fbdev_flip_crtcs(ScrnInfoPtr pScrn,
		      unsigned int page_scan_next)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	int i;

	fPtr->page_scan_next = page_scan_next;

	for (i = 0; i < config->num_output; i++) {
		xf86OutputPtr output = config->output[i];
		xf86CrtcPtr crtc = output->crtc;
		struct fbdev_crtc *crtc_priv;
		struct fbdev_output *output_priv;

		if (!crtc)
			continue;

		crtc_priv = crtc->driver_private;
		output_priv = output->driver_private;

		if (!crtc->enabled || crtc_priv->dpms != DPMSModeOn)
			continue;

		if (output_priv->xv_clone_fullscreen &&
		    omap_video_clone_active(pScrn, crtc))
			continue;

		omap_overlay_pan(crtc_priv->ovl, page_scan_next,
				 crtc->x, crtc->y,
				 crtc_priv->sw, crtc_priv->sh);
	}
}

xf86OutputPtr
fbdev_crtc_get_output(xf86CrtcPtr crtc)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
	int i;

	for (i = 0; i < config->num_output; i++) {
		xf86OutputPtr output = config->output[i];

		if (output->crtc != crtc)
			continue;

		return output;
	}

	return NULL;
}

enum omap_mirror
fbdev_rr_to_omap_mirror(Rotation rotation)
{
	enum omap_mirror mirror = OMAP_MIRROR_NONE;

	/* RR reflection is before rotation, omap mirroring is after rotation */
	switch (rotation & 0xf) {
	case RR_Rotate_0:
	case RR_Rotate_180:
		if (rotation & RR_Reflect_X)
			mirror |= OMAP_MIRROR_HORZ;
		if (rotation & RR_Reflect_Y)
			mirror |= OMAP_MIRROR_VERT;
		break;
	case RR_Rotate_90:
	case RR_Rotate_270:
		if (rotation & RR_Reflect_X)
			mirror |= OMAP_MIRROR_VERT;
		if (rotation & RR_Reflect_Y)
			mirror |= OMAP_MIRROR_HORZ;
		break;
	default:
		assert(0);
	}

	return mirror;
}

enum omap_rotate
fbdev_rr_to_omap_rotate(Rotation rotation)
{
	/* RR angle is counter clockwise, omap angle is clockwise */
	switch (rotation & 0xf) {
	case RR_Rotate_0:
		return OMAP_ROTATE_0;
	case RR_Rotate_90:
		return OMAP_ROTATE_270;
	case RR_Rotate_180:
		return OMAP_ROTATE_180;
	case RR_Rotate_270:
		return OMAP_ROTATE_90;
	default:
		assert(0);
	}
}

void fbdev_crtc_output_coords(xf86CrtcPtr crtc,
			      const BoxRec *box,
			      unsigned int *ret_x,
			      unsigned int *ret_y,
			      unsigned int *ret_w,
			      unsigned int *ret_h)
{
	xf86OutputPtr output = fbdev_crtc_get_output(crtc);
	struct fbdev_crtc *priv = crtc->driver_private;
	Rotation rotation = crtc->rotation;
	unsigned int x, y, w, h;
	unsigned int tx, ty, tw, th;

	x = box->x1 - crtc->x;
	y = box->y1 - crtc->y;
	w = box->x2 - box->x1;
	h = box->y2 - box->y1;

	/* Check if the display can handle mirroring/rotation */
	if (fbdev_output_check_rotation(output, rotation))
		rotation = RR_Rotate_0;

	if (rotation & RR_Reflect_X)
		x = xf86ModeWidth(&crtc->mode, rotation) - x - w;
	if (rotation & RR_Reflect_Y)
		y = xf86ModeHeight(&crtc->mode, rotation) - y - h;

	tx = x;
	ty = y;
	tw = w;
	th = h;

	switch (rotation & 0xf) {
	case RR_Rotate_0:
		x = tx;
		y = ty;
		w = tw;
		h = th;
		break;
	case RR_Rotate_90:
		x = ty;
		y = crtc->mode.VDisplay - tx - tw;
		w = th;
		h = tw;
		break;
	case RR_Rotate_180:
		x = crtc->mode.HDisplay - tx - tw;
		y = crtc->mode.VDisplay - ty - th;
		w = tw;
		h = th;
		break;
	case RR_Rotate_270:
		x = crtc->mode.HDisplay - ty - th;
		y = tx;
		w = th;
		h = tw;
		break;
	}

	switch (rotation & 0xf) {
	case RR_Rotate_0:
	case RR_Rotate_180:
		*ret_w = w * priv->dw / priv->sw;
		*ret_h = h * priv->dh / priv->sh;
		*ret_x = x * priv->dw / priv->sw + priv->dx;
		*ret_y = y * priv->dh / priv->sh + priv->dy;
		break;
	case RR_Rotate_90:
	case RR_Rotate_270:
		*ret_w = w * priv->dw / priv->sh;
		*ret_h = h * priv->dh / priv->sw;
		*ret_x = x * priv->dw / priv->sh + priv->dx;
		*ret_y = y * priv->dh / priv->sw + priv->dy;
		break;
	}
}

static void
put_overlay(xf86CrtcPtr crtc)
{
	struct fbdev_crtc *priv = crtc->driver_private;

	if (!priv->ovl)
		return;

	fbdev_put_overlay(crtc->scrn, priv->ovl);
	priv->ovl = NULL;
}

static Bool
get_overlay(xf86CrtcPtr crtc)
{
	struct fbdev_crtc *priv = crtc->driver_private;
	FBDevPtr fPtr = FBDEVPTR(crtc->scrn);

	if (priv->ovl)
		return TRUE;

	priv->ovl = fbdev_get_overlay(crtc->scrn, priv->usage);
	if (!priv->ovl)
		return FALSE;

	if (!omap_fb_assign_overlay(fPtr->fb[0], priv->ovl)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Unable to assign overlay to an output\n");
		put_overlay(crtc);
		return FALSE;
	}

	return TRUE;
}

static void
crtc_dpms(xf86CrtcPtr crtc, int mode)
{
	struct fbdev_crtc *priv = crtc->driver_private;

	DebugF("%s(crtc=%p, mode=%d)\n",
	       __func__, crtc, mode);

	DebugF("%s crtc->enabled = %u\n",
	       __func__, crtc->enabled);

	priv->dpms = mode;

	if (mode != DPMSModeOn) {
		if (!priv->ovl)
			return;

		if (!omap_overlay_disable(priv->ovl)) {
			xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				   "Unable to disable overlay\n");
			return;
		}

		/*
		 * Hopefully the dpms() hook gets called every time
		 * the CRTC is not used anymore. Otherwise we'll hoard the
		 * overlay indefinitely.
		 */
		if (mode == DPMSModeOff && !crtc->enabled)
			put_overlay(crtc);
	} else {
		if (!get_overlay(crtc)) {
			xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				   "No suitable overlay available for CRTC\n");
			return;
		}

		if (!omap_overlay_enable(priv->ovl))
			xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				   "Unable to enable overlay\n");
	}
}

static void
crtc_destroy(xf86CrtcPtr crtc)
{
	DebugF("%s(crtc=%p)\n",
	       __func__, crtc);

	if (crtc->driver_private)
		free(crtc->driver_private);
}

static void output_calc(Rotation rotation,
			unsigned int hdisp,
			unsigned int vdisp,
			unsigned int mode_w,
			unsigned int mode_h,
			unsigned int *ret_x,
			unsigned int *ret_y,
			unsigned int *ret_w,
			unsigned int *ret_h)
{
	unsigned int x, y, w, h;

	x = (hdisp - mode_w) >> 1;
	y = (vdisp - mode_h) >> 1;
	w = mode_w;
	h = mode_h;

	switch (rotation & 0xf) {
	case RR_Rotate_0:
	case RR_Rotate_180:
		if (rotation & RR_Reflect_X)
			x = hdisp - x - w;
		if (rotation & RR_Reflect_Y)
			y = vdisp - y - h;
		break;
	case RR_Rotate_90:
	case RR_Rotate_270:
		if (rotation & RR_Reflect_X)
			y = vdisp - y - h;
		if (rotation & RR_Reflect_Y)
			x = hdisp - x - w;
		break;
	}

	switch (rotation & 0xf) {
	case RR_Rotate_0:
		*ret_x = x;
		*ret_y = y;
		*ret_w = w;
		*ret_h = h;
		break;
	case RR_Rotate_90:
		*ret_x = vdisp - y - h;
		*ret_y = x;
		*ret_w = h;
		*ret_h = w;
		break;
	case RR_Rotate_180:
		*ret_x = hdisp - x - w;
		*ret_y = vdisp - y - h;
		*ret_w = w;
		*ret_h = h;
		break;
	case RR_Rotate_270:
		*ret_x = y;
		*ret_y = hdisp - x - w;
		*ret_w = h;
		*ret_h = w;
		break;
	}
}

static Bool
crtc_set_mode_major(xf86CrtcPtr crtc,
		    DisplayModePtr mode,
		    Rotation rotation,
		    int x, int y)
{
	FBDevPtr fPtr = FBDEVPTR(crtc->scrn);
	struct fbdev_crtc *priv = crtc->driver_private;
	xf86OutputPtr output = fbdev_crtc_get_output(crtc);
	struct fbdev_output *output_priv = output->driver_private;
	unsigned int buffer;
	unsigned int sx, sy, sw, sh;
	unsigned int dx, dy, dw, dh;
	unsigned int aspectw, aspecth;
	enum omap_rotate out_rotate = OMAP_ROTATE_0;
	enum omap_rotate ovl_rotate = OMAP_ROTATE_0;
	enum omap_mirror out_mirror = OMAP_MIRROR_NONE;
	enum omap_mirror ovl_mirror = OMAP_MIRROR_NONE;
	CARD32 wss;
	int dpms;

	buffer = fPtr->page_scan_next;

	DebugF("%s(crtc=%p, mode=%p, rotation=%d, x=%d, y=%d)\n",
	       __func__, crtc, mode, rotation, x, y);

	DebugF(" mode = %dx%d\n",
	       mode->HDisplay, mode->VDisplay);

	if (!crtc->enabled) {
		DebugF("CRTC is disabled\n");
		return TRUE;
	}

	dpms = priv->dpms;
	priv->dpms = DPMSModeOn;

	if (!get_overlay(crtc)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "No suitable overlay available for CRTC\n");
		goto error;
	}

	sx = x;
	sy = y;
	aspectw = sw = xf86ModeWidth(mode, rotation);
	aspecth = sh = xf86ModeHeight(mode, rotation);

	if (fbdev_output_check_rotation(output, rotation)) {
		out_mirror = fbdev_rr_to_omap_mirror(rotation);
		out_rotate = fbdev_rr_to_omap_rotate(rotation);
	} else {
		ovl_mirror = fbdev_rr_to_omap_mirror(rotation);
		ovl_rotate = fbdev_rr_to_omap_rotate(rotation);
		rotation = RR_Rotate_0;
	}

	if (!output_priv->xv_clone_fullscreen ||
	    !omap_video_clone(crtc->scrn, crtc, &buffer,
			      &sx, &sy, &sw, &sh,
			      &aspectw, &aspecth)) {
		if (omap_overlay_get_fb(priv->ovl) != fPtr->fb[0]) {
			/*
			 * Trick to make Xv<->CRTC clone switching smoother.
			 * set_mode_major will re-enable the overlay later.
			 */
			omap_overlay_disable(priv->ovl);

			if (!omap_fb_assign_overlay(fPtr->fb[0], priv->ovl)) {
				xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
					   "Unable to assign overlay "
					   "to a framebuffer\n");
				goto error_put_overlay;
			}
		}
	}

	/* FIXME proper error handling? */
	/*
	 * fbdev_output_update_timings() doesn't work without
	 * an overlay, so assign one before calling
	 * fbdev_output_update_timings().
	 */
	if (!omap_output_assign_overlay(output_priv->out, priv->ovl)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Unable to assign overlay to an output\n");
		goto error_put_overlay;
	}

	/*
	 * Set the TV standard before calling fbdev_output_update_timings()
	 * as changing the TV standard changes the timings.
	 */
	if (dpms != DPMSModeOn &&
	    output_priv->type == FBDEV_OUTPUT_TYPE_TV) {
		if (!omap_output_tv_standard(output_priv->out,
					     output_priv->tv_std)) {
			xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				   "Unable to set TV standard\n");
			goto error_put_overlay;
		}
	}

	if (dpms != DPMSModeOn) {
		if (!fbdev_output_update_timings(output))
			goto error_put_overlay;
	}

	if (output_priv->type == FBDEV_OUTPUT_TYPE_TV) {
		omap_tvout_calc_scaling(output_priv->widescreen,
					output_priv->tv_std,
					output_priv->scale,
					aspectw, aspecth,
					&dx, &dy, &dw, &dh, &wss,
					output_priv->hdisp,
					output_priv->vdisp,
					output_priv->xoffset - 1,
					output_priv->yoffset - 1,
					output_priv->dynamic_aspect);
	} else {
		/* FIXME scaling modes? */

		output_calc(rotation, output_priv->hdisp, output_priv->vdisp,
			    mode->HDisplay, mode->VDisplay,
			    &dx, &dy, &dw, &dh);
	}

	priv->sw = sw;
	priv->sh = sh;
	priv->dx = dx;
	priv->dy = dy;
	priv->dw = dw;
	priv->dh = dh;

	if (!omap_output_setup(output_priv->out, out_mirror, out_rotate)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Unable to setup output\n");
		goto error_put_overlay;
	}

	if (!omap_output_alpha_blending(output_priv->out,
					output_priv->alpha_mode)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Unable to set output alpha blending mode\n");
		goto error_put_overlay;
	}

	if (!omap_overlay_setup(priv->ovl, buffer,
				sx, sy, sw, sh,
				dx, dy, dw, dh,
				ovl_mirror, ovl_rotate)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Unable to setup overlay\n");
		goto error_put_overlay;
	}

	if (!omap_overlay_global_alpha(priv->ovl,
				       output_priv->graphics_alpha)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Unable to set overlay global alpha\n");
		goto error_put_overlay;
	}

	/*
	 * Enable regardlerss of the DPMS mode since the Xv<->CRTC clone
	 * swithcing will temporarily disable the overlay to avoid visible
	 * glitches.
	 */
	if (!omap_overlay_enable(priv->ovl)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Unable to enable overlay\n");
		goto error_put_overlay;
	}

	if (dpms != DPMSModeOn &&
	    !omap_output_enable(output_priv->out)) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
			   "Unable to enable output\n");
		if (!omap_overlay_disable(priv->ovl))
			xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				   "Unable to disable overlay\n");
		goto error_put_overlay;
	}

	omap_video_update_crtc(crtc->scrn, crtc);

	if (output_priv->type == FBDEV_OUTPUT_TYPE_TV)
		omap_output_wss(output_priv->out, wss);

	/*
	 * Update the full output to get rid of garbage on the
	 * edges in case the new mode is smaller than the old one.
	 */
	switch (rotation & 0xf) {
	case RR_Rotate_0:
	case RR_Rotate_180:
		dw = output_priv->hdisp;
		dh = output_priv->vdisp;
		break;
	case RR_Rotate_90:
	case RR_Rotate_270:
		dw = output_priv->vdisp;
		dh = output_priv->hdisp;
		break;
	}
	omap_output_update(output_priv->out, 0, 0, dw, dh);

	return TRUE;

 error_put_overlay:
	omap_overlay_disable(priv->ovl);
	put_overlay(crtc);
 error:
	priv->dpms = DPMSModeOff;

	return FALSE;
}

static void
crtc_set_origin(xf86CrtcPtr crtc, int x, int y)
{
	FBDevPtr fPtr = FBDEVPTR(crtc->scrn);
	struct fbdev_crtc *priv = crtc->driver_private;
	xf86OutputPtr output = fbdev_crtc_get_output(crtc);
	struct fbdev_output *output_priv = output->driver_private;

	DebugF("%s(crtc=%p, x=%d, y=%d)\n",
	       __func__, crtc, x, y);

	if (output_priv->xv_clone_fullscreen &&
	    omap_video_clone_active(crtc->scrn, crtc))
		return;

	omap_overlay_pan(priv->ovl, fPtr->page_scan_next,
			 x, y, priv->sw, priv->sh);

	omap_video_update_crtc(crtc->scrn, crtc);

	omap_output_update(output_priv->out,
			   priv->dx, priv->dy, priv->dw, priv->dh);
}

static const xf86CrtcFuncsRec fbdev_crtc_funcs = {
	.dpms                = crtc_dpms,
	.destroy             = crtc_destroy,
	.set_mode_major      = crtc_set_mode_major,
	.set_origin          = crtc_set_origin,
};

xf86CrtcPtr
fbdev_crtc_create(ScrnInfoPtr pScrn,
		  enum fbdev_overlay_usage usage)
{
	struct fbdev_crtc *priv;
	xf86CrtcPtr crtc;

	DebugF("%s(pScrn=%p)\n",
	       __func__, pScrn);

	priv = calloc(1, sizeof *priv);
	if (!priv)
		return NULL;

	crtc = xf86CrtcCreate(pScrn, &fbdev_crtc_funcs);
	if (!crtc) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Unable to create CRTC\n");
		free(priv);
		return NULL;
	}

	priv->usage = usage;
	priv->dpms = DPMSModeOff;

	crtc->driver_private = priv;

	return crtc;
}
