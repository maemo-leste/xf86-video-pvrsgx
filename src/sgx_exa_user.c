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

#include "exa.h"
#include "sgx_exa.h"
#include "sgx_exa_user.h"

#include "compat-api.h"

void sgx_exa_copy_region(DrawablePtr draw, RegionPtr reg, DrawablePtr src_draw,
			DrawablePtr dst_draw)
{
        ScreenPtr screen = dst_draw->pScreen;
	RegionPtr copy_clip = RegionCreate(NULL, 0);
        GCPtr gc;

        gc = GetScratchGC(dst_draw->depth, screen);
	RegionCopy(copy_clip, reg);
        (*gc->funcs->ChangeClip)(gc, CT_REGION, copy_clip, 0);
        ValidateGC(dst_draw, gc);

        (*gc->ops->CopyArea)(src_draw, dst_draw, gc, 0, 0, draw->width,
                                draw->height, 0, 0);

        FreeScratchGC(gc);
}

void sgx_exa_set_reg(const DrawablePtr draw, RegionRec *reg)
{
	BoxRec box;

	box.x1 = 0;
	box.y1 = 0;
	box.x2 = draw->width;
	box.y2 = draw->height;

	RegionInit(reg, &box, 0);
}

Bool sgx_exa_update_pixmap(PixmapPtr pPixmap,
			   int width, int height,
			   int depth, int bitsPerPixel,
			   int pitch, PVR2DMEMINFO *mem_info)
{
	ScreenPtr pScreen = pPixmap->drawable.pScreen;
	struct PVR2DPixmap *priv = exaGetPixmapDriverPrivate(pPixmap);
	Bool ret;

	ret = (*pScreen->ModifyPixmapHeader)(pPixmap,
					     width, height,
					     depth, bitsPerPixel,
					     pitch, NULL);

	if (ret) {
		priv->pvr2dmem = mem_info;
		priv->shmaddr = mem_info->pBase;
	}

	return ret;
}

PixmapPtr sgx_exa_create_pixmap(ScreenPtr pScreen,
				int width, int height,
				int depth, int bitsPerPixel,
				int pitch, PVR2DMEMINFO *mem_info)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	PixmapPtr pPixmap;
	Bool ret;

	pPixmap = (*pScreen->CreatePixmap)(pScreen, 0, 0, pScrn->depth,
					   SGX_EXA_CREATE_PIXMAP_FLIP);
	if (!pPixmap)
		return NULL;

	ret = sgx_exa_update_pixmap(pPixmap, width, height, depth,
				    bitsPerPixel, pitch, mem_info);
	if (!ret) {
		(*pScreen->DestroyPixmap)(pPixmap);
		return NULL;
	}

	return pPixmap;
}

/* EOF */
