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

#include <X11/Xatom.h>
#include <xf86.h>
#include <xf86Crtc.h>
#include <X11/extensions/dpmsconst.h>

#include "omap.h"

#include "omap_video.h"
#include "fbdev.h"

static Atom prop_xv_clone_fullscreen;
static Atom prop_tv_signal_format;
static Atom prop_tv_signal_properties;
static Atom prop_tv_aspect_ratio;
static Atom prop_tv_scale;
static Atom prop_tv_xoffset;
static Atom prop_tv_yoffset;
static Atom prop_tv_dynamic_aspect_ratio;
static Atom prop_alpha_mode;
static Atom prop_graphics_alpha;
static Atom prop_video_alpha;

static Atom signal_format_composite_pal;
static Atom signal_format_composite_ntsc;
static Atom signal_properties_pal;
static Atom signal_properties_ntsc;
static Atom aspect_ratio_4_3;
static Atom aspect_ratio_16_9;

static Bool intersect_box(BoxPtr box1, const BoxRec *box2)
{
	box1->x1 = max(box1->x1, box2->x1);
	box1->x2 = min(box1->x2, box2->x2);
	box1->y1 = max(box1->y1, box2->y1);
	box1->y2 = min(box1->y2, box2->y2);

	return box1->x1 < box1->x2 && box1->y1 < box1->y2;
}

void fbdev_update_outputs(ScrnInfoPtr pScrn,
			  const BoxRec *update_box)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	int i;

	for (i = 0; i < config->num_output; i++) {
		xf86OutputPtr output = config->output[i];
		xf86CrtcPtr crtc = output->crtc;
		struct fbdev_crtc *crtc_priv;
		struct fbdev_output *output_priv;
		BoxRec crtc_box;
		enum omap_output_update update = OMAP_OUTPUT_UPDATE_AUTO;
		unsigned int x, y, w, h;

		if (!crtc)
			continue;

		crtc_priv = crtc->driver_private;
		output_priv = output->driver_private;

		if (!crtc->enabled || crtc_priv->dpms != DPMSModeOn)
			continue;

		if (output_priv->xv_clone_fullscreen &&
		    omap_video_clone_active(pScrn, crtc))
			continue;

		omap_output_get_update_mode(output_priv->out, &update);
		if (update != OMAP_OUTPUT_UPDATE_MANUAL)
			continue;

		crtc_box.x1 = crtc->x;
		crtc_box.x2 = crtc->x +
			xf86ModeWidth(&crtc->mode, crtc->rotation);
		crtc_box.y1 = crtc->y;
		crtc_box.y2 = crtc->y +
			xf86ModeHeight(&crtc->mode, crtc->rotation);

		if (update_box && !intersect_box(&crtc_box, update_box))
			continue;

		fbdev_crtc_output_coords(crtc, &crtc_box, &x, &y, &w, &h);

		omap_output_update(output_priv->out, x, y, w, h);
	}
}

Bool fbdev_output_check_rotation(xf86OutputPtr output, Rotation rotation)
{
	struct fbdev_output *priv = output->driver_private;
	enum omap_mirror mirror = fbdev_rr_to_omap_mirror(rotation);
	enum omap_rotate rotate = fbdev_rr_to_omap_rotate(rotation);

	return omap_output_check(priv->out, mirror, rotate);
}

Bool fbdev_output_update_timings(xf86OutputPtr output)
{
	struct fbdev_output *priv = output->driver_private;

	if (!omap_output_get_timings(priv->out, &priv->pixclk,
				     &priv->hdisp, &priv->hfp,
				     &priv->hsw, &priv->hbp,
				     &priv->vdisp, &priv->vfp,
				     &priv->vsw, &priv->vbp)) {
		xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
			   "Unable to get output timings\n");
		return FALSE;
	}

	/* update offsets in case the resolution changed */
	priv->xoffset = min(priv->xoffset, priv->hdisp);
	priv->yoffset = min(priv->yoffset, priv->vdisp);

	return TRUE;
}

