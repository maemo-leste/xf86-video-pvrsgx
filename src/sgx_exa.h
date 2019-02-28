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

#ifndef SGX_EXA_H

#define SGX_EXA_H 1

#include "sgx_pvr2d.h"

/*
 * Hints to CreatePixmap to tell the driver how the pixmap is going to be used.
 * Compare to CREATE_PIXMAP_USAGE_* in the server.
 */
enum {
	SGX_EXA_CREATE_PIXMAP_FLIP = 0x10000000
};

extern Bool getDrawableInfo(DrawablePtr pDraw, PVR2DMEMINFO ** ppMemInfo,
			    long *pXoff, long *pYoff);

extern int getSGXPitchAlign(int width);

extern Bool GetPVR2DFormat(int depth, PVR2DFORMAT * format);

extern void PVR2DUnmapAllPixmaps(void);

extern Bool EXA_Init(ScreenPtr pScreen);

extern void EXA_Fini(ScreenPtr pScreen);

#endif /* SGX_EXA_H */
