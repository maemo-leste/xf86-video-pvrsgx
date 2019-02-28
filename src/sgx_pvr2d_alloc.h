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

#ifndef SGX_PVR2D_ALLOC_H
#define SGX_PVR2D_ALLOC_H

extern unsigned int page_size;

Bool PVR2DDelayedMemDestroy(Bool wait);
void DestroyPVR2DMemory(ScreenPtr pScreen, struct PVR2DPixmap *ppix);
Bool PVR2DAllocatePixmapMem(PixmapPtr pPixmap, struct PVR2DPixmap *ppix,
			    int width, int height, int pitch,
			    pointer pPixData);
Bool pvr2d_dri2_migrate_pixmap(PixmapPtr pPixmap, struct PVR2DPixmap *pix);

#endif