static void update_output(xf86OutputPtr output)
{
	xf86CrtcPtr crtc = output->crtc;
	struct fbdev_crtc *priv;

	if (!crtc)
		return;

	priv = crtc->driver_private;

	if (!crtc->enabled || priv->dpms != DPMSModeOn)
		return;

	crtc->funcs->set_mode_major(crtc, &crtc->mode,
				    crtc->rotation,
				    crtc->x, crtc->y);
}

static void restart_output(xf86OutputPtr output)
{
	xf86CrtcPtr crtc = output->crtc;
	struct fbdev_crtc *priv;

	if (!crtc)
		return;

	priv = crtc->driver_private;

	if (!crtc->enabled || priv->dpms != DPMSModeOn)
		return;

	output->funcs->dpms(output, DPMSModeOff);
	crtc->funcs->dpms(crtc, DPMSModeOff);
	crtc->funcs->set_mode_major(crtc, &crtc->mode,
				    crtc->rotation,
				    crtc->x, crtc->y);
}

static void change_property_atom(xf86OutputPtr output,
				 Atom prop, Atom value)
{
	int r;

	r = RRChangeOutputProperty(output->randr_output, prop,
				   XA_ATOM, 32, PropModeReplace,
				   1, &value, FALSE, FALSE);
	if (r)
		xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
			   "Failed to change output property\n");
}

static void create_property_atom(xf86OutputPtr output,
				 Atom prop, Atom value)
{
	int r;

	r = RRConfigureOutputProperty(output->randr_output, prop,
				      FALSE, FALSE, TRUE, 0, NULL);
	if (r) {
		xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
			   "Failed to create output property\n");
		return;
	}

	change_property_atom(output, prop, value);
}

static void change_property_range(xf86OutputPtr output,
				  Atom prop, INT32 value)
{
	int r;
	r = RRChangeOutputProperty(output->randr_output, prop,
				   XA_INTEGER, 32, PropModeReplace,
				   1, &value, FALSE, FALSE);
	if (r)
		xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
			   "Failed to change output property\n");
}

static void create_property_range(xf86OutputPtr output,
				  Atom prop, INT32 range[2], INT32 value)
{
	int r;

	r = RRConfigureOutputProperty(output->randr_output, prop,
				      FALSE, TRUE, FALSE, 2, range);
	if (r) {
		xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
			   "Failed to create output property\n");
		return;
	}

	change_property_range(output, prop, value);
}

