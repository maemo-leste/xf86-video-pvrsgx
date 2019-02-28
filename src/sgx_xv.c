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

#include "fbdev.h"
#include "sgx_pvr2d.h"
#include "sgx_exa.h"
#include "sgx_xv.h"
#include "omap_video.h"

#include "xf86xv.h"
#include <X11/extensions/Xv.h>
#include "fourcc.h"
#include "damage.h"

#define BRIGHTNESS_DEFAULT_VALUE   0
#define BRIGHTNESS_MIN            -50
#define BRIGHTNESS_MAX             50

#define CONTRAST_DEFAULT_VALUE     0
#define CONTRAST_MIN              -100
#define CONTRAST_MAX               100

#define SATURATION_DEFAULT_VALUE   100
#define SATURATION_MIN             0
#define SATURATION_MAX             200

#define HUE_DEFAULT_VALUE   0
#define HUE_MIN            -30
#define HUE_MAX             30

#define NUM_TEXTURED_XV_PORTS 2

static Atom xvBrightness, xvContrast, xvHue, xvSaturation;

typedef struct _pvr2DPortPrivRec {
	int brightness;
	int contrast;
	int saturation;
	int hue;
	unsigned long sgx_packed_filtervalues[9];
	unsigned long sgx_planar_filtervalues[9];
} pvr2DPortPrivRec, *pvr2DPortPrivPtr;

static XF86VideoEncodingRec DummyEncoding = {
	0, "XV_IMAGE", VIDEO_IMAGE_MAX_WIDTH, VIDEO_IMAGE_MAX_HEIGHT, {1, 1},
};

static XF86VideoFormatRec Formats[] = {
	{15, TrueColor},
	{16, TrueColor},
	{24, TrueColor},
};

static XF86AttributeRec Attributes[] = {
	{XvSettable | XvGettable, BRIGHTNESS_MIN, BRIGHTNESS_MAX,
	 "XV_BRIGHTNESS"},
	{XvSettable | XvGettable, CONTRAST_MIN, CONTRAST_MAX, "XV_CONTRAST"},
	{XvSettable | XvGettable, SATURATION_MIN, SATURATION_MAX,
	 "XV_SATURATION"},
	{XvSettable | XvGettable, HUE_MIN, HUE_MAX, "XV_HUE"},
};

static XF86ImageRec Images[] = {
	XVIMAGE_UYVY,
	XVIMAGE_YUY2,
	XVIMAGE_YV12,
	XVIMAGE_I420,
};

static void pvr2DQueryBestSize(ScrnInfoPtr pScrn, Bool motion, short vid_w,
			       short vid_h, short drw_w, short drw_h,
			       unsigned int *p_w, unsigned int *p_h,
			       pointer data)
{
	if (vid_w > (drw_w << 1))
		drw_w = vid_w >> 1;
	if (vid_h > (drw_h << 1))
		drw_h = vid_h >> 1;

	*p_w = drw_w;
	*p_h = drw_h;
}

