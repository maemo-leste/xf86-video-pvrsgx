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
#include <error.h>
#include <string.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/time.h>

/* all driver need this */
#include <X11/Xatom.h>
#include "xf86.h"
#include "xf86_OSproc.h"

#include "mipointer.h"
#include "mibstore.h"
#include "micmap.h"
#include "colormapst.h"
#include "xf86cmap.h"
#include "shadow.h"
#include "xf86Xinput.h"

/* for visuals */
#include "fb.h"

#include "xf86Crtc.h"
#include "xf86RandR12.h"
#include "xf86xv.h"

#include "fbdev.h"
#include <linux/fb.h>
#include <linux/omapfb.h>

#include "sgx_dri2.h"
#include "sgx_pvr2d.h"
#include "sgx_xv.h"
#include "omap_video.h"
#include "omap_tvout.h"
#include "omap.h"
#include "extfb.h"
#include "perf.h"

enum {
	SGX_PAGE_SIZE = 4096,
};

/* FIXME turn into an option? */
#define USE_SGX 1

/* -------------------------------------------------------------------- */

static const OptionInfoRec *FBDevAvailableOptions(int chipid, int busid);
static void FBDevIdentify(int flags);
static Bool FBDevProbe(DriverPtr drv, int flags);
static Bool FBDevPreInit(ScrnInfoPtr pScrn, int flags);
static Bool FBDevScreenInit(SCREEN_INIT_ARGS_DECL);
static Bool FBDevSwitchMode(SWITCH_MODE_ARGS_DECL);
static void FBDevAdjustFrame(ADJUST_FRAME_ARGS_DECL);
static Bool FBDevEnterVT(VT_FUNC_ARGS_DECL);
static void FBDevLeaveVT(VT_FUNC_ARGS_DECL);
static void FBDevFreeScreen(FREE_SCREEN_ARGS_DECL);
static Bool FBDevValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode,
			   Bool verbose, int flags);
static Bool FBDevCreateScreenResources(ScreenPtr pScreen);
static Bool FBDevCloseScreen(CLOSE_SCREEN_ARGS_DECL);
static Bool fbdev_randr12_preinit(ScrnInfoPtr pScrn);
static void fbdev_randr12_uninit(ScrnInfoPtr pScrn);

/* -------------------------------------------------------------------- */

static Bool check_bpp_depth(unsigned int bpp,
			    unsigned int depth)
{
	if (!USE_SGX) {
		if (bpp == 24 && depth == 24)
			return TRUE;
	}

	if (bpp == 32 && depth == 24)
		return TRUE;
	if (bpp == 16 && depth == 16)
		return TRUE;
	if (bpp == 16 && depth == 12)
		return TRUE;

	return FALSE;
}

static enum omap_format get_omap_format(unsigned int bpp,
					unsigned int depth)
{
	if (bpp == 32 && depth == 24)
		return OMAP_FORMAT_XRGB8888;
	if (bpp == 24 && depth == 24)
		return OMAP_FORMAT_RGB888;
	if (bpp == 16 && depth == 16)
		return OMAP_FORMAT_RGB565;
	if (bpp == 16 && depth == 12)
		return OMAP_FORMAT_XRGB4444;

	FatalError("Invalid format\n");
}

static unsigned int pitch_alignment(unsigned int width,
				    unsigned int bpp)
{
	unsigned int align = 1;

	align = ALIGN(align, sizeof(FbBits));

	if (USE_SGX)
		return ALIGN(align, getSGXPitchAlign(width) * bpp >> 3);
	else
		return align;
}

static unsigned int buffer_alignment(void)
{
	if (USE_SGX)
		return SGX_PAGE_SIZE;
	else
		return 1;
}

static void *get_fbmem(FBDevPtr fPtr)
{
	if (USE_SGX)
		return NULL;
	else
		return fPtr->fbmem;
}

static Bool pre_fb_reset(ScrnInfoPtr pScrn)
{
	if (USE_SGX)
		return PVR2D_PreFBReset(pScrn);
	else
		return TRUE;
}

static Bool post_fb_reset(ScrnInfoPtr pScrn)
{
	if (USE_SGX)
		return PVR2D_PostFBReset(pScrn);
	else
		return TRUE;
}

