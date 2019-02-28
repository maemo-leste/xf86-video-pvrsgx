/*
 * Copyright ©-2009 Nokia Corporation
 *
 * Permission to use, copy, modify, distribute and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the names of the authors and/or copyright holders
 * not be used in advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.  The authors and
 * copyright holders make no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without any express
 * or implied warranty.
 *
 * THE AUTHORS AND COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * The external framebuffer supports updating a sub-window
 * of the display. To save power and system bandwidth we
 * use the feature (manual update), but this means we have
 * to tell the fb driver when changes accrue, so we request
 * damage events to the root window's pixmap.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/extensions/dpmsconst.h>

#include "scrnintstr.h"
#include "windowstr.h"
#include "regionstr.h"
#include "damage.h"
#include "damagestr.h"
#include "xf86Crtc.h"

#include "fbdev.h"
#include "extfb.h"
#include "sgx_dri2.h"

void extfb_lock_display_update(ScrnInfoPtr pScrn)
{
       FBDevPtr fbdev = FBDEVPTR(pScrn);
       assert(!fbdev->extfb.update_lock);
       fbdev->extfb.update_lock = TRUE;
}

void extfb_unlock_display_update(ScrnInfoPtr pScrn)
{
       FBDevPtr fbdev = FBDEVPTR(pScrn);
       assert(fbdev->extfb.update_lock);
       fbdev->extfb.update_lock = FALSE;
}

void ExtFBUpdate(ScrnInfoPtr pScrn, RegionPtr update_region)
{
	BoxPtr pBox;
	Bool overlap;

	if (!RegionValidate(update_region, &overlap))
		return;

	/* Create a bounding box that covers all updates */
	pBox = RegionExtents(update_region);

	fbdev_update_outputs(pScrn, pBox);

	DebugF("ExtFB updating.\n");
}

void ExtFBDamage(ScrnInfoPtr pScrn, RegionPtr pRegion)
{
	FBDevPtr fbdev = FBDEVPTR(pScrn);

	if (!fbdev->extfb.enabled)
		return;

	if (fbdev->extfb.update_lock)
		return;

	pvr2d_dri2_schedule_damage((DrawablePtr)fbdev->pixmap, pRegion);
}

static void
DamageExtFBReport(DamagePtr pDamage, RegionPtr pRegion, void *closure)
{
	ScrnInfoPtr pScrn = closure;

	ExtFBDamage(pScrn, pRegion);
}

Bool ExtFBCreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	FBDevPtr fbdev = FBDEVPTR(pScrn);

	if (!fbdev->extfb.enabled)
		return TRUE;

	fbdev->extfb.damage = DamageCreate(DamageExtFBReport,
					   NULL,
					   DamageReportRawRegion,
					   TRUE,
					   pScreen,
					   pScrn);	/* Private token */

	if (fbdev->extfb.damage == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "omap/extfb: Couldn't create "
			   "damage on screen pixmap\n");
		return FALSE;
	}

	DamageSetReportAfterOp(fbdev->extfb.damage, TRUE);

	DamageRegister(&fbdev->pixmap->drawable, fbdev->extfb.damage);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "ExtFB partial update support initialized\n");

	return TRUE;
}

void ExtFBCloseScreen(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	FBDevPtr fbdev = FBDEVPTR(pScrn);

	if (fbdev->extfb.damage) {
		DamageUnregister(&fbdev->pixmap->drawable, fbdev->extfb.damage);
		DamageDestroy(fbdev->extfb.damage);
	}
}