static void pvr2DSetupFilterValues(pvr2DPortPrivPtr pPriv)
{
	unsigned long *sgx_filtervalues = pPriv->sgx_packed_filtervalues;
	//                  Red,  Green, Blue
	short rgbYi[3] = { 75, 149, 37 };	// Y component
	short rgbUi[3] = { 0, -50, 65 };	// U component
	short rgbVi[3] = { 102, -104, 0 };	// V component
	short rgbConst[3] = { -14267, 17354, -8859 };
	short rgbShift[3] = { 6, 7, 5 };

	// In the filter set 2 only taps (bytes) 1,4,7 can be used (counting from 1, left to right 12345678)

	//packed format coefficients
	// filter set 0 (red):          VUYx VUYx
	// filter set 1 (green):        VUYx VUYx
	// filter set 2 (blue):         YUVU YxVU
	sgx_filtervalues[0] = ((rgbVi[0] & 0xff) << 24) | ((rgbUi[0] & 0xff) << 16 | ((((rgbYi[0] + 1) / 2) & 0xff) << 8));
	sgx_filtervalues[1] = (((rgbYi[0] / 2) & 0xff) << 8);
	sgx_filtervalues[2] = ((rgbConst[0] & 0xffff) << 4) | ((rgbShift[0] & 0xf) << 0);

	sgx_filtervalues[3] = ((rgbVi[1] & 0xff) << 24) | ((rgbUi[1] & 0xff) << 16 | ((((rgbYi[1] + 1) / 2) & 0xff) << 8));
	sgx_filtervalues[4] = (((rgbYi[1] / 2) & 0xff) << 8);
	sgx_filtervalues[5] = ((rgbConst[1] & 0xffff) << 4) | ((rgbShift[1] & 0xf) << 0);

	sgx_filtervalues[6] = ((rgbYi[2] & 0xff) << 24) | ((rgbUi[2] & 0xff) << 0);
	sgx_filtervalues[7] = ((rgbVi[2] & 0xff) << 8);
	sgx_filtervalues[8] = ((rgbConst[2] & 0xffff) << 4) | ((rgbShift[2] & 0xf) << 0);

	// planar format coefficients
	// filter set 0 (red):          YYVV VVUU
	// filter set 1 (green):        YYVV VVUU
	// filter set 2 (blue):         YVVV VUUU

	sgx_filtervalues = pPriv->sgx_planar_filtervalues;
	sgx_filtervalues[0] = ((((rgbYi[0] + 1) / 2) & 0xff) << 24) | ((((rgbYi[0]) / 2) & 0xff) << 16) | ((rgbVi[0] & 0xff));
	sgx_filtervalues[1] = ((rgbUi[0] & 0xff) << 8);
	sgx_filtervalues[2] = ((rgbConst[0] & 0xffff) << 4) | ((rgbShift[0] & 0xf) << 0);

	sgx_filtervalues[3] = ((((rgbYi[1] + 1) / 2) & 0xff) << 24) | ((((rgbYi[1]) / 2) & 0xff) << 16) | (rgbVi[1] & 0xff);
	sgx_filtervalues[4] = ((rgbUi[1] & 0xff) << 8);
	sgx_filtervalues[5] = ((rgbConst[1] & 0xffff) << 4) | ((rgbShift[1] & 0xf) << 0);

	sgx_filtervalues[6] = ((rgbYi[2] & 0xff) << 24) | (rgbVi[2] & 0xff);
	sgx_filtervalues[7] = ((rgbUi[2] & 0xff) << 8);
	sgx_filtervalues[8] = ((rgbConst[2] & 0xffff) << 4) | ((rgbShift[2] & 0xf) << 0);
}

static int pvr2DSetPortAttribute(ScrnInfoPtr pScrn, Atom attribute, INT32 value,
				 pointer data)
{
	pvr2DPortPrivPtr pPriv = (pvr2DPortPrivPtr) data;

	if (attribute == xvBrightness) {
		pPriv->brightness =
		    ClipValue(value, BRIGHTNESS_MAX, BRIGHTNESS_MIN);
	} else if (attribute == xvContrast) {
		pPriv->contrast = ClipValue(value, CONTRAST_MAX, CONTRAST_MIN);
	} else if (attribute == xvSaturation) {
		pPriv->saturation =
		    ClipValue(value, SATURATION_MAX, SATURATION_MIN);
	} else if (attribute == xvHue) {
		pPriv->hue = ClipValue(value, HUE_MAX, HUE_MIN);
	} else
		return BadValue;

	pvr2DSetupFilterValues(pPriv);

	return Success;
}

static int pvr2DGetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
				 INT32 * value, pointer data)
{
	pvr2DPortPrivPtr pPriv = (pvr2DPortPrivPtr) data;

	if (attribute == xvBrightness)
		*value = pPriv->brightness;
	else if (attribute == xvContrast)
		*value = pPriv->contrast;
	else if (attribute == xvSaturation)
		*value = pPriv->saturation;
	else if (attribute == xvHue)
		*value = pPriv->hue;
	else
		return BadValue;

	return Success;
}