static Bool omap_preinit(ScrnInfoPtr pScrn)
{
	static const char *devices[] = {
		"/dev/fb0",
		"/dev/fb1",
		"/dev/fb2",
	};
	static const char *ovl_names[] = {
		"gfx",
		"vid1",
		"vid2",
	};
	static const char *out_names[] = {
		"lcd",
		"tv",
	};
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	int i;

	/*
	 * FIXME perhaps add probing/enumeration to omap.c and
	 * just return ready made list of devices from there.
	 */
	for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++) {
		fPtr->ovl[i] = omap_overlay_new();
		if (!fPtr->ovl[i])
			return FALSE;
		if (!omap_overlay_open(fPtr->ovl[i],
				       devices[i], ovl_names[i])) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Unable to open overlay %s/%s\n",
				   devices[i], ovl_names[i]);
			return FALSE;
		}

		/* Disable alpha blending */
		omap_overlay_global_alpha(fPtr->ovl[i], 255);
	}

	for (i = 0; i < ARRAY_SIZE(fPtr->fb); i++) {
		fPtr->fb[i] = omap_fb_new();
		if (!fPtr->fb[i])
			return FALSE;
		if (!omap_fb_open(fPtr->fb[i], devices[i])) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Unable to open fb %s\n", devices[i]);
			return FALSE;
		}
	}

	for (i = 0; i < ARRAY_SIZE(fPtr->out); i++) {
		fPtr->out[i] = omap_output_new();
		if (!fPtr->out[i])
			return FALSE;
		if (!omap_output_open(fPtr->out[i], out_names[i])) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Unable to open output %s\n", out_names[i]);
			return FALSE;
		}
	}

	for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++)
		omap_fb_assign_overlay(fPtr->fb[i], fPtr->ovl[i]);

	for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++)
		omap_output_assign_overlay(fPtr->out[0], fPtr->ovl[i]);

	for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++)
		fPtr->ovl_usage[i] = FBDEV_OVERLAY_USAGE_NONE;

	/*
	 * Free all framebuffers except the first one. Leave that
	 * one alone to preserve the contents for blinkless startup.
	 */
	for (i = 1; i < ARRAY_SIZE(fPtr->fb); i++) {
		struct omap_fb *fb = fPtr->fb[i];

		omap_fb_assign_overlay(fb, fPtr->ovl[0]);

		omap_fb_free(fb);
	}
	omap_fb_assign_overlay(fPtr->fb[0], fPtr->ovl[0]);

	for (i = 0; i < ARRAY_SIZE(fPtr->out); i++) {
		struct omap_output *out = fPtr->out[i];
		enum omap_output_update update = OMAP_OUTPUT_UPDATE_AUTO;
		enum omap_output_tear tearsync = OMAP_OUTPUT_TEAR_NONE;

		omap_output_assign_overlay(out, fPtr->ovl[0]);

		omap_output_get_caps(out, &update, &tearsync);

		/* Switch to manual updates if supported */
		if (update == OMAP_OUTPUT_UPDATE_MANUAL) {
			omap_output_set_update_mode(out,
						    OMAP_OUTPUT_UPDATE_MANUAL);
			fPtr->extfb.enabled = TRUE;
		}

		/* Configure tearsync if supported */
		if (tearsync == OMAP_OUTPUT_TEAR_SYNC)
			omap_output_set_tearsync(out, fPtr->conf.vsync ?
						 OMAP_OUTPUT_TEAR_SYNC :
						 OMAP_OUTPUT_TEAR_NONE);

		/* Disable alpha blending and color keying */
		omap_output_alpha_blending(out, false);
		omap_output_color_key(out, false, 0);
	}
	omap_output_assign_overlay(fPtr->out[0], fPtr->ovl[0]);

	return TRUE;
}

static void omap_uninit(ScrnInfoPtr pScrn)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	int i;

	/* FIXME restore initial config? */

	for (i = ARRAY_SIZE(fPtr->ovl) - 1; i >= 0; i--)
		omap_overlay_disable(fPtr->ovl[i]);

	for (i = ARRAY_SIZE(fPtr->out) - 1; i >= 0; i--) {
		omap_output_close(fPtr->out[i]);
		omap_output_del(fPtr->out[i]);
		fPtr->out[i] = NULL;
	}

	for (i = ARRAY_SIZE(fPtr->fb) - 1; i >= 0; i--) {
		omap_fb_close(fPtr->fb[i]);
		omap_fb_del(fPtr->fb[i]);
		fPtr->fb[i] = NULL;
	}

	for (i = ARRAY_SIZE(fPtr->ovl) - 1; i >= 0; i--) {
		omap_overlay_close(fPtr->ovl[i]);
		omap_overlay_del(fPtr->ovl[i]);
		fPtr->ovl[i] = NULL;
	}
}

/*
 * FIXME This could be a lot fancier taking into
 * account the pixel formart, need to scale etc.
 */
struct omap_overlay *
fbdev_get_overlay(ScrnInfoPtr pScrn, enum fbdev_overlay_usage usage)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	int i = -1;

	/* Let's reserve GFX for CRTC0 */
	if (usage == FBDEV_OVERLAY_USAGE_CRTC0) {
		if (i < 0 && !fPtr->ovl_usage[0])
			i = 0;
	}

	/* Prefer VID2 for Xv, VID1 for CRTCs */
	if (usage == FBDEV_OVERLAY_USAGE_XV) {
		if (i < 0 && !fPtr->ovl_usage[2])
			i = 2;
		if (i < 0 && !fPtr->ovl_usage[1])
			i = 1;
	} else if (usage == FBDEV_OVERLAY_USAGE_CRTC0 ||
		   usage == FBDEV_OVERLAY_USAGE_CRTC1) {
		if (i < 0 && !fPtr->ovl_usage[1])
			i = 1;
		if (i < 0 && !fPtr->ovl_usage[2])
			i = 2;
	}

	/* Fixed Xv mapping */
	if (usage == FBDEV_OVERLAY_USAGE_XV1) {
		if (i < 0 && !fPtr->ovl_usage[1])
			i = 1;
	}
	if (usage == FBDEV_OVERLAY_USAGE_XV2) {
		if (i < 0 && !fPtr->ovl_usage[2])
			i = 2;
	}


	if (i < 0)
		return NULL;

	DebugF("Getting overlay %d for %s\n", i,
	       usage == FBDEV_OVERLAY_USAGE_CRTC0 ? "CRTC0" :
	       usage == FBDEV_OVERLAY_USAGE_CRTC1 ? "CRTC1" :
	       usage == FBDEV_OVERLAY_USAGE_XV ? "XV" :
	       usage == FBDEV_OVERLAY_USAGE_XV1 ? "XV1" :
	       usage == FBDEV_OVERLAY_USAGE_XV2 ? "XV2" : "Unknown reason");

	fPtr->ovl_usage[i] = usage;
	return fPtr->ovl[i];
}

