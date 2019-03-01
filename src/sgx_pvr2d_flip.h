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

#ifndef SGX_PVR2D_FLIP_H
#define SGX_PVR2D_FLIP_H

#include <xorg-server.h>
#include <X11/Xdefs.h>
#include <pixmap.h>
#include <pvr2d.h>

struct pvr2d_fs_flip_buf {
	PVR2DMEMINFO *mem_info;
	PVR2D_HANDLE mem_handle;
	void *user_priv;
	Bool busy;
	Bool reserved;

	/* For copies to/from this buffer */
	PixmapPtr pixmap;
};

#define DEFAULT_PAGE_FLIP_BUFFERS 3

/*
 * The code doesn't impose any upper limit for this. Note that
 * the EGL client side mapping cache should be able to hold at
 * least this many mappings. Otherwise performance will suffer
 * as the buffers need to be constantly remapped while flipping.
 * So no real point in going above the mapping cache size.
 */
#define MAX_PAGE_FLIP_BUFFERS 3

struct pvr2d_page_flip {
	unsigned stride;
	unsigned bpp;
	unsigned num_bufs;
	struct pvr2d_fs_flip_buf bufs[MAX_PAGE_FLIP_BUFFERS];

	unsigned front_idx;
	unsigned back_idx;
	void *user_priv;

	/* used for sanity checking */
	unsigned int completes_pending;
	unsigned int flips_pending;
};

int pvr2d_page_flip_create(PVR2DCONTEXTHANDLE ctx, unsigned buf_len,
			unsigned stride, unsigned bpp, void **bufs,
			unsigned num_bufs, struct pvr2d_page_flip *page_flip);

void pvr2d_page_flip_destroy(PVR2DCONTEXTHANDLE ctx,
			struct pvr2d_page_flip *page_flip);

#endif /* SGX_PVR2D_FLIP_H */