static int pvr2DQueryImageAttributes(ScrnInfoPtr pScrn, int id,
				     unsigned short *w, unsigned short *h,
				     int *pitches, int *offsets)
{
	int size, tmp;

	if (*w > VIDEO_IMAGE_MAX_WIDTH)
		*w = VIDEO_IMAGE_MAX_WIDTH;
	if (*h > VIDEO_IMAGE_MAX_HEIGHT)
		*h = VIDEO_IMAGE_MAX_HEIGHT;

	*w = ALIGN(*w, 2);
	if (offsets)
		offsets[0] = 0;

	switch (id) {
	case FOURCC_YV12:
	case FOURCC_I420:
		*h = ALIGN(*h, 2);
		size = ALIGN(*w, 4);
		if (pitches)
			pitches[0] = size;
		size *= *h;
		if (offsets)
			offsets[1] = size;
		tmp = ALIGN(*w >> 1, 4);
		if (pitches)
			pitches[1] = pitches[2] = tmp;
		tmp *= (*h >> 1);
		size += tmp;
		if (offsets)
			offsets[2] = size;
		size += tmp;
		break;
	case FOURCC_UYVY:
	case FOURCC_YUY2:
	default:
		size = *w << 1;
		if (pitches)
			pitches[0] = size;
		size *= *h;
		break;
	}

	return size;

}

static PVR2DEXTBLTINFO pvr2dextblt;
/* putImage needs 1 source surface for packed and 3 source surfaces for planar formats.
 * We keep two sets of source surfaces for asynchronous operation */
static struct _Mem {
	PVR2DMEMINFO *pMemInfo;
	unsigned size;
} MemSet[2][3], *pMem;

static void freeMem(struct _Mem *pMem)
{
	PVR2DCONTEXTHANDLE context = pvr2d_get_screen()->context;

	if (!pMem->pMemInfo)
		return;

	if (PVR2DQueryBlitsComplete(context, pMem->pMemInfo, 0) != PVR2D_OK)
		DBG("%s: Pending blits in free memory!\n", __func__);

	PVR2DQueryBlitsComplete(context, pMem->pMemInfo, 1);
	PVR2DMemFree(context, pMem->pMemInfo);
	pMem->pMemInfo = NULL;
	pMem->size = 0;
}

static Bool allocMem(struct _Mem *pMem, unsigned size)
{
	if (pMem->size >= size)
		return TRUE;
	freeMem(pMem);

	if (!pMem->pMemInfo
	    && PVR2DMemAlloc(pvr2d_get_screen()->context, size, 4, 0,
			     &pMem->pMemInfo) != PVR2D_OK) {
		pMem->pMemInfo = NULL;
		return FALSE;
	}
	pMem->size = size;
	return TRUE;
}

static void pvr2DStopVideo(ScrnInfoPtr pScrn, pointer data, Bool cleanup)
{
	DBG("%s(pScrn, %p, %s\n", __func__, data, cleanup ? "TRUE" : "FALSE");

	if (cleanup) {
		int i;

		for (i = 0; i < 3; i++) {
			freeMem(&MemSet[0][i]);
			freeMem(&MemSet[1][i]);
		}
	}
}

static int initSrcSurf(int surfNum, unsigned width, unsigned stride,
		       unsigned height, void *buf, unsigned buf_stride)
{
	PVR2DCONTEXTHANDLE context = pvr2d_get_screen()->context;

	if (!allocMem(&pMem[surfNum], stride * height))
		return BadAlloc;

	DBG("Preparing surface %i, w=%i,stride=%i,height=%i\n", surfNum, width,
	    stride, height);
	pvr2dextblt.SrcSurface[surfNum].pSrcMemInfo = pMem[surfNum].pMemInfo;
	pvr2dextblt.SrcSurface[surfNum].SrcFilterMode = PVR2D_FILTER_LINEAR;
	pvr2dextblt.SrcSurface[surfNum].SrcRepeatMode = PVR2D_REPEAT_NONE;
	pvr2dextblt.SrcSurface[surfNum].SrcSurfWidth = width;
	pvr2dextblt.SrcSurface[surfNum].SrcStride = stride;
	pvr2dextblt.SrcSurface[surfNum].SrcSurfHeight = height;

	/* this is just a debugging check to see if blits have completed on a
	 * source surface before we try to populate it with new data
	 */
	if (PVR2DQueryBlitsComplete(context, pMem[surfNum].pMemInfo, 0) !=
		PVR2D_OK) {
		DBG("%s: Pending blits!\n", __func__);
		PVR2DQueryBlitsComplete(context, pMem[surfNum].pMemInfo, 1);
	}

	if (stride == buf_stride)
		memcpy(pvr2dextblt.SrcSurface[surfNum].pSrcMemInfo->pBase, buf,
		       stride * height);
	else {
		int i;
		unsigned copy_size = min(stride, buf_stride);

		for (i = 0; i < height; i++)
			memcpy(pvr2dextblt.SrcSurface[surfNum].pSrcMemInfo->
			       pBase + i * stride, buf + i * buf_stride,
			       copy_size);
	}

	return Success;
}