void fbdev_put_overlay(ScrnInfoPtr pScrn, struct omap_overlay *ovl)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	int i;

	for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++) {
		if (ovl != fPtr->ovl[i])
			continue;

		DebugF("Putting overlay %d from %s\n", i,
		       fPtr->ovl_usage[i] == FBDEV_OVERLAY_USAGE_CRTC0 ?
		       "CRTC0" :
		       fPtr->ovl_usage[i] == FBDEV_OVERLAY_USAGE_CRTC1 ?
		       "CRTC1" :
		       fPtr->ovl_usage[i] == FBDEV_OVERLAY_USAGE_XV ?
		       "XV" :
		       fPtr->ovl_usage[i] == FBDEV_OVERLAY_USAGE_XV1 ?
		       "XV1" :
		       fPtr->ovl_usage[i] == FBDEV_OVERLAY_USAGE_XV2 ?
		       "XV2" : "Unknown reason");

		fPtr->ovl_usage[i] = FBDEV_OVERLAY_USAGE_NONE;
		omap_fb_assign_overlay(fPtr->fb[i], fPtr->ovl[i]);
		break;
	}
}

static Bool
realloc_fb(ScrnInfoPtr pScrn,
	   unsigned int width,
	   unsigned int height,
	   unsigned int bpp,
	   unsigned int depth)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	bool ret;
	int i;
	bool enabled_ovls[ARRAY_SIZE(fPtr->ovl)] = { [0] = false };

	DebugF("%s(%p, %u, %u, %u, %u)\n",
	       __func__, pScrn, width, height, bpp, depth);

	if (fPtr->fbmem)
		omap_fb_unmap(fPtr->fb[0]);

	/* Disable overlays */
	for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++) {
		/* Only overlays used as CRTCs need to be disabled */
		if (fPtr->ovl_usage[i] != FBDEV_OVERLAY_USAGE_CRTC0 &&
		    fPtr->ovl_usage[i] != FBDEV_OVERLAY_USAGE_CRTC1)
			continue;
		enabled_ovls[i] = omap_overlay_enabled(fPtr->ovl[i]);
		if (!enabled_ovls[i])
			continue;
		if (!omap_overlay_disable(fPtr->ovl[i]))
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Unable to disable overlay\n");
	}

	/*
	 * Wait for the overlays to be disabled.
	 * The kernel should probably be changed to do this automagically.
	 */
	for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++) {
		if (!enabled_ovls[i])
			continue;
		if (!omap_overlay_wait(fPtr->ovl[i]))
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Unable to wait for overlay to disable\n");
	}

	ret = omap_fb_alloc(fPtr->fb[0], width, height,
			    get_omap_format(bpp, depth),
			    fPtr->conf.page_flip_bufs,
			    buffer_alignment(),
			    pitch_alignment(width, bpp));
	if (!ret) {
		if (fPtr->fbmem) {
			/* remap the original framebuffer configuration */
			if (!omap_fb_map(fPtr->fb[0],
					 &fPtr->fbmem, &fPtr->fbmem_len))
				FatalError("Unable to map framebuffer\n");
		}

		/* Restore overlays */
		for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++) {
			if (!enabled_ovls[i])
				continue;
			if (!omap_overlay_enable(fPtr->ovl[i]))
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					   "Unable to enable overlay\n");
		}

		return FALSE;
	}

	/* map the new framebuffer configuration */
	if (!omap_fb_map(fPtr->fb[0], &fPtr->fbmem, &fPtr->fbmem_len))
		FatalError("Unable to map framebuffer\n");

	/* Restore enabled overlays */
	for (i = 0; i < ARRAY_SIZE(fPtr->ovl); i++) {
		if (!enabled_ovls[i])
			continue;
		if (!omap_overlay_enable(fPtr->ovl[i]))
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Unable to enable overlay\n");
	}

	return TRUE;
}

/* -------------------------------------------------------------------- */

_X_EXPORT DriverRec PVRSGX = {
	.driverVersion = PVRSGX_VERSION,
	.driverName = PVRSGX_DRIVER_NAME,
	.Identify = FBDevIdentify,
	.Probe = FBDevProbe,
	.AvailableOptions = FBDevAvailableOptions,
};

/* Supported "chipsets" */
static SymTabRec FBDevChipsets[] = {
	{
		.token = 0,
		.name = "pvrsgx",
	},
	{
		.token = -1,
		.name = NULL
	},
};