static void output_create_resources(xf86OutputPtr output)
{
	struct fbdev_output *priv = output->driver_private;
	INT32 range[2];
	Atom prop, value;

	DebugF("%s(output=%p)\n",
	       __func__, output);

	if (priv->type == FBDEV_OUTPUT_TYPE_LCD) {
		prop = MAKE_ATOM(RR_PROPERTY_CONNECTOR_TYPE);
		value = MAKE_ATOM("Panel");
		create_property_atom(output, prop, value);

		prop = MAKE_ATOM(RR_PROPERTY_SIGNAL_FORMAT);
		value = MAKE_ATOM("LVDS");
		create_property_atom(output, prop, value);
	}

	if (priv->type == FBDEV_OUTPUT_TYPE_TV) {
		prop = MAKE_ATOM(RR_PROPERTY_CONNECTOR_TYPE);
		value = MAKE_ATOM("TV-Composite");
		create_property_atom(output, prop, value);

		prop_tv_signal_format = MAKE_ATOM(RR_PROPERTY_SIGNAL_FORMAT);
		signal_format_composite_pal = MAKE_ATOM("Composite-PAL");
		signal_format_composite_ntsc = MAKE_ATOM("Composite-NTSC");
		create_property_atom(output, prop_tv_signal_format,
				     signal_format_composite_pal);

		prop_tv_signal_properties =
			MAKE_ATOM(RR_PROPERTY_SIGNAL_PROPERTIES);
		signal_properties_pal = MAKE_ATOM("PAL");
		signal_properties_ntsc = MAKE_ATOM("NTSC");
		create_property_atom(output, prop_tv_signal_properties,
				     signal_properties_pal);

		prop_tv_aspect_ratio = MAKE_ATOM("TVAspectRatio");
		aspect_ratio_4_3 = MAKE_ATOM("4:3");
		aspect_ratio_16_9 = MAKE_ATOM("16:9");
		create_property_atom(output, prop_tv_aspect_ratio,
				     aspect_ratio_4_3);

		prop_tv_scale = MAKE_ATOM("TVScale");
		range[0] = 0;
		range[1] = 100;
		create_property_range(output, prop_tv_scale, range, 90);

		/*
		 * This property determines if the XV_CLONE_FULLSCREEN Xv
		 * attribute can take over this output. So each Xv client can
		 * decide if they want to use the cloning feature and some
		 * policy agent can prevent them from doing so if necessary.
		 */
		prop_xv_clone_fullscreen =
			MAKE_ATOM("XvCloneFullscreen");
		range[0] = 0;
		range[1] = 1;
		create_property_range(output, prop_xv_clone_fullscreen, range,
				      priv->type == FBDEV_OUTPUT_TYPE_TV);

		/*
		 * -1 apparently can't be used as a value, so:
		 * 0 == use default offset
		 * 1..max == 0..max-1
		 */
		prop_tv_xoffset = MAKE_ATOM("TVXOffset");
		range[0] = 0;
		range[1] = 1024;
		create_property_range(output, prop_tv_xoffset, range, 0);

		prop_tv_yoffset = MAKE_ATOM("TVYOffset");
		range[0] = 0;
		range[1] = 1024;
		create_property_range(output, prop_tv_yoffset, range, 0);

		/*
		 * Dynamic aspect ratio selection can be disabled using this
		 * property. Useful if the TV doesn't support WSS.
		 */
		prop_tv_dynamic_aspect_ratio =
			MAKE_ATOM("TVDynamicAspectRatio");
		range[0] = 0;
		range[1] = 1;
		create_property_range(output, prop_tv_dynamic_aspect_ratio,
				      range, 1);
	}

	prop_alpha_mode = MAKE_ATOM("AlphaMode");
	range[0] = 0;
	range[1] = 1;
	create_property_range(output, prop_alpha_mode, range, 0);

	prop_graphics_alpha = MAKE_ATOM("GraphicsAlpha");
	range[0] = 0;
	range[1] = 255;
	create_property_range(output, prop_graphics_alpha, range, 255);

	prop_video_alpha = MAKE_ATOM("VideoAlpha");
	range[0] = 0;
	range[1] = 255;
	create_property_range(output, prop_video_alpha, range, 255);

	/*
	 * FIXME?
	 * other stuff?
	 */
}

static void output_dpms(xf86OutputPtr output, int mode)
{
	struct fbdev_output *priv = output->driver_private;

	DebugF("%s(output=%p, mode=%d)\n",
	       __func__, output, mode);

	if (mode != DPMSModeOn) {
		if (!omap_output_disable(priv->out))
			xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
				   "Unable to disable output\n");
	} else {
		if (!omap_output_enable(priv->out))
			xf86DrvMsg(output->scrn->scrnIndex, X_WARNING,
				   "Unable to enable output\n");
	}
}

static int check_mode(const DisplayModeRec *mode,
		      const DisplayModeRec *builtin)
{
	if (mode->HDisplay > builtin->HDisplay ||
	    mode->VDisplay > builtin->VDisplay)
		return MODE_BAD;

	return MODE_OK;
}

static int check_modes(const DisplayModeRec *mode,
		       const DisplayModeRec *builtin)
{
	const DisplayModeRec *p = builtin;

	if (!p)
		return MODE_BAD;

	do {
		if (check_mode(mode, p) == MODE_OK)
			return MODE_OK;
		p = p->next;
	} while (p && p != builtin);

	return MODE_BAD;
}