static int pvr2DPutImage(ScrnInfoPtr pScrn, short src_x, short src_y,
			 short drw_x, short drw_y, short src_w, short src_h,
			 short drw_w, short drw_h, int id, unsigned char *buf,
			 short width, short height, Bool Sync,
			 RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
	pvr2DPortPrivPtr pPriv = (pvr2DPortPrivPtr) data;
	float texcoords[4];
	unsigned src_stride;
	unsigned tex_stride;
	int sgx_pitch_align;
	int ret;
	unsigned long *sgx_filtervalues = 0;

	static int memsetSelector;
	pMem = MemSet[memsetSelector++];
	memsetSelector &= 1;

	DBG("%s(pScrn, %d, %d, %d, %d, %d, %d, %d, %d, %d, %p, %d, %d, %s, %p, %p, %p\n", __func__, src_x, src_y, drw_x, drw_y, src_w, src_h, drw_w, drw_h, id, buf, width, height, Sync ? "TRUE" : "FALSE", clipBoxes, data, pDraw);

	if (!getDrawableInfo
	    (pDraw, &pvr2dextblt.pDstMemInfo, &pvr2dextblt.DstX,
	     &pvr2dextblt.DstY))
		return BadDrawable;

	if (!GetPVR2DFormat(pDraw->depth, &pvr2dextblt.DstFormat))
		return BadMatch;

	pvr2dextblt.DstX += drw_x;
	pvr2dextblt.DstY += drw_y;
	pvr2dextblt.DSizeX = drw_w;
	pvr2dextblt.DSizeY = drw_h;

	if (pDraw->type == DRAWABLE_WINDOW)
		pvr2dextblt.DstStride =
		    pScrn->pScreen->GetWindowPixmap((WindowPtr) pDraw)->devKind;
	else
		pvr2dextblt.DstStride = ((PixmapPtr) pDraw)->devKind;

	sgx_pitch_align = getSGXPitchAlign(width);

	switch (id) {
	case FOURCC_YUY2:
	case FOURCC_UYVY:
		sgx_filtervalues = pPriv->sgx_packed_filtervalues;
		pvr2dextblt.SrcSurface[0].SrcFormat =
		    id == FOURCC_YUY2 ? PVR2D_YUY2 : PVR2D_UYVY;
		ret =
		    initSrcSurf(0, width,
				ALIGN(2 * width, sgx_pitch_align),
				height, buf + src_y * src_w * 2 + src_x * 2,
				src_w * 2);
		break;
	case FOURCC_YV12:
	case FOURCC_I420:
		sgx_filtervalues = pPriv->sgx_planar_filtervalues;
		pvr2dextblt.SrcSurface[0].SrcFormat =
		    pvr2dextblt.SrcSurface[1].SrcFormat =
		    pvr2dextblt.SrcSurface[2].SrcFormat =
		    id == FOURCC_YV12 ? PVR2D_YV12 : PVR2D_I420;

		src_stride = ALIGN(src_w, 4);
		ret =
		    initSrcSurf(0, width,
				ALIGN(width, sgx_pitch_align), height,
				buf + src_y * src_stride + src_x, src_stride);
		if (ret != Success)
			break;
		buf += src_h * src_stride;

		width = (width + 1) / 2;
		height = (height + 1) / 2;
		src_x /= 2;
		src_y /= 2;
		src_w /= 2;
		src_h /= 2;
		src_stride = ALIGN(src_w, 4);
		tex_stride = ALIGN(width, sgx_pitch_align);
		ret =
		    initSrcSurf(1, width, tex_stride, height,
				buf + src_y * src_stride + src_x, src_stride);
		if (ret != Success)
			break;
		buf += src_h * src_stride;

		ret =
		    initSrcSurf(2, width, tex_stride, height,
				buf + src_y * src_stride + src_x, src_stride);
		break;
	default:
		return BadMatch;
	}

	if (ret != Success)
		return ret;

	texcoords[0] = 0 /*(float)src_x / (float)src_w */ ;
	texcoords[1] = 0 /*(float)src_y / (float)src_h */ ;

	texcoords[2] = 1 /*(float)min(src_x + height, src_w) / (float)src_w */ ;
	texcoords[3] = 1 /*(float)min(src_y + height, src_h) / (float)src_h */ ;

	DamageDamageRegion(pDraw, clipBoxes);

	if (PVR2DVideoBlt(pvr2d_get_screen()->context, &pvr2dextblt, texcoords,
				sgx_filtervalues) != PVR2D_OK)
		return BadImplementation;

	return Success;
}