static const OptionInfoRec FBDevOptions[] = {
	{
		.token = OPTION_PERF_TIME,
		.name = "PerfTime",
		.type = OPTV_INTEGER,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_PERF_RESET,
		.name = "PerfReset",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_TEST_COPY,
		.name = "TestCopy",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_TEST_COPY_ONLY,
		.name = "TestCopyOnly",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_SWAP_CONTROL,
		.name = "SwapMethod",
		.type = OPTV_STRING,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_RENDER_SYNC,
		.name = "RenderSync",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_VSYNC,
		.name = "VSync",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_PAGE_FLIP_BUFS,
		.name = "PageFlipBuffers",
		.type = OPTV_INTEGER,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_FLIP_STATS_TIME,
		.name = "FlipStatsTime",
		.type = OPTV_INTEGER,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_FLIP_STATS_RESET,
		.name = "FlipStatsReset",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_CAN_CHANGE_SCREEN_SIZE,
		.name = "CanChangeScreenSize",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_POISON_GETBUFFERS,
		.name = "PoisonGetBuffers",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = OPTION_POISON_SWAPBUFFERS,
		.name = "PoisonSwapBuffers",
		.type = OPTV_BOOLEAN,
		.value = { 0 },
		.found = FALSE
	},
	{
		.token = -1,
		.name = NULL,
		.type = OPTV_NONE,
		.value = { 0 },
		.found = FALSE
	},
};

MODULESETUPPROTO(FBDevSetup);

static XF86ModuleVersionInfo FBDevVersRec = {
	.modname = "pvrsgx",
	.vendor = MODULEVENDORSTRING,
	._modinfo1_ = MODINFOSTRING1,
	._modinfo2_ = MODINFOSTRING2,
	.xf86version = XORG_VERSION_CURRENT,
	.majorversion = PACKAGE_VERSION_MAJOR,
	.minorversion = PACKAGE_VERSION_MINOR,
	.patchlevel = PACKAGE_VERSION_PATCHLEVEL,
	.abiclass = ABI_CLASS_VIDEODRV,
	.abiversion = ABI_VIDEODRV_VERSION,
	.moduleclass = MOD_CLASS_VIDEODRV,
	.checksum = { 0, 0, 0, 0 },
};

_X_EXPORT XF86ModuleData pvrsgxModuleData = {
	.vers = &FBDevVersRec,
	.setup = FBDevSetup,
};

pointer FBDevSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (!setupDone) {
		setupDone = TRUE;
		xf86AddDriver(&PVRSGX, module, 0);
		return (pointer) 1;
	} else {
		if (errmaj)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}
}

static Bool FBDevGetRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate != NULL)
		return TRUE;

	pScrn->driverPrivate = calloc(1, sizeof(FBDevRec));
	if (!pScrn->driverPrivate)
		return FALSE;

	return TRUE;
}

static void FBDevFreeRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate == NULL)
		return;
	free(pScrn->driverPrivate);
	pScrn->driverPrivate = NULL;
}

/* -------------------------------------------------------------------- */

static const OptionInfoRec *FBDevAvailableOptions(int chipid, int busid)
{
	return FBDevOptions;
}

static void FBDevIdentify(int flags)
{
	xf86PrintChipsets(PVRSGX_NAME, "driver for framebuffer", FBDevChipsets);
}

static Bool FBDevProbe(DriverPtr drv, int flags)
{
	int i;
	ScrnInfoPtr pScrn;
	GDevPtr *devSections;
	int numDevSections;
	int entity;
	Bool foundScreen = FALSE;

	ENTER();

	/* For now, just bail out for PROBE_DETECT. */
	if (flags & PROBE_DETECT)
		return FALSE;

	numDevSections = xf86MatchDevice(PVRSGX_DRIVER_NAME, &devSections);
	if (numDevSections <= 0)
		return FALSE;

	for (i = 0; i < numDevSections; i++) {
		entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);

		pScrn = xf86ConfigFbEntity(NULL, 0, entity,
					   NULL, NULL, NULL, NULL);
		if (!pScrn)
			continue;

		foundScreen = TRUE;

		pScrn->driverVersion = PVRSGX_VERSION;
		pScrn->driverName = PVRSGX_DRIVER_NAME;
		pScrn->name = PVRSGX_NAME;
		pScrn->Probe = FBDevProbe;
		pScrn->PreInit = FBDevPreInit;
		pScrn->ScreenInit = FBDevScreenInit;
		pScrn->SwitchMode = FBDevSwitchMode;
		pScrn->AdjustFrame = FBDevAdjustFrame;
		pScrn->EnterVT = FBDevEnterVT;
		pScrn->LeaveVT = FBDevLeaveVT;
		pScrn->FreeScreen = FBDevFreeScreen;
		pScrn->ValidMode = FBDevValidMode;
	}

	free(devSections);

	LEAVE();

	return foundScreen;
}

