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

#ifndef SGX_EXA_USER_H
#define SGX_EXA_USER_H

void sgx_exa_copy_region(DrawablePtr draw, RegionPtr reg, DrawablePtr src_draw,
			DrawablePtr dst_draw);

void sgx_exa_set_reg(const DrawablePtr draw, RegionRec *reg);

/*
 * Use X-server's EXA component to copy contents of a given drawable into/from
 * or between native PVR2D pixel buffers.
 */
PixmapPtr sgx_exa_create_pixmap(ScreenPtr pScreen,
				int width, int height,
				int depth, int bitsPerPixel,
				int pitch, PVR2DMEMINFO *mem_info);

Bool sgx_exa_update_pixmap(PixmapPtr pPixmap,
			   int width, int height,
			   int depth, int bitsPerPixel,
			   int pitch, PVR2DMEMINFO *mem_info);

#endif /* SGX_EXA_USER_H */