XF86VideoAdaptorPtr pvr2dSetupTexturedVideo(ScreenPtr pScreen)
{
	XF86VideoAdaptorPtr adapt;
	pvr2DPortPrivPtr pPriv;
	int i;

	if (!(adapt = calloc(1, sizeof(XF86VideoAdaptorRec))))
		return NULL;

	adapt->type = XvWindowMask | XvInputMask | XvImageMask;
	adapt->flags = VIDEO_OVERLAID_IMAGES /*| VIDEO_CLIP_TO_VIEWPORT */ ;
	adapt->name = "SGX Textured Video";
	adapt->nEncodings = 1;
	adapt->pEncodings = &DummyEncoding;

	adapt->nFormats = ARRAY_SIZE(Formats);
	adapt->pFormats = Formats;

	adapt->nAttributes = ARRAY_SIZE(Attributes);
	adapt->pAttributes = Attributes;

	adapt->nImages = ARRAY_SIZE(Images);
	adapt->pImages = Images;

	adapt->StopVideo = pvr2DStopVideo;
	adapt->SetPortAttribute = pvr2DSetPortAttribute;
	adapt->GetPortAttribute = pvr2DGetPortAttribute;
	adapt->QueryBestSize = pvr2DQueryBestSize;
	adapt->PutImage = pvr2DPutImage;
	adapt->QueryImageAttributes = pvr2DQueryImageAttributes;

	adapt->pPortPrivates = (DevUnion *)
	    calloc(NUM_TEXTURED_XV_PORTS, sizeof(DevUnion));

	if (!adapt->pPortPrivates)
		goto out_err;

	adapt->nPorts = 0;
	for (i = 0; i < NUM_TEXTURED_XV_PORTS; ++i) {
		pPriv = calloc(1, sizeof(pvr2DPortPrivRec));
		if (!pPriv)
			goto out_err;

		pvr2DSetupFilterValues(pPriv);

		adapt->pPortPrivates[i].ptr = (pointer) pPriv;
		adapt->nPorts++;
	}

	xvBrightness = MAKE_ATOM("XV_BRIGHTNESS");
	xvContrast = MAKE_ATOM("XV_CONTRAST");
	xvHue = MAKE_ATOM("XV_HUE");
	xvSaturation = MAKE_ATOM("XV_SATURATION");

	return adapt;

out_err:
	if (adapt->pPortPrivates)
		for (i = 1; i <= NUM_TEXTURED_XV_PORTS; ++i) {
			pPriv = adapt->pPortPrivates[i - 1].ptr;
			free(pPriv);
		}
	free(adapt->pPortPrivates);
	free(adapt);

	return NULL;
}