static void FBDevInitConf(ScrnInfoPtr pScrn)
{
	int i;
	const char *s;
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	MessageType from;

	/* SwapMethod */

	from = X_DEFAULT;
	fPtr->conf.swap_control = DRI2_SWAP_CONTROL_FLIP;

	s = xf86GetOptValString(fPtr->Options, OPTION_SWAP_CONTROL);
	if (s != NULL) {
		if (xf86NameCmp(s, "flip") == 0) {
			from = X_CONFIG;
			fPtr->conf.swap_control = DRI2_SWAP_CONTROL_FLIP;
		} else if (xf86NameCmp(s, "blit") == 0) {
			from = X_CONFIG;
			fPtr->conf.swap_control = DRI2_SWAP_CONTROL_BLIT;
		} else
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "%s is not a valid SwapMethod value\n", s);
	}

	xf86DrvMsg(pScrn->scrnIndex, from, "SwapMethod is %s\n",
		   fPtr->conf.swap_control == DRI2_SWAP_CONTROL_FLIP ?
		   "flip" : "blit");

	/* VSync */

	from = X_DEFAULT;
	fPtr->conf.vsync = TRUE;

	if (xf86GetOptValBool(fPtr->Options, OPTION_VSYNC, &fPtr->conf.vsync))
		from = X_CONFIG;

	xf86DrvMsg(pScrn->scrnIndex, from, "%s vblank synchronization\n",
		   fPtr->conf.vsync ? "Enabling" : "Disabling");

	/* RenderSync */

	from = X_DEFAULT;
	fPtr->conf.render_sync = TRUE;

	if (xf86GetOptValBool(fPtr->Options,
			      OPTION_VSYNC, &fPtr->conf.render_sync))
		from = X_CONFIG;

	xf86DrvMsg(pScrn->scrnIndex, from, "%s synchronous rendering\n",
		   fPtr->conf.render_sync ? "Enabling" : "Disabling");

	/* PageFlipBuffers */

	from = X_DEFAULT;
	fPtr->conf.page_flip_bufs = DEFAULT_PAGE_FLIP_BUFFERS;

	if (xf86GetOptValInteger(fPtr->Options, OPTION_PAGE_FLIP_BUFS, &i)) {
		if (fPtr->conf.page_flip_bufs < 1 ||
		    fPtr->conf.page_flip_bufs > MAX_PAGE_FLIP_BUFFERS) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "%d is not a valid PageFlipBuffers value\n",
				   i);
		} else {
			from = X_CONFIG;
			fPtr->conf.page_flip_bufs = i;
		}
	}

	xf86DrvMsg(pScrn->scrnIndex, from, "Using %d page flip buffers.\n",
		   fPtr->conf.page_flip_bufs);

	/* CanChangeScreenSize */

	from = X_DEFAULT;
	fPtr->conf.can_change_screen_size = FALSE;

	if (xf86GetOptValBool(fPtr->Options,
			      OPTION_CAN_CHANGE_SCREEN_SIZE,
			      &fPtr->conf.can_change_screen_size))
		from = X_CONFIG;

	xf86DrvMsg(pScrn->scrnIndex, from, "Screen size %s be changed\n",
		   fPtr->conf.can_change_screen_size ? "can" : "can't");

	/* PoisonGetBuffers */

	from = X_DEFAULT;
	fPtr->conf.poison_getbuffers = FALSE;

	if (xf86GetOptValBool(fPtr->Options, OPTION_POISON_GETBUFFERS,
			      &fPtr->conf.poison_getbuffers))
		from = X_CONFIG;

	xf86DrvMsg(pScrn->scrnIndex, from, "%s poisoning in GetBuffers\n",
		   fPtr->conf.poison_getbuffers ? "Enabling" : "Disabling");

	/* PoisonSwapBuffers */

	from = X_DEFAULT;
	fPtr->conf.poison_swapbuffers = FALSE;

	if (xf86GetOptValBool(fPtr->Options, OPTION_POISON_SWAPBUFFERS,
			      &fPtr->conf.poison_swapbuffers))
		from = X_CONFIG;

	xf86DrvMsg(pScrn->scrnIndex, from, "%s poisoning in SwapBuffers\n",
		   fPtr->conf.poison_swapbuffers ? "Enabling" : "Disabling");
}

static Bool FBDevPreInit(ScrnInfoPtr pScrn, int flags)
{
	FBDevPtr fPtr;
	rgb rgb_zeros = { 0, 0, 0 };
	Gamma gamma_zeros = { 0.0, 0.0, 0.0 };

	if (flags & PROBE_DETECT)
		return FALSE;

	ENTER();

	if (pScrn->numEntities != 1)
		return FALSE;

	pScrn->monitor = pScrn->confScreen->monitor;

	if (!FBDevGetRec(pScrn))
		return FALSE;
	fPtr = FBDEVPTR(pScrn);

	fPtr->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

	if (!xf86SetDepthBpp(pScrn, 16, 16, 16,
			     Support32bppFb | Support24bppFb))
		goto error_free;
	xf86PrintDepthBpp(pScrn);

	if (!xf86SetWeight(pScrn, rgb_zeros, rgb_zeros))
		goto error_free;

	if (!xf86SetDefaultVisual(pScrn, -1))
		goto error_free;

	if (!xf86SetGamma(pScrn, gamma_zeros))
		goto error_free;

	if (pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "requested default visual (%s) is not supported\n",
			   xf86GetVisualName(pScrn->defaultVisual));
		goto error_free;
	}

	if (!check_bpp_depth(pScrn->bitsPerPixel, pScrn->depth)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "requested depth %d is not supported\n",
			   pScrn->depth);
		goto error_free;
	}

	pScrn->progClock = TRUE;
	pScrn->rgbBits = 8;
	pScrn->chipset = "pvrsgx";

	/* handle options */
	xf86CollectOptions(pScrn, NULL);
	fPtr->Options = malloc(sizeof(FBDevOptions));
	if (!fPtr->Options)
		goto error_free;
	memcpy(fPtr->Options, FBDevOptions, sizeof(FBDevOptions));
	xf86ProcessOptions(pScrn->scrnIndex, fPtr->pEnt->device->options,
			   fPtr->Options);

	FBDevInitConf(pScrn);

	if (!omap_preinit(pScrn))
		goto error_free;

	if (!fbdev_randr12_preinit(pScrn))
		goto error_omap;

	xf86SetDpi(pScrn, 0, 0);

	if (!xf86LoadSubModule(pScrn, "fb"))
		goto error_randr;

	LEAVE();

	return TRUE;

 error_randr:
	fbdev_randr12_uninit(pScrn);
 error_omap:
	omap_uninit(pScrn);
 error_free:
	FBDevFreeRec(pScrn);
	return FALSE;
}

