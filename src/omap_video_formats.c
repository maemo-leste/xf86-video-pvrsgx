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

/**
 * None of the functions in this file need to deal with rotation:
 * we can rotate on scanout from the LCD controller, so all our
 * copies are unrotated, as rotating while copying is a horrific
 * speed hit.
 */

#ifdef HAVE_KDRIVE_CONFIG_H
#include <kdrive-config.h>
#endif

#include "fbdev.h"
#include <fourcc.h>
#include "omap_video_formats.h"

/**
 * Copy YUV422/YUY2 data with no scaling.
 */
void omap_copy_packed(CARD8 * src, CARD8 * dst,
		      int srcPitch, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h)
{
	src += top * srcPitch + (left << 1);

	/* memcpy FTW on ARM. */
	if (srcPitch == dstPitch && !left) {
		memcpy(dst, src, srcH * srcPitch);
	} else {
		while (srcH--) {
			memcpy(dst, src, srcW << 1);
			src += srcPitch;
			dst += dstPitch;
		}
	}
}

/**
 * Copy I420/YV12 data to YUY2, with no scaling.  Originally from kxv.c.
 */
void omap_copy_planar(CARD8 * src, CARD8 * dst,
		      int srcPitch, int srcPitch2, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h,
		      int id)
{
	int i, j;
	CARD8 *src1, *src2, *src3, *dst1;

	/* compute source data pointers */
	src1 = src;
	src2 = src1 + h * srcPitch;
	src3 = src2 + (h >> 1) * srcPitch2;

	src += top * srcPitch + left;
	src2 += (top >> 1) * srcPitch2 + (left >> 1);
	src3 += (top >> 1) * srcPitch2 + (left >> 1);

	if (id == FOURCC_I420) {
		CARD8 *srct = src3;
		src3 = src2;
		src2 = srct;
	}

	dst1 = dst;

	srcW >>= 1;
	for (j = 0; j < srcH; j++) {
		CARD32 *dst = (CARD32 *) dst1;
		CARD16 *s1 = (CARD16 *) src1;
		CARD8 *s2 = src2;
		CARD8 *s3 = src3;

		for (i = 0; i < srcW; i++) {
			*dst++ =
			    (*s1 & 0x00ff) | ((*s1 & 0xff00) << 8) | (*s3 << 8)
			    | (*s2 << 24);
			s1++;
			s2++;
			s3++;
		}
		src1 += srcPitch;
		dst1 += dstPitch;
		if (j & 1) {
			src2 += srcPitch2;
			src3 += srcPitch2;
		}
	}
}

/**
 * Copy 16 bpp data with no scaling.
 */
void omap_copy_16(CARD8 * src, CARD8 * dst,
		  int srcPitch, int dstPitch,
		  int srcW, int srcH,
		  int left, int top,
		  int w, int h)
{
	src += top * srcPitch + (left << 1);

	/* memcpy FTW on ARM. */
	if (srcPitch == dstPitch && !left) {
		memcpy(dst, src, srcH * srcPitch);
	} else {
		while (srcH--) {
			memcpy(dst, src, srcW << 1);
			src += srcPitch;
			dst += dstPitch;
		}
	}
}

/**
 * Copy 32 bpp data with no scaling.
 */
void omap_copy_32(CARD8 * src, CARD8 * dst,
		  int srcPitch, int dstPitch,
		  int srcW, int srcH,
		  int left, int top,
		  int w, int h)
{
	src += top * srcPitch + (left << 2);

	/* memcpy FTW on ARM. */
	if (srcPitch == dstPitch && !left) {
		memcpy(dst, src, srcH * srcPitch);
	} else {
		while (srcH--) {
			memcpy(dst, src, srcW << 2);
			src += srcPitch;
			dst += dstPitch;
		}
	}
}
