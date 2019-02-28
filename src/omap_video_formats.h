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

#ifndef OMAP_VIDEO_FORMATS_H
#define OMAP_VIDEO_FORMATS_H

void omap_copy_packed(CARD8 * src, CARD8 * dst,
		      int srcPitch, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h);

/**
 * Copy I420/YV12 data to YUY2, with no scaling.  Originally from kxv.c.
 */
void omap_copy_planar(CARD8 * src, CARD8 * dst,
		      int srcPitch, int srcPitch2, int dstPitch,
		      int srcW, int srcH,
		      int left, int top,
		      int w, int h,
		      int id);


void omap_copy_16(CARD8 * src, CARD8 * dst,
		  int srcPitch, int dstPitch,
		  int srcW, int srcH,
		  int left, int top,
		  int w, int h);

void omap_copy_32(CARD8 * src, CARD8 * dst,
		  int srcPitch, int dstPitch,
		  int srcW, int srcH,
		  int left, int top,
		  int w, int h);

#endif /* OMAP_VIDEO_FORMATS_H */
