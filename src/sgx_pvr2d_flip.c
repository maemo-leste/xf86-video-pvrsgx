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

#include "sgx_pvr2d_flip.h"
#include "xf86.h" /* ErrorF */

static int pvr2d_flip_get_bufs(PVR2DCONTEXTHANDLE ctx, unsigned buf_len,
				void **bufs_in, unsigned num_bufs,
				struct pvr2d_fs_flip_buf *bufs_out)
{
	int i;

	for (i = 0; i < num_bufs; ++i) {
		bufs_out[i].user_priv = 0;

		if (PVR2DMemWrap(ctx, bufs_in[i], PVR2D_WRAPFLAG_CONTIGUOUS,
			buf_len, 0, &bufs_out[i].mem_info) != PVR2D_OK) {
			ErrorF("%s: Failed to wrap FB memory\n", __func__);
			return 1;
		}
 
		if (PVR2DMemExport(ctx, 0, bufs_out[i].mem_info,
				&bufs_out[i].mem_handle) != PVR2D_OK) {
			ErrorF("%s: Error exporting buffer %d\n", __func__, i);
			return 1;
		}
	}

	return 0;
}

static void pvr2d_flip_put_bufs(PVR2DCONTEXTHANDLE ctx,
				struct pvr2d_fs_flip_buf *bufs,
				unsigned num_bufs)
{
	int i;

	/*
	 * There is no 'Unexport' required by the pvr2d library at the moment.
	 * The counterpart of PVR2DMemWrap is PVR2DMemFree.
	 */
	for (i = 0; i < num_bufs; ++i) {
		PVR2DQueryBlitsComplete(ctx, bufs[i].mem_info, TRUE);

		if (PVR2DMemFree(ctx, bufs[i].mem_info) != PVR2D_OK)
			ErrorF("%s: Failed to free flip buffer\n", __func__);
	}
}

int pvr2d_page_flip_create(PVR2DCONTEXTHANDLE ctx, unsigned buf_len,
			unsigned stride, unsigned bpp, void **bufs_in,
			unsigned num_bufs, struct pvr2d_page_flip *page_flip)
{
	int i;

	if (!bpp || num_bufs > MAX_PAGE_FLIP_BUFFERS)
		return 1;
 
	if (pvr2d_flip_get_bufs(ctx, buf_len, bufs_in, num_bufs,
		page_flip->bufs))
		return 1;
 
	page_flip->stride = stride;
	page_flip->bpp = bpp;
	page_flip->num_bufs = num_bufs;
	page_flip->front_idx = 0;
	page_flip->back_idx = (page_flip->front_idx + 1) % num_bufs;
	page_flip->user_priv = 0;

	for (i = 0; i < num_bufs; i++)
		page_flip->bufs[i].busy = FALSE;
	page_flip->bufs[page_flip->front_idx].busy = TRUE;
 
	return 0;
}

void pvr2d_page_flip_destroy(PVR2DCONTEXTHANDLE ctx,
			struct pvr2d_page_flip *page_flip)
{
	pvr2d_flip_put_bufs(ctx, page_flip->bufs, page_flip->num_bufs);
	memset(page_flip, 0, sizeof(*page_flip));
}