static Bool FBDevSaveScreen(ScreenPtr pScreen, int mode)
{
	return TRUE;
}

static Bool FBDevValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode,
			   Bool verbose, int flags)
{
	return TRUE;
}

static Bool FBDevSwitchMode(SWITCH_MODE_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	return xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
}

static void FBDevAdjustFrame(ADJUST_FRAME_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	xf86CrtcPtr crtc = xf86CompatCrtc(pScrn);

	if (crtc && crtc->enabled)
		crtc->funcs->set_origin(crtc, x, y);
}

static Bool FBDevScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	VisualPtr visual;
	unsigned int pitch;

	ENTER();

	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, 5,
		"\tbitsPerPixel=%d, depth=%d, defaultVisual=%s\n"
		"\tmask: %x,%x,%x, offset: %u,%u,%u\n", pScrn->bitsPerPixel,
		pScrn->depth, xf86GetVisualName(pScrn->defaultVisual),
		(unsigned)pScrn->mask.red, (unsigned)pScrn->mask.green,
		(unsigned)pScrn->mask.blue, (unsigned)pScrn->offset.red,
		(unsigned)pScrn->offset.green, (unsigned)pScrn->offset.blue);
	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, 5,
		"\tvirtualX=%d, virtualY=%d\n",
		pScrn->virtualX, pScrn->virtualY);

	if (!realloc_fb(pScrn, pScrn->virtualX, pScrn->virtualY,
			pScrn->bitsPerPixel, pScrn->depth)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Unable to allocate video memory\n");
		return FALSE;
	}

	if (!omap_fb_get_info(fPtr->fb[0], NULL, NULL, &pitch)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Unable to get framebuffer pitch\n");
		return FALSE;
	}

	pScrn->displayWidth = (pitch << 3) / pScrn->bitsPerPixel;

	/* mi layer */
	miClearVisualTypes();
	if (!miSetVisualTypes
	    (pScrn->depth, TrueColorMask, pScrn->rgbBits, TrueColor)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "visual type setup failed"
			   " for %d bits per pixel [1]\n", pScrn->bitsPerPixel);
		return FALSE;
	}
	if (!miSetPixmapDepths()) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "pixmap depth setup failed\n");
		return FALSE;
	}

	if (!fbScreenInit
	    (pScreen, get_fbmem(fPtr), pScrn->virtualX, pScrn->virtualY,
	     pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
	     pScrn->bitsPerPixel)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "fbScreenInit failed\n");
		return FALSE;
	}

	/* Fixup RGB ordering */
	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
		if ((visual->class | DynamicClass) == DirectColor) {
			visual->offsetRed = pScrn->offset.red;
			visual->offsetGreen = pScrn->offset.green;
			visual->offsetBlue = pScrn->offset.blue;
			visual->redMask = pScrn->mask.red;
			visual->greenMask = pScrn->mask.green;
			visual->blueMask = pScrn->mask.blue;
		}
	}

	/* must be after RGB ordering fixed */
	if (!fbPictureInit(pScreen, NULL, 0)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Render extension initialisation failed\n");
		return FALSE;
	}

	if (USE_SGX)
		EXA_Init(pScreen);

	fbdev_init_video(pScreen, 2, fPtr->fb[2], fPtr->fb[1]);

	xf86SetBlackWhitePixels(pScreen);
	miInitializeBackingStore(pScreen);
	xf86SetBackingStore(pScreen);

	pScrn->vtSema = TRUE;

	/* software cursor */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	if (!xf86SetDesiredModes(pScrn))
		return FALSE;

	if (!xf86CrtcScreenInit(pScreen))
		return FALSE;
	/*
	 * xf86CrtcScreenInit() assumes rotation/reflection is impossible
	 * without a shadow framebuffer. Set things right.
	 */
	xf86RandR12SetRotations(pScreen, RR_Rotate_0 | RR_Rotate_90 |
				RR_Rotate_180 | RR_Rotate_270 |
				RR_Reflect_X | RR_Reflect_Y);

	if (!miCreateDefColormap(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "internal error: failed to create colormap\n");
		return FALSE;
	}

	/* Setup the screen saver */
	pScreen->SaveScreen = FBDevSaveScreen;

	/* Wrap the current CreateScreenResources function */
	fPtr->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = FBDevCreateScreenResources;

	/* Wrap the current CloseScreen function */
	fPtr->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = FBDevCloseScreen;

	/* Let the server copy the framebuffer cleanly */
	pScreen->canDoBGNoneRoot = TRUE;

	LEAVE();

	return TRUE;
}

static Bool FBDevEnterVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	/* FIXME save the current config somehow */

	return xf86SetDesiredModes(pScrn);
}

static void FBDevLeaveVT(VT_FUNC_ARGS_DECL)
{
	/* FIXME restore original config somehow */
}

static void FBDevFreeScreen(FREE_SCREEN_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	FBDevPtr fPtr = FBDEVPTR(pScrn);

	if (!fPtr)
		return;

	fbdev_randr12_uninit(pScrn);

	omap_uninit(pScrn);

	FBDevFreeRec(pScrn);
}