static int output_mode_valid(xf86OutputPtr output,
			     DisplayModePtr mode)
{
	struct fbdev_output *priv = output->driver_private;
	FBDevPtr fPtr = FBDEVPTR(output->scrn);

	DebugF("%s(output=%p, mode=%p)\n",
	       __func__, output, mode);

	DebugF(" mode = %dx%d\n",
	       mode->HDisplay, mode->VDisplay);

	/*
	 * FIXME we could support other mode sizes
	 * by adding borders or scaling.
	 */

	if (priv->type == FBDEV_OUTPUT_TYPE_LCD)
		return check_modes(mode, fPtr->builtin_lcd);

	if (priv->type == FBDEV_OUTPUT_TYPE_TV)
		return check_modes(mode, fPtr->builtin_tv);

	return MODE_BAD;
}

static xf86OutputStatus output_detect(xf86OutputPtr output)
{
	struct fbdev_output *priv = output->driver_private;

	DebugF("%s(output=%p)\n",
	       __func__, output);

	if (priv->type == FBDEV_OUTPUT_TYPE_LCD)
		return XF86OutputStatusConnected;

	/* FIXME get jack status from somewhere */
	return XF86OutputStatusUnknown;
}

static DisplayModePtr output_get_modes(xf86OutputPtr output)
{
	struct fbdev_output *priv = output->driver_private;
	FBDevPtr fPtr = FBDEVPTR(output->scrn);

	DebugF("%s(output=%p)\n",
	       __func__, output);

	/*
	 * FIXME we could support other mode sizes
	 * by adding borders or scaling.
	 */

	if (priv->type == FBDEV_OUTPUT_TYPE_LCD)
		return xf86DuplicateModes(output->scrn, fPtr->builtin_lcd);

	if (priv->type == FBDEV_OUTPUT_TYPE_TV)
		return xf86DuplicateModes(output->scrn, fPtr->builtin_tv);

	return NULL;
}