static Bool FBDevCreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	Bool res;

	pScreen->CreateScreenResources = fPtr->CreateScreenResources;
	res = pScreen->CreateScreenResources(pScreen);

	if (res) {
		fPtr->pixmap = (*pScreen->GetScreenPixmap)(pScreen);
		if (!fPtr->pixmap) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Couldn't get the screen pixmap\n");
			res = FALSE;
		}
	}

	if (res)
		res = PVR2DCreateScreenResources(pScreen);

	if (res)
		res = ExtFBCreateScreenResources(pScreen);

	fPtr->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = FBDevCreateScreenResources;

	return res;
}

static Bool FBDevCloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	FBDevPtr fPtr = FBDEVPTR(pScrn);

	ExtFBCloseScreen(pScreen);

	PVR2DCloseScreen(pScreen);

	(*pScreen->DestroyPixmap)(pScreen->devPrivate);
	pScreen->devPrivate = NULL;

	fbdev_fini_video(pScreen);

	if (USE_SGX)
		EXA_Fini(pScreen);

	if (fPtr->fbmem)
		omap_fb_unmap(fPtr->fb[0]);
	fPtr->fbmem = NULL;
	fPtr->fbmem_len = 0;

	pScrn->vtSema = FALSE;

	pScreen->CloseScreen = fPtr->CloseScreen;
	return (*pScreen->CloseScreen) (CLOSE_SCREEN_ARGS);
}

/* -------------------------------------------------------------------- */

static void update_scrn_info(ScrnInfoPtr pScrn, int width, int height,
			     unsigned int *pitch)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);

	if (!omap_fb_get_info(fPtr->fb[0], NULL, NULL, pitch))
		FatalError("Unable to get framebuffer pitch\n");

	pScrn->virtualX = width;
	pScrn->virtualY = height;
	pScrn->displayWidth = (*pitch << 3) / pScrn->bitsPerPixel;
}

static Bool
fbdev_crtc_config_resize(ScrnInfoPtr pScrn, int width, int height)
{
	ScreenPtr pScreen;
	PixmapPtr pixmap;
	FBDevPtr fPtr;
	unsigned int pitch;
	Bool ret;
	int old_width, old_height;

	DebugF("%s(%p, %d, %d)\n",
	       __func__, pScrn, width, height);

	pScreen = pScrn->pScreen;

	fPtr = FBDEVPTR(pScrn);

	old_width = pScrn->virtualX;
	old_height = pScrn->virtualY;

	if (width == old_width && height == old_height)
		return TRUE;

	if (!pre_fb_reset(pScrn)) {
		FatalError("Unable to clear SGX config\n");
		return FALSE;
	}

	ret = realloc_fb(pScrn, width, height,
			 pScrn->bitsPerPixel, pScrn->depth);
	if (!ret) {
		width  = old_width;
		height = old_height;
	}

	update_scrn_info(pScrn, width, height, &pitch);

	if (!post_fb_reset(pScrn)) {
		if (!ret)
			FatalError("Unable to restore SGX config\n");
		width  = old_width;
		height = old_height;

		if (!realloc_fb(pScrn, width, height,
				pScrn->bitsPerPixel, pScrn->depth))
			FatalError("Unable to restore framebuffer config\n");

		update_scrn_info(pScrn, width, height, &pitch);

		if (!post_fb_reset(pScrn))
			FatalError("Unable to restore SGX config\n");
	}

	/*
	 * Not sure we can trust that the original framebuffer is still
	 * mapped at the same location, so let's do the
	 * ModifyPixmapHeader() thing even when things didn't succeed.
	 */
	pixmap = pScreen->GetScreenPixmap(pScreen);
	if (!pixmap)
		FatalError("Unable to get screen pixmap\n");

	if (!pScreen->ModifyPixmapHeader(pixmap, width, height,
					 pScrn->depth, pScrn->bitsPerPixel,
					 pitch, get_fbmem(fPtr)))
		FatalError("Unable to modify screen pixmap\n");

	return ret;
}

static DisplayModePtr rotate_mode(DisplayModePtr mode)
{
	DisplayModeRec tmp = *mode;

	tmp.HDisplay = mode->VDisplay;
	tmp.HSyncStart = mode->VSyncStart;
	tmp.HSyncEnd = mode->VSyncEnd;
	tmp.HTotal = mode->VTotal;
	tmp.VDisplay = mode->HDisplay;
	tmp.VSyncStart = mode->HSyncStart;
	tmp.VSyncEnd = mode->HSyncEnd;
	tmp.VTotal = mode->HTotal;

	tmp.CrtcHDisplay = tmp.HDisplay;
	tmp.CrtcHSyncStart = tmp.HSyncStart;
	tmp.CrtcHSyncEnd = tmp.HSyncEnd;
	tmp.CrtcHTotal = tmp.HTotal;
	tmp.CrtcVDisplay = tmp.VDisplay;
	tmp.CrtcVSyncStart = tmp.VSyncStart;
	tmp.CrtcVSyncEnd = tmp.VSyncEnd;
	tmp.CrtcVTotal = tmp.VTotal;

	tmp.type = M_T_DRIVER;

	return xf86DuplicateMode(&tmp);
}

static const xf86CrtcConfigFuncsRec fbdev_crtc_config_funcs = {
	.resize = fbdev_crtc_config_resize,
};