static Bool output_set_property(xf86OutputPtr output,
				Atom property,
				RRPropertyValuePtr value)
{
	struct fbdev_output *priv = output->driver_private;

	DebugF("%s(output=%p, property=%u, value=%p)\n",
	       __func__, output, (unsigned int) property, value);

	if (property == prop_xv_clone_fullscreen) {
		INT32 val;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		if (value->type != XA_INTEGER ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		val = *(INT32 *)value->data;
		if (val < 0 || val > 1)
			return FALSE;

		priv->xv_clone_fullscreen = val;
		return TRUE;
	} else if (property == prop_tv_signal_format) {
		Atom val;
		enum omap_tv_standard tv_std;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		if (value->type != XA_ATOM ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		memcpy(&val, value->data, 4);

		if (val == signal_format_composite_pal)
			tv_std = OMAP_TV_STANDARD_PAL;
		else if (val == signal_format_composite_ntsc)
			tv_std = OMAP_TV_STANDARD_NTSC;
		else
			return FALSE;

		if (priv->tv_std == tv_std)
			return TRUE;

		priv->tv_std = tv_std;
		restart_output(output);

		return TRUE;
	} else if (property == prop_tv_signal_properties) {
		Atom val;
		enum omap_tv_standard tv_std;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		if (value->type != XA_ATOM ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		memcpy(&val, value->data, 4);

		if (val == signal_properties_pal)
			tv_std = OMAP_TV_STANDARD_PAL;
		else if (val == signal_properties_ntsc)
			tv_std = OMAP_TV_STANDARD_NTSC;
		else
			return FALSE;

		if (priv->tv_std == tv_std)
			return TRUE;

		priv->tv_std = tv_std;
		restart_output(output);

		return TRUE;
	} else if (property == prop_tv_aspect_ratio) {
		Atom val;
		Bool widescreen;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		if (value->type != XA_ATOM ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		memcpy(&val, value->data, 4);

		if (val == aspect_ratio_4_3)
			widescreen = FALSE;
		else if (val == aspect_ratio_16_9)
			widescreen = TRUE;
		else
			return FALSE;

		if (priv->widescreen == widescreen)
			return TRUE;

		priv->widescreen = widescreen;
		update_output(output);

		return TRUE;
	} else if (property == prop_tv_scale) {
		INT32 val;

		if (value->type != XA_INTEGER ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		val = *(INT32 *)value->data;
		if (val < 0 || val > 100)
			return FALSE;

		if (priv->scale == val)
			return TRUE;

		priv->scale = val;
		update_output(output);

		return TRUE;
	} else if (property == prop_tv_xoffset) {
		INT32 val;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		if (value->type != XA_INTEGER ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		val = *(INT32 *)value->data;
		if (val < 0 || val > priv->hdisp)
			return FALSE;

		if (priv->xoffset == val)
			return TRUE;

		priv->xoffset = val;
		update_output(output);

		return TRUE;
	} else if (property == prop_tv_yoffset) {
		INT32 val;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		if (value->type != XA_INTEGER ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		val = *(INT32 *)value->data;
		if (val < 0 || val > priv->vdisp)
			return FALSE;

		if (priv->yoffset == val)
			return TRUE;

		priv->yoffset = val;
		update_output(output);

		return TRUE;
	} else if (property == prop_tv_dynamic_aspect_ratio) {
		INT32 val;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		if (value->type != XA_INTEGER ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		val = *(INT32 *)value->data;
		if (val < 0 || val > 1)
			return FALSE;

		if (priv->dynamic_aspect == val)
			return TRUE;

		priv->dynamic_aspect = val;
		update_output(output);

		return TRUE;
	} else if (property == prop_alpha_mode) {
		INT32 val;

		if (value->type != XA_INTEGER ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		val = *(INT32 *)value->data;
		if (val < 0 || val > 1)
			return FALSE;

		if (priv->alpha_mode == val)
			return TRUE;

		priv->alpha_mode = val;
		update_output(output);

		return TRUE;
	} else if (property == prop_graphics_alpha) {
		INT32 val;

		if (value->type != XA_INTEGER ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		val = *(INT32 *)value->data;
		if (val < 0 || val > 255)
			return FALSE;

		if (priv->graphics_alpha == val)
			return TRUE;

		priv->graphics_alpha = val;
		update_output(output);

		return TRUE;
	} else if (property == prop_video_alpha) {
		INT32 val;

		if (value->type != XA_INTEGER ||
		    value->format != 32 || value->size != 1)
			return FALSE;

		val = *(INT32 *)value->data;
		if (val < 0 || val > 255)
			return FALSE;

		if (priv->video_alpha == val)
			return TRUE;

		priv->video_alpha = val;
		update_output(output);

		return TRUE;
	}

	return FALSE;
}

static Bool output_get_property(xf86OutputPtr output,
				Atom property)
{
	struct fbdev_output *priv = output->driver_private;

	DebugF("%s(output=%p, property=%u)\n",
	       __func__, output, (unsigned int) property);

	if (property == prop_tv_signal_format) {
		Atom val;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		switch (priv->tv_std) {
		case OMAP_TV_STANDARD_PAL:
			val = signal_format_composite_pal;
			break;
		case OMAP_TV_STANDARD_NTSC:
			val = signal_format_composite_ntsc;
			break;
		default:
			assert(0);
		}

		change_property_atom(output, prop_tv_signal_format, val);
	} else if (property == prop_tv_signal_properties) {
		Atom val;

		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		switch (priv->tv_std) {
		case OMAP_TV_STANDARD_PAL:
			val = signal_properties_pal;
			break;
		case OMAP_TV_STANDARD_NTSC:
			val = signal_properties_ntsc;
			break;
		default:
			assert(0);
		}

		change_property_atom(output, prop_tv_signal_properties, val);
	} else if (property == prop_tv_xoffset) {
		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		change_property_range(output, prop_tv_xoffset, priv->xoffset);
	} else if (property == prop_tv_yoffset) {
		if (priv->type != FBDEV_OUTPUT_TYPE_TV)
			return FALSE;

		change_property_range(output, prop_tv_yoffset, priv->yoffset);
	}

	return TRUE;
}

static xf86CrtcPtr output_get_crtc(xf86OutputPtr output)
{
	DebugF("%s(output=%p)\n",
	       __func__, output);

	/* FIXME Is this OK? */
	return output->crtc;
}

static void output_destroy(xf86OutputPtr output)
{
	DebugF("%s(output=%p)\n",
	       __func__, output);

	if (output->driver_private)
		free(output->driver_private);
}

static const xf86OutputFuncsRec fbdev_output_funcs = {
	.create_resources = output_create_resources,
	.dpms             = output_dpms,
	.mode_valid       = output_mode_valid,
	.detect           = output_detect,
	.get_modes        = output_get_modes,
	.set_property     = output_set_property,
	.get_property     = output_get_property,
	.get_crtc         = output_get_crtc,
	.destroy          = output_destroy,
};

xf86OutputPtr
fbdev_output_create(ScrnInfoPtr pScrn,
		    const char *name,
		    enum fbdev_output_type type,
		    struct omap_output *out,
		    unsigned int crtc)
{
	enum omap_mirror mirror;
	enum omap_rotate rotate;
	struct fbdev_output *priv;
	xf86OutputPtr output;
	unsigned int width = 0, height = 0;
	FBDevPtr fPtr = FBDEVPTR(pScrn);

	DebugF("%s(output=%p, name=%s)\n",
	       __func__, pScrn, name);

	if (!omap_output_get_setup(out, &mirror, &rotate)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Unable to get initial output configuration\n");
		return NULL;
	}

	if (!omap_output_assign_overlay(out, fPtr->ovl[0]) ||
	    !omap_output_get_size(out, NULL, NULL, &width, &height)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Unable to get output size\n");
		return NULL;
	}

	priv = calloc(1, sizeof *priv);
	if (!priv)
		return NULL;

	output = xf86OutputCreate(pScrn,
				  &fbdev_output_funcs,
				  name);
	if (!output) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Unable to create output\n");
		free(priv);
		return NULL;
	}

	priv->widescreen = FALSE;
	priv->scale = 90;
	priv->type = type;
	priv->out = out;
	priv->xv_clone_fullscreen = type == FBDEV_OUTPUT_TYPE_TV;
	priv->dynamic_aspect = TRUE;
	priv->alpha_mode = FALSE;
	priv->graphics_alpha = 255;
	priv->video_alpha = 255;

	switch (type) {
	case FBDEV_OUTPUT_TYPE_LCD:
#if 0
		/* FIXME causes some text to become invisible in DUI apps */
		/* FIXME should come from the kernel */
		output->subpixel_order = SubPixelHorizontalRGB;
#endif
		break;
	case FBDEV_OUTPUT_TYPE_TV:
		break;
	}

	output->mm_width  = (width + 500) / 1000;
	output->mm_height = (height + 500) / 1000;

	output->possible_crtcs = 1 << crtc;
	output->possible_clones = 0;
	output->interlaceAllowed = FALSE;
	output->doubleScanAllowed = FALSE;

	switch (rotate) {
	case OMAP_ROTATE_0:
		output->initial_rotation = RR_Rotate_0;
		break;
	case OMAP_ROTATE_90:
		output->initial_rotation = RR_Rotate_270;
		break;
	case OMAP_ROTATE_180:
		output->initial_rotation = RR_Rotate_180;
		break;
	case OMAP_ROTATE_270:
		output->initial_rotation = RR_Rotate_90;
		break;
	}

	switch (rotate) {
	case OMAP_ROTATE_0:
	case OMAP_ROTATE_180:
		if (mirror & OMAP_MIRROR_HORZ)
			output->initial_rotation |= RR_Reflect_X;
		if (mirror & OMAP_MIRROR_VERT)
			output->initial_rotation |= RR_Reflect_Y;
		break;
	case OMAP_ROTATE_90:
	case OMAP_ROTATE_270:
		if (mirror & OMAP_MIRROR_HORZ)
			output->initial_rotation |= RR_Reflect_Y;
		if (mirror & OMAP_MIRROR_VERT)
			output->initial_rotation |= RR_Reflect_X;
		break;
	}

	output->driver_private = priv;

	return output;
}