static Bool fbdev_randr12_preinit(ScrnInfoPtr pScrn)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);
	DisplayModeRec mode = { .Clock = 0 };
	unsigned int pixclk, hdisp, hfp, hsw, hbp, vdisp, vfp, vsw, vbp;

	if (!omap_output_get_timings(fPtr->out[0], &pixclk,
				     &hdisp, &hfp, &hsw, &hbp,
				     &vdisp, &vfp, &vsw, &vbp)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Can't get display timings\n");
		return FALSE;
	}

	mode.Clock = pixclk;
	mode.HDisplay = hdisp;
	mode.HSyncStart = mode.HDisplay + hfp;
	mode.HSyncEnd = mode.HSyncStart + hsw;
	mode.HTotal = mode.HSyncEnd + hbp;
	mode.VDisplay = vdisp;
	mode.VSyncStart = mode.VDisplay + vfp;
	mode.VSyncEnd = mode.VSyncStart + vsw;
	mode.VTotal = mode.VSyncEnd + vbp;

	mode.SynthClock = mode.Clock;
	mode.CrtcHDisplay = mode.HDisplay;
	mode.CrtcHSyncStart = mode.HSyncStart;
	mode.CrtcHSyncEnd = mode.HSyncEnd;
	mode.CrtcHTotal = mode.HTotal;
	mode.CrtcVDisplay = mode.VDisplay;
	mode.CrtcVSyncStart = mode.VSyncStart;
	mode.CrtcVSyncEnd = mode.VSyncEnd;
	mode.CrtcVTotal = mode.VTotal;

	mode.type = M_T_DRIVER;

	/*
	 * Without some idea about refresh rate all default
	 * modes will be rejected. This hack allows them in.
	 */
#if 0
	/* FIXME if more modes are in the list the server sets up the
	 * preferred mode on startup but still xrandr shows that the
	 * displays are off. What's going on?
	 */
	if (!mode.Clock)
		mode.Clock = mode.HTotal * mode.VTotal * 60;
#endif

	xf86CrtcConfigInit(pScrn, &fbdev_crtc_config_funcs);

	/* SGX has a 2048x2048 maximum texture size limit */
	/* FIXME should be limited further based on the amount of vram */
	xf86CrtcSetSizeRange(pScrn, 1, 1, 2048, 2048);

	fPtr->crtc_lcd = fbdev_crtc_create(pScrn, FBDEV_OVERLAY_USAGE_CRTC0);
	if (!fPtr->crtc_lcd)
		return FALSE;
	fPtr->crtc_tv = fbdev_crtc_create(pScrn, FBDEV_OVERLAY_USAGE_CRTC1);
	if (!fPtr->crtc_tv)
		return FALSE;

	fPtr->builtin_lcd = xf86DuplicateMode(&mode);
	fPtr->builtin_lcd->type |= M_T_PREFERRED;

	fPtr->output_lcd = fbdev_output_create(pScrn, "LCD",
					       FBDEV_OUTPUT_TYPE_LCD,
					       fPtr->out[0], 0);
	if (!fPtr->output_lcd)
		return FALSE;

	/* Select the preferred TV mode to match LCD's initial rotation */
	if (fPtr->output_lcd->initial_rotation & (RR_Rotate_90 |
						  RR_Rotate_270)) {
		fPtr->builtin_tv = rotate_mode(&mode);
		fPtr->builtin_tv->type |= M_T_PREFERRED;
		fPtr->builtin_tv = xf86ModesAdd(fPtr->builtin_tv,
						xf86DuplicateMode(&mode));
	} else {
		fPtr->builtin_tv = xf86DuplicateMode(&mode);
		fPtr->builtin_tv->type |= M_T_PREFERRED;
		fPtr->builtin_tv = xf86ModesAdd(fPtr->builtin_tv,
						rotate_mode(&mode));
	}

	fPtr->output_tv = fbdev_output_create(pScrn, "TV",
					      FBDEV_OUTPUT_TYPE_TV,
					      fPtr->out[1], 1);
	if (!fPtr->output_tv)
		return FALSE;

	if (!xf86InitialConfiguration(pScrn, TRUE)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Impossible initial config\n");
		return FALSE;
	}

	if (!fPtr->conf.can_change_screen_size)
		xf86CrtcSetSizeRange(pScrn, pScrn->virtualX, pScrn->virtualY,
				     pScrn->virtualX, pScrn->virtualY);

	xf86PrintModes(pScrn);

	return TRUE;
}

static void fbdev_randr12_uninit(ScrnInfoPtr pScrn)
{
	FBDevPtr fPtr = FBDEVPTR(pScrn);

	xf86OutputDestroy(fPtr->output_tv);
	xf86OutputDestroy(fPtr->output_lcd);
	xf86CrtcDestroy(fPtr->crtc_tv);
	xf86CrtcDestroy(fPtr->crtc_lcd);

	while (fPtr->builtin_lcd)
		xf86DeleteMode(&fPtr->builtin_lcd, fPtr->builtin_lcd);
	while (fPtr->builtin_tv)
		xf86DeleteMode(&fPtr->builtin_tv, fPtr->builtin_tv);
}

CARD32 fbdev_rgb_to_pixel(ScrnInfoPtr pScrn,
			  CARD8 red, CARD8 green, CARD8 blue)
{
	return ((red << pScrn->offset.red) & pScrn->mask.red) |
		((green << pScrn->offset.green) & pScrn->mask.green) |
		((blue << pScrn->offset.blue) & pScrn->mask.blue);
}
