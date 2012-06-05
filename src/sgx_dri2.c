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

#include "xorg-server.h"
#include "xf86.h"
#include "exa.h"

#include "dri2.h"
#include "pvr2d.h"

#include "extfb.h"
#include "sgx_dri2.h"
#include "sgx_pvr2d.h"
#include "sgx_pvr2d_alloc.h"
#include "fbdev.h"
#include "omap.h"
#include "sgx_exa_user.h"
#include "extfb.h"
#include "flip_stats.h"
#include "linux/omapfb.h"

#include <stdbool.h>

/* This is used in flags in the dri2 buffer structure */
#define PVR2D_MEMORY (1<<0)

//#define DebugF	ErrorF

struct pvr2d_buf_priv {
	PixmapPtr pixmap;
	Bool reserved;
};

struct dri2_swap_request *swap_reqs_head;
struct dri2_swap_request *swap_reqs_tail;

static PixmapPtr get_drawable_pixmap(DrawablePtr draw)
{
	ScreenPtr screen = draw->pScreen;

	if (draw->type == DRAWABLE_WINDOW)
		return (*screen->GetWindowPixmap)((WindowPtr)draw);

	return (PixmapPtr)draw;
}

/* Sets the parameters of the given DRI2 buffer according to the existing front
 * buffer of the given drawable. In case back buffer is needed, one in addition
 * allocates a pixmap with corresponding buffer.
 */
static int pvr2d_dri2_get_non_sys_buf(DrawablePtr draw,
				      DRI2BufferPtr buf)
{
	ScreenPtr screen = draw->pScreen;
	struct pvr2d_buf_priv *priv = buf->driverPrivate;
	struct PVR2DPixmap *pix;

	if (buf->attachment == DRI2BufferFrontLeft) {
		priv->pixmap = get_drawable_pixmap(draw);
		priv->pixmap->refcnt++;
	} else
		priv->pixmap = (*screen->CreatePixmap)(screen,
					draw->width, draw->height,
					draw->depth,
					CREATE_PIXMAP_USAGE_BACKING_PIXMAP);

	if (!priv->pixmap)
		return 1;

	pix = exaGetPixmapDriverPrivate(priv->pixmap);

	if (!pvr2d_dri2_migrate_pixmap(priv->pixmap, pix)) {
		/* clean up pixmaps we might have allocated */
		(*screen->DestroyPixmap)(priv->pixmap);
		priv->pixmap = NULL;
		return 1;
	}

	buf->pitch = priv->pixmap->devKind;
	buf->cpp = priv->pixmap->drawable.bitsPerPixel / 8;
	buf->name = pix->shmid;
	buf->flags = 0;

	return 0;
}

static void pvr2d_dri2_get_sys_buf(DrawablePtr draw,
				   DRI2BufferPtr buf)
{
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;
	struct pvr2d_buf_priv *priv = buf->driverPrivate;

	if (buf->attachment == DRI2BufferFrontLeft) {
		buf->name = (unsigned)
			page_flip->bufs[page_flip->front_idx].mem_handle;
		page_flip->bufs[page_flip->front_idx].user_priv = buf;
		priv->pixmap = get_drawable_pixmap(draw);
		priv->pixmap->refcnt++;
	}
	else if (buf->attachment == DRI2BufferBackLeft) {
		buf->name = (unsigned)
			page_flip->bufs[page_flip->back_idx].mem_handle;
		page_flip->bufs[page_flip->back_idx].user_priv = buf;
		priv->pixmap =
			page_flip->bufs[page_flip->back_idx].pixmap;
		priv->pixmap->refcnt++;
	}
	else
		FatalError("invalid buffer attachement for sys buffer\n");

	/* Tell the client that the memory is accessible through PVR mapping */
	buf->flags = PVR2D_MEMORY;
	buf->pitch = page_flip->stride;
	buf->cpp = page_flip->bpp / 8;
}

/* Consider if one can use fullscreen page flipping with the given drawable */
static int pvr2d_dri2_fs_flip_capable(struct pvr2d_page_flip *page_flip,
				      DrawablePtr draw)
{
	FBDevPtr fbdev = FBDEVPTR(xf86ScreenToScrn(draw->pScreen));

	if (fbdev->conf.swap_control != DRI2_SWAP_CONTROL_FLIP)
		return 0;

	if (draw->type == DRAWABLE_PIXMAP)
		return 0;

	if (page_flip->num_bufs < 2)
		return 0;

	return DRI2CanFlip(draw);
}

static void kill_swap_req(struct dri2_swap_request *req);
static void swap_reqs_drop_last(void);
static struct dri2_swap_request *swap_reqs_find_by_update(unsigned int idx);
static Bool swap_req_dequeue(struct dri2_swap_request *req);
static void pvr2d_dri2_update_front(struct pvr2d_page_flip *page_flip,
				    ScrnInfoPtr pScrn,
				    unsigned int new_front_idx);

/*
 * One needs to release the flip buffers, create private back buffer for the
 * drawable, create pixmaps for both the back and front buffers and finally
 * explicitly copy the contents in the flip buffer being released into the
 * private back buffer of the drawable.
 */
static int pvr2d_dri2_migrate_from_fs(struct pvr2d_page_flip *page_flip)
{
	DRI2BufferPtr front = page_flip->bufs[page_flip->front_idx].user_priv;
	DRI2BufferPtr back = page_flip->bufs[page_flip->back_idx].user_priv;
	DrawablePtr draw = page_flip->user_priv;
	struct pvr2d_buf_priv *priv = back->driverPrivate;
	PixmapPtr old_pixmap = priv->pixmap;

	assert(front->attachment == DRI2BufferFrontLeft);
	assert(back->attachment == DRI2BufferBackLeft);

	if (pvr2d_dri2_get_non_sys_buf(draw, back)) {
		free(back->driverPrivate);
		(void)memset(back, 0, sizeof(DRI2BufferRec));
		return 1;
	}

	if (pvr2d_dri2_get_non_sys_buf(draw, front)) {
		free(front->driverPrivate);
		(void)memset(front, 0, sizeof(DRI2BufferRec));
		return 1;
	}

	page_flip->bufs[page_flip->back_idx].reserved = FALSE;
	assert(!page_flip->bufs[page_flip->front_idx].reserved);

	page_flip->bufs[page_flip->back_idx].user_priv = 0;
	page_flip->bufs[page_flip->front_idx].user_priv = 0;

	/* There is nothing to be done if one fails in the copy. */
	if (priv->reserved) {
		RegionRec reg;

		sgx_exa_set_reg(draw, &reg);
		sgx_exa_copy_region(draw, &reg,
				    &old_pixmap->drawable,
				    &priv->pixmap->drawable);
	}

	(*draw->pScreen->DestroyPixmap)(old_pixmap);

	page_flip->user_priv = 0;

	return 0;
}

static unsigned int page_flip_buf_idx(struct pvr2d_page_flip *page_flip,
				      DRI2BufferPtr buffer)
{
	switch (buffer->attachment) {
	case DRI2BufferFrontLeft:
		return page_flip->front_idx;
	case DRI2BufferBackLeft:
		return page_flip->back_idx;
	default:
		assert(0);
	}
}

static void page_flip_buf_reserve(struct pvr2d_page_flip *page_flip,
				  DRI2BufferPtr buf)
{
	unsigned int idx = page_flip_buf_idx(page_flip, buf);

	assert(buf->attachment == DRI2BufferBackLeft);

	page_flip->bufs[idx].reserved = TRUE;
}

/*
 * Try to copy the contents of the private buffer into the fullscreen flip
 * buffer and release the private as it is not needed for now.
 */
static void pvr2d_dri2_migrate_to_fs(DrawablePtr draw, DRI2BufferPtr buf)
{
	struct pvr2d_buf_priv *priv = buf->driverPrivate;
	PixmapPtr old_pixmap = priv->pixmap;

	pvr2d_dri2_get_sys_buf(draw, buf);

	/* There is nothing to do if one fails in the copy. */
	if (buf->attachment == DRI2BufferBackLeft && priv->reserved) {
		RegionRec reg;

		sgx_exa_set_reg(draw, &reg);
		sgx_exa_copy_region(draw, &reg,
				    &old_pixmap->drawable,
				    &priv->pixmap->drawable);
	}

	(*draw->pScreen->DestroyPixmap)(old_pixmap);
}

static bool page_flip_possible(struct pvr2d_page_flip *page_flip,
			       DrawablePtr draw)
{
	return pvr2d_dri2_fs_flip_capable(page_flip, draw) &&
		!page_flip->bufs[page_flip->back_idx].busy &&
		(page_flip->user_priv == draw ||
		 !page_flip->bufs[page_flip->back_idx].reserved);
}

static DrawablePtr pvr2d_dri2_choose_drawable(DrawablePtr draw,
						DRI2BufferPtr buffer)
{
	struct pvr2d_buf_priv *priv = buffer->driverPrivate;

	if (buffer->attachment == DRI2BufferFrontLeft)
		return draw;

	return &priv->pixmap->drawable;
}

static void poison_buffer(DrawablePtr draw, DRI2BufferPtr buffer,
			  CARD8 red, CARD8 green, CARD8 blue)
{
	ScreenPtr pScreen = draw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	xRectangle rect = {
		.width = draw->width,
		.height = draw->height,
	};
	ChangeGCVal val = {
		.val = fbdev_rgb_to_pixel(pScrn, red, green, blue),
	};
	GCPtr gc;

	draw = pvr2d_dri2_choose_drawable(draw, buffer);

	gc = GetScratchGC(draw->depth, pScreen);
	ChangeGC(NullClient, gc, GCForeground, &val);
	ValidateGC(draw, gc);

	(*gc->ops->PolyFillRect)(draw, gc, 1, &rect);

	FreeScratchGC(gc);
}

static DRI2BufferPtr pvr2d_dri2_create_buf(DrawablePtr draw,
				unsigned int attachment, unsigned int format)
{
	FBDevPtr fbdev = FBDEVPTR(xf86ScreenToScrn(draw->pScreen));
	DRI2BufferPtr buffer;
	struct pvr2d_buf_priv *priv;
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;

	/*
	 * No special formats supported. Buffer format
	 * will match the drawable.
	 */
	if (format)
		return NULL;

	buffer = calloc(1, sizeof *buffer);
	if (buffer == NULL)
		return NULL;

	priv = calloc(1, sizeof(struct pvr2d_buf_priv));
	if (priv == NULL) {
		free(buffer);
		return NULL;
	}

	buffer->attachment = attachment;
	buffer->format = format;
	buffer->driverPrivate = priv;

	switch (attachment) {
	case DRI2BufferFrontLeft:
	case DRI2BufferBackLeft:
		if (page_flip_possible(page_flip, draw)) {
			if (page_flip->user_priv &&
			    page_flip->user_priv != draw &&
			    pvr2d_dri2_migrate_from_fs(page_flip))
				FatalError("Failed to migrate from fullscreen");

			page_flip->user_priv = draw;

			pvr2d_dri2_get_sys_buf(draw, buffer);

			if (attachment == DRI2BufferBackLeft) {
				page_flip_buf_reserve(page_flip, buffer);
				if (!priv->reserved &&
				    fbdev->conf.poison_getbuffers)
					poison_buffer(draw, buffer,
						      0xff, 0x00, 0x00);

				priv->reserved = TRUE;
			}

			return buffer;
		}
		break;
	case DRI2BufferFrontRight:
	case DRI2BufferBackRight:
	case DRI2BufferFakeFrontLeft:
	case DRI2BufferFakeFrontRight:
		break;
	default:
		return buffer;
	}

	if (pvr2d_dri2_get_non_sys_buf(draw, buffer))
		goto err;

	if (attachment == DRI2BufferBackLeft) {
		if (!priv->reserved && fbdev->conf.poison_getbuffers)
			poison_buffer(draw, buffer, 0xff, 0x00, 0x00);
		priv->reserved = TRUE;
	}

	return buffer;

err:
	free(priv);
	free(buffer);

	return NULL;
}

static Bool dri2_buffer_is_fs(struct pvr2d_page_flip *page_flip,
			      DRI2BufferPtr buffer)
{
	unsigned int idx = page_flip_buf_idx(page_flip, buffer);

	return buffer == page_flip->bufs[idx].user_priv;
}

static void  pvr2d_dri2_reuse_buf(DrawablePtr draw, DRI2BufferPtr buffer)
{
	FBDevPtr fbdev = FBDEVPTR(xf86ScreenToScrn(draw->pScreen));
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;
	struct pvr2d_buf_priv *priv = buffer->driverPrivate;

	switch (buffer->attachment) {
	case DRI2BufferFrontLeft:
	case DRI2BufferBackLeft:
		/* Migrate itself out of fullscreen? */
		if (page_flip->user_priv == draw &&
		    !pvr2d_dri2_fs_flip_capable(page_flip, draw)) {
			assert(dri2_buffer_is_fs(page_flip, buffer));

			if (pvr2d_dri2_migrate_from_fs(page_flip))
				FatalError("Failed to migrate from fullscreen");
		}

		if (page_flip_possible(page_flip, draw)) {
			if (page_flip->user_priv &&
			    page_flip->user_priv != draw &&
			    pvr2d_dri2_migrate_from_fs(page_flip))
				FatalError("Failed to migrate from fullscreen");

			page_flip->user_priv = draw;

			if (!dri2_buffer_is_fs(page_flip, buffer))
				pvr2d_dri2_migrate_to_fs(draw, buffer);

			if (buffer->attachment == DRI2BufferBackLeft)
				page_flip_buf_reserve(page_flip, buffer);
		}

		if (buffer->attachment == DRI2BufferBackLeft) {
			if (!priv->reserved && fbdev->conf.poison_getbuffers)
				poison_buffer(draw, buffer, 0xff, 0x00, 0x00);
			priv->reserved = TRUE;
		}
		break;
	default:
		break;
	}
}

static void pvr2d_dri2_destroy_buf(DrawablePtr draw, DRI2BufferPtr buffer)
{
	ScreenPtr screen = draw->pScreen;
	struct pvr2d_buf_priv *priv;
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;

	if (!buffer)
		return;

	priv = buffer->driverPrivate;

	if (priv->pixmap)
		(*screen->DestroyPixmap)(priv->pixmap);

	if (page_flip->user_priv == draw) {

		/*
		 * Consider if the buffer corresponds to one representing one
		 * of the page flip buffers. In such a case the page flip buffer
		 * needs to be released.
		 */
		if (page_flip->bufs[page_flip->front_idx].user_priv == buffer) {
			page_flip->bufs[page_flip->front_idx].user_priv = 0;
			assert(!page_flip->bufs[page_flip->front_idx].reserved);

			if (!page_flip->bufs[page_flip->back_idx].user_priv)
				page_flip->user_priv = 0;
		}

		if (page_flip->bufs[page_flip->back_idx].user_priv == buffer) {
			page_flip->bufs[page_flip->back_idx].user_priv = 0;
			page_flip->bufs[page_flip->back_idx].reserved = FALSE;

			if (!page_flip->bufs[page_flip->front_idx].user_priv)
				page_flip->user_priv = 0;
		}
	}

	free(buffer->driverPrivate);
	free(buffer);
}

/*
 * Consider if the given drawable has its front buffer set as something else
 * than the drawable suppose to you use currently. This can happen, for example,
 * when the drawable was forced to migrate from fullscreen. At that time there
 * was no private window pixmap available and the drawable was set to use the
 * root pixmap.
 */
static void pvr2d_check_front(DrawablePtr draw, DRI2BufferPtr src_buf,
				DRI2BufferPtr dst_buf)
{
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;
	ScreenPtr screen = draw->pScreen;
	struct pvr2d_buf_priv *priv;
	PixmapPtr pixmap;
	DRI2BufferPtr buf;

	if (dst_buf->attachment == DRI2BufferFrontLeft)
		buf = dst_buf;
	else if (src_buf->attachment == DRI2BufferFrontLeft)
		buf = src_buf;
	else
		return;

	priv = buf->driverPrivate;
	pixmap = get_drawable_pixmap(draw);

	if (priv->pixmap == pixmap)
		return;

	if (priv->pixmap) {
		(*screen->DestroyPixmap)(priv->pixmap);
		priv->pixmap = NULL;
	}

	if (page_flip->user_priv == draw) {
		pvr2d_dri2_get_sys_buf(draw, buf);
	} else {
		if (pvr2d_dri2_get_non_sys_buf(draw, buf)) {
			free(buf->driverPrivate);
			(void)memset(buf, 0, sizeof(DRI2BufferRec));
		}
	}
}

static void pvr2d_dri2_copy_region(DrawablePtr draw, RegionPtr reg,
				DRI2BufferPtr dst_buf, DRI2BufferPtr src_buf)
{
	DrawablePtr src_draw;
	DrawablePtr dst_draw;

	pvr2d_check_front(draw, src_buf, dst_buf);

	src_draw = pvr2d_dri2_choose_drawable(draw, src_buf);
	dst_draw = pvr2d_dri2_choose_drawable(draw, dst_buf);

	sgx_exa_copy_region(draw, reg, src_draw, dst_draw);
}

/*
 * Switch the unique memory area identifiers (names). This is needed to trigger
 * DRI2 clients to switch between the front and back. Server does not consult
 * its DRI2 drivers for every 'DRI2GetBuffers' request but instead the driver is
 * expected to switch the buffer designators "behing the scenes" upon every
 * swap.
 */
static void pvr2d_dri2_exchange_bufs(DrawablePtr draw,
				     DRI2BufferPtr front, DRI2BufferPtr back)
{
	ScreenPtr pScreen = draw->pScreen;
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;
	struct pvr2d_buf_priv *front_priv = front->driverPrivate;
	struct pvr2d_buf_priv *back_priv = back->driverPrivate;

	assert(front->name == (unsigned)
	       page_flip->bufs[page_flip->front_idx].mem_handle);
	assert(back->name == (unsigned)
	       page_flip->bufs[page_flip->back_idx].mem_handle);

	/* Clear information that will be stale after the index adjustment */
	page_flip->bufs[page_flip->front_idx].user_priv = NULL;
	page_flip->bufs[page_flip->back_idx].user_priv = NULL;

	page_flip->front_idx = page_flip->back_idx;
	page_flip->back_idx =
		(page_flip->back_idx + 1) % page_flip->num_bufs;

	/* Update the information to match the new indexes. */
	front->name = (unsigned)
		page_flip->bufs[page_flip->front_idx].mem_handle;
	page_flip->bufs[page_flip->front_idx].user_priv = front;

	back->name = (unsigned)
		page_flip->bufs[page_flip->back_idx].mem_handle;
	page_flip->bufs[page_flip->back_idx].user_priv = back;

	/* Update our pixmap pointers too. */
	(*pScreen->DestroyPixmap)(front_priv->pixmap);
	front_priv->pixmap = get_drawable_pixmap(draw);
	front_priv->pixmap->refcnt++;

	(*pScreen->DestroyPixmap)(back_priv->pixmap);
	back_priv->pixmap = page_flip->bufs[page_flip->back_idx].pixmap;
	back_priv->pixmap->refcnt++;
}

static Bool swap_req_enqueue(struct dri2_swap_request *req)
{
	assert(!swap_reqs_tail == !swap_reqs_head);
	assert(!req->next);

	if (!swap_reqs_tail) {
		swap_reqs_head = swap_reqs_tail = req;
		return FALSE;
	}

	assert(swap_reqs_tail->next == NULL);

	swap_reqs_tail->next = req;
	swap_reqs_tail = req;
	return TRUE;
}

static Bool swap_req_dequeue(struct dri2_swap_request *req)
{
	Bool r = TRUE;
	struct dri2_swap_request *it, *prev = NULL;
	assert(swap_reqs_head && swap_reqs_tail);
	assert(swap_reqs_tail->next == NULL);

	for (it = swap_reqs_head; it != req; it = it->next) {
		assert(it);
		prev = it;
	}

	if (prev)
		prev->next = req->next;
	else
		swap_reqs_head = req->next;

	if (req == swap_reqs_tail) {
		assert(req->next == NULL);
		swap_reqs_tail = prev;
		r = FALSE;
	}

	req->next = NULL;
	return r;
}

static void swap_reqs_drop_last(void)
{
	struct dri2_swap_request *req;

	assert(swap_reqs_head && swap_reqs_tail);
	assert(swap_reqs_tail->next == NULL);

	if (swap_reqs_head == swap_reqs_tail) {
		swap_reqs_head = swap_reqs_tail = NULL;
		return;
	}

	for (req = swap_reqs_head; req; req = req->next) {
		if (req->next != swap_reqs_tail)
			continue;

		req->next = NULL;
		swap_reqs_tail = req;
		break;
	}
}

static struct dri2_swap_request *swap_reqs_find_by_update(unsigned int idx)
{
	struct dri2_swap_request *it;

	for (it = swap_reqs_head; it; it = it->next) {
		if (it->display_update_idx == idx)
			return it;
	}
	return NULL;
}

static struct dri2_swap_request *swap_reqs_find_first_pending_flip(void)
{
	struct dri2_swap_request *it;

	for (it = swap_reqs_head; it; it = it->next) {
		if (it->type == SWAP_FLIP && !it->complete_done)
			return it;
	}
	return NULL;
}

/**
 * This handles the special case when rendering completes out of order
 * and vsync is disabled. In that case flips has to complete after render is
 * completed but in-order.
 *
 * For vsync enabled case this function always returns head or NULL depending on if
 * render is done.
 */
static struct dri2_swap_request *swap_reqs_find_next_flip(
		struct dri2_swap_request *req)
{
	struct dri2_swap_request *it;

	for (it = swap_reqs_head; it; it = it->next) {
		/*
		 * If given request has display update enabled select only
		 * requests that also have display update enabled and
		 * vice-versa.
		 */
		if ((req->display_update_idx == SWAP_INVALID_IDX) !=
			(it->display_update_idx == SWAP_INVALID_IDX))
			continue;

		/* If render is not done we shouldn't flip */
		if (it->render_done)
			return it;
		return NULL;
	}
	return NULL;
}

static void free_swap_req(struct dri2_swap_request *req)
{
	RegionUninit(&req->update_region);

	free(req);
}

static void kill_swap_req(struct dri2_swap_request *req)
{
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;

	if (!req->complete_done) {
		DrawablePtr draw;

		assert(req->type == SWAP_FLIP);

		flip_stats_swap_completed_dead();

		/*
		 * Complete it so the previous client can continue with it's
		 * new and shiny private buffers. This is of course possible
		 * only if the very drawable still exists.
		 */
		if (dixLookupDrawable(&draw, req->drawable_id, serverClient,
					M_ANY, DixWriteAccess) == Success)
			DRI2SwapComplete(req->client, draw, 0, 0, 0,
					DRI2_EXCHANGE_COMPLETE,
					req->event_complete, req->event_data);

		page_flip->completes_pending--;
	}

	if (req->flip_issued && req->display_update_idx != SWAP_INVALID_IDX)
		page_flip->flips_pending--;

	page_flip->bufs[req->back_idx].busy = FALSE;

	flip_stats_swap_killed(req);

	/*
	 * req cannot be freed yet since it's being passed around
	 * by the event handlers. Just mark it as dead so the
	 * handlers know to drop it when they see it.
	 */
	if (!req->render_done || req->flip_issued)
		req->dead = true;
	else
		free_swap_req(req);
}

void dri2_kill_swap_reqs(void)
{
	struct dri2_swap_request *req = swap_reqs_tail;

	while (req) {
		swap_reqs_drop_last();
		kill_swap_req(req);
		req = swap_reqs_tail;
	}
}

static void complete_swap_req(struct dri2_swap_request *req,
			      unsigned int tv_sec,
			      unsigned int tv_usec)
{
	DrawablePtr draw;
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;

	assert(!req->complete_done);
	assert(!req->dead);
	assert(req->type == SWAP_FLIP);

	req->complete_done = true;

	page_flip->completes_pending--;

	if (dixLookupDrawable(&draw, req->drawable_id, serverClient, M_ANY,
				DixWriteAccess) != Success)
		return;

	DRI2SwapComplete(req->client, draw, 0, tv_sec, tv_usec,
			DRI2_EXCHANGE_COMPLETE, req->event_complete,
			req->event_data);
}

static void flip_swap_req(struct dri2_swap_request *req);

static void pvr2d_dri2_issue_flip(ScrnInfoPtr screen_info,
		struct dri2_swap_request *req)
{
	DrawablePtr draw;

	if (req->type == SWAP_EXTFB) {
		ExtFBUpdate(req->event_data, &req->update_region);
		return;
	}
	if (req->display_update_idx != SWAP_INVALID_IDX) {
		/* Start the update for the new front buffer */
		fbdev_update_outputs(screen_info, NULL);
	} else {
		RegionRec reg;

		if (dixLookupDrawable(&draw, req->drawable_id, serverClient,
				      M_ANY, DixWriteAccess) != Success)
			return;

		sgx_exa_set_reg(draw, &reg);
		ExtFBDamage(screen_info, &reg);
	}
}

static bool pvr2d_dri2_is_overlay_manual_update(FBDevPtr fbdev,
		unsigned int overlay)
{
	struct omap_output *out = omap_overlay_get_output(fbdev->ovl[overlay]);
	enum omap_output_update update = OMAP_OUTPUT_UPDATE_AUTO;

	if (!out)
		return false;

	omap_output_get_update_mode(out, &update);

	return update == OMAP_OUTPUT_UPDATE_MANUAL;
}

static void pvr2d_dri2_flip_handler(int fd, unsigned overlay,
			unsigned tv_sec, unsigned tv_usec,
			unsigned long user_data)
{
	struct dri2_swap_request *next_flip, *req =
		(struct dri2_swap_request *)user_data;
	struct pvr2d_screen *screen = pvr2d_get_screen();
	struct pvr2d_page_flip *page_flip = &screen->page_flip;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(req->screen);

	/* Wait until all CRTCs have flipped */
	if (--req->num_flips_pending > 0)
		return;

	assert(req->render_done);
	assert(!req->flip_done);
	assert(req->flip_issued);

	if (req->dead) {
		flip_stats_flip_completed_dead();
		free_swap_req(req);
		return;
	}

	assert(swap_reqs_find_next_flip(req) == req);

	req->flip_done = true;

	flip_stats_flip_completed_from_flip_handler();

	assert(req->front_idx == SWAP_INVALID_IDX ||
			page_flip->bufs[req->front_idx].busy);
	assert(req->back_idx == SWAP_INVALID_IDX ||
			page_flip->bufs[req->back_idx].busy);

	if (req->front_idx != SWAP_INVALID_IDX)
		page_flip->bufs[req->front_idx].busy = FALSE;

	if (req->display_update_idx != SWAP_INVALID_IDX) {
		assert(page_flip->flips_pending == 1);
		page_flip->flips_pending--;
	}

	pvr2d_dri2_issue_flip(pScrn, req);


	if (req->back_idx != SWAP_INVALID_IDX) {
		struct dri2_swap_request *complete_req =
			swap_reqs_find_first_pending_flip();
		/*
		 * Now that another buffer is available
		 * complete the last swap request.
		 */
		if (complete_req) {
			assert(complete_req->back_idx == page_flip->front_idx);

			flip_stats_swap_completed_from_flip_handler();

			complete_swap_req(complete_req, tv_sec, tv_usec);
		}
	}

	/*
	 * do this after the above completion phase in
	 * case r == swap_reqs_tail (happens with num_bufs=2)
	 */
	swap_req_dequeue(req);

	next_flip = swap_reqs_find_next_flip(req);
	/*
	 * If the next buffer has already finished
	 * rendering flip it to screen.
	 */
	if (next_flip) {
		flip_stats_flip_issued_from_flip_handler();

		flip_swap_req(next_flip);
	}

	free_swap_req(req);
}

static void pvr2d_dri2_sync_handler(int fd, const void *sync_info,
			unsigned tv_sec, unsigned tv_usec,
			unsigned long user_data)
{
	struct dri2_swap_request *req = (struct dri2_swap_request *)user_data;

	assert(!req->render_done);
	assert(!req->flip_issued);
	assert(!req->flip_done);

	if (req->dead) {
		flip_stats_render_completed_dead();
		free_swap_req(req);
		return;
	}

	req->render_done = true;

	flip_stats_render_completed(req);

	/* issue the swap if we're next in line */
	if (swap_reqs_find_next_flip(req) == req) {
		flip_stats_flip_issued_from_render_handler();

		flip_swap_req(req);
	}
}

static void flip_swap_req(struct dri2_swap_request *req)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();
	struct pvr2d_page_flip *page_flip = &screen->page_flip;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(req->screen);
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	int i;

	assert(swap_reqs_find_next_flip(req) == req);

	assert(req->render_done);
	assert(!req->flip_done);
	assert(!req->flip_issued);

	if (req->back_idx != SWAP_INVALID_IDX)
		fbdev_flip_crtcs(pScrn, req->back_idx);

	req->flip_issued = true;

	req->num_flips_pending = 0;

	if (req->display_update_idx != SWAP_INVALID_IDX) {
		/* Re-use the original event */
		/*
		 * FIXME Requesting the event for all overlays is not the most
		 * efficient way. It works because inactive overlays will get
		 * the event immediately from the kernel and we can't flip only
		 * a part of the screen so all active overlays must be contained
		 * within the screen.
		 */
		assert(!page_flip->flips_pending);
		page_flip->flips_pending++;

		for (i = 0; i < 3; i++) {
			if (!omap_overlay_enabled(fbdev->ovl[i]))
				continue;

			if (pvr2d_dri2_is_overlay_manual_update(fbdev, i)) {
				/*
				 * Fall back to flip event if
				 * update event isn't supported.
				 */
				if (PVR2DUpdateEventReq(screen->context, i, req) == PVR2D_OK ||
				    PVR2DFlipEventReq(screen->context, i, req) == PVR2D_OK)
					req->num_flips_pending++;
			} else if (req->back_idx != SWAP_INVALID_IDX) {
				if (PVR2DFlipEventReq(screen->context, i, req) == PVR2D_OK)
					req->num_flips_pending++;
			}
		}
	}

	if (!req->num_flips_pending) {
		req->num_flips_pending = 1;
		pvr2d_dri2_flip_handler(0, 0, 0, 0, (unsigned long)req);
	}
}

static void pvr2d_dri2_update_front(struct pvr2d_page_flip *page_flip,
		ScrnInfoPtr pScrn,
		unsigned int new_front_idx)
{
	pvr2d_get_screen()->sys_mem_info =
		page_flip->bufs[new_front_idx].mem_info;

	SysMemInfoChanged(pScrn);
}

/* Request for synchronization event once rendering is finished. */
static int pvr2d_dri2_sync_req(ClientPtr client, DrawablePtr draw,
				DRI2BufferPtr front, DRI2BufferPtr back,
				PVR2DMEMINFO *mem_info,
				DRI2SwapEventPtr func, void *data)
{
	struct dri2_swap_request *req;
	struct pvr2d_screen *screen = pvr2d_get_screen();
	struct pvr2d_page_flip *page_flip = &screen->page_flip;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(draw->pScreen);
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	RegionRec reg;

	req = calloc(1, sizeof *req);
	if (!req)
		return FALSE;

	req->type = SWAP_FLIP;
	req->drawable_id = draw->id;
	req->client = client;
	req->screen = draw->pScreen;
	req->event_complete = func;
	req->event_data = data;
	req->front = front;
	req->back = back;
	req->front_idx = page_flip->front_idx;
	req->back_idx = page_flip->back_idx;
	if (fbdev->conf.vsync)
		req->display_update_idx = page_flip->back_idx;
	else
		req->display_update_idx = SWAP_INVALID_IDX;
	RegionNull(&req->update_region);

	assert(front->name == (unsigned)
	       page_flip->bufs[req->front_idx].mem_handle);
	assert(back->name == (unsigned)
	       page_flip->bufs[req->back_idx].mem_handle);

	assert(!page_flip->bufs[req->back_idx].busy);
	page_flip->bufs[req->back_idx].busy = TRUE;
	pvr2d_dri2_update_front(page_flip, pScrn, req->back_idx);

	swap_req_enqueue(req);

	extfb_lock_display_update(pScrn);
	sgx_exa_set_reg(draw, &reg);
	DamageDamageRegion(draw, &reg);
	extfb_unlock_display_update(pScrn);

	flip_stats_swap_requested();

	page_flip->completes_pending++;

	pvr2d_dri2_exchange_bufs(draw, front, back);

	if (page_flip->bufs[page_flip->back_idx].busy) {
		/*
		 * Wait for the next back buffer to become free before this
		 * swap is completed in order to throttle the client
		 * during the next GetBuffers request.
		 */
	} else {
		/*
		 * Next back buffer is free so the swap can be completed
		 * immediately.
		 */
		flip_stats_swap_completed_from_swap_request();

		complete_swap_req(req, 0, 0);
	}

	assert(page_flip->completes_pending < 2);

	if (!fbdev->conf.render_sync ||
	    PVR2DSyncEventReq(screen->context, mem_info, req, PVR2D_WAIT_SYNC_EVENT) != PVR2D_OK) {
		pvr2d_dri2_sync_handler(0, NULL, 0, 0,
				(unsigned long)req);
	}

	return TRUE;
}

static void pvr2d_dri2_no_swap(ClientPtr client, DrawablePtr draw,
				DRI2BufferPtr front, DRI2BufferPtr back,
				DRI2SwapEventPtr func, void *data)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();
	struct pvr2d_page_flip *page_flip = &screen->page_flip;
	struct pvr2d_buf_priv *priv = back->driverPrivate;
	RegionRec reg;

	if (pvr2d_dri2_fs_flip_capable(page_flip, draw))
		flip_stats_blit_while_fullscreen();

	if (page_flip->user_priv == draw) {
		/*
		 * No point in migrating from fullscreen here. Just copy
		 * from the page flip back buffer to the current front buffer.
		 * Migration happens during the next GetBuffers, if necessary.
		 */
		assert(!page_flip->bufs[page_flip->front_idx].reserved);
		assert(page_flip->bufs[page_flip->back_idx].reserved);
		page_flip->bufs[page_flip->back_idx].reserved = FALSE;
	}

	assert(priv->reserved);
	priv->reserved = FALSE;

	sgx_exa_set_reg(draw, &reg);

	pvr2d_dri2_copy_region(draw, &reg, front, back);

	DRI2SwapComplete(client, draw, 0, 0, 0, DRI2_BLIT_COMPLETE,
			func, data);
}

bool pvr2d_dri2_schedule_damage(DrawablePtr draw, RegionPtr region)
{
	struct dri2_swap_request *req;
	struct pvr2d_screen *screen = pvr2d_get_screen();
	struct pvr2d_page_flip *page_flip = &screen->page_flip;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(draw->pScreen);

	/* check if there is update scheduled already */
	req = swap_reqs_find_by_update(page_flip->front_idx);
	if (req) {
		RegionAppend(&req->update_region, region);
		return TRUE;
	}

	req = calloc(1, sizeof *req);
	if (!req)
		return FALSE;

	req->type = SWAP_EXTFB;
	req->drawable_id = draw->id;
	req->screen = draw->pScreen;
	req->event_data = pScrn;
	req->front_idx = SWAP_INVALID_IDX;
	req->back_idx = SWAP_INVALID_IDX;
	req->display_update_idx = page_flip->front_idx;
	RegionNull(&req->update_region);

	swap_req_enqueue(req);

	/* Avoid problem that client could push multiple xrender operations to
	 * block page flip.
	 */
	req->render_done = true;

	req->complete_done = true;

	RegionAppend(&req->update_region, region);

	if (swap_reqs_find_next_flip(req) == req) {
		flip_stats_flip_issued_from_damage_handler();

		flip_swap_req(req);
	}

	return TRUE;
}

static int pvr2d_dri2_schedule_swap(ClientPtr client, DrawablePtr draw,
				DRI2BufferPtr front, DRI2BufferPtr back,
				CARD64 *target_msc, CARD64 divisor,
				CARD64 remainder, DRI2SwapEventPtr func,
				void *data)
{
	FBDevPtr fbdev = FBDEVPTR(xf86ScreenToScrn(draw->pScreen));
	struct pvr2d_screen *screen = pvr2d_get_screen();
	struct pvr2d_page_flip *page_flip = &screen->page_flip;
	struct pvr2d_buf_priv *priv = back->driverPrivate;

	if (!priv->reserved) {
		ErrorF("SwapBuffers without GetBuffers (drawable=0x%lx)\n",
		       draw->id);
		/* FIXME fail or ignore the swap? */
		if (fbdev->conf.poison_swapbuffers)
			poison_buffer(draw, front, 0xff, 0xff, 0x00);
		return FALSE;
	}

	if (!page_flip_possible(page_flip, draw)) {
		pvr2d_dri2_no_swap(client, draw, front, back, func, data);
		return TRUE;
	}

	/*
	 * Consider if the given drawable is using its private buffer even if
	 * its fullscreen flip capable. This is possible if by the time the
	 * drawable became fullscreen capable, some other drawable was still
	 * occupying the flip buffers.
	 */
	if (page_flip->user_priv != draw) {
		assert(!page_flip->user_priv ||
		       !page_flip->bufs[page_flip->back_idx].reserved);

		if (page_flip->user_priv &&
		    pvr2d_dri2_migrate_from_fs(page_flip))
			FatalError("Failed to migrate from fullscreen");

		pvr2d_dri2_migrate_to_fs(draw, front);
		pvr2d_dri2_migrate_to_fs(draw, back);

		page_flip_buf_reserve(page_flip, back);

		page_flip->user_priv = draw;
	}

	assert(priv->reserved);
	priv->reserved = FALSE;

	assert(!page_flip->bufs[page_flip->front_idx].reserved);
	assert(page_flip->bufs[page_flip->back_idx].reserved);
	page_flip->bufs[page_flip->back_idx].reserved = FALSE;

	return pvr2d_dri2_sync_req(client, draw, front, back,
				page_flip->bufs[page_flip->back_idx].mem_info,
				func, data);
}

/*
 * dri2 requires AuthMagic callback to authenticate clients. Because there
 * is no authentication we just provide stub that always returns passed.
 */
static int pvr2d_dri2_auth_magic(int fd, uint32_t magic)
{
	return 0;
}

Bool DRI2_Init(ScreenPtr pScreen)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();
	DRI2InfoRec info = {
		.driverName = "pvr2d",
		.version = DRI2INFOREC_VERSION,
		.CreateBuffer = pvr2d_dri2_create_buf,
		.DestroyBuffer = pvr2d_dri2_destroy_buf,
		.CopyRegion = pvr2d_dri2_copy_region,
		.ScheduleSwap = pvr2d_dri2_schedule_swap,
		.AuthMagic = pvr2d_dri2_auth_magic,
		.ReuseBufferNotify = pvr2d_dri2_reuse_buf,
	};

	if (!xf86LoadSubModule(xf86ScreenToScrn(pScreen), "dri2"))
		return FALSE;

	screen->flip_event_handler = pvr2d_dri2_flip_handler;
	screen->sync_event_handler = pvr2d_dri2_sync_handler;

	swap_reqs_head = swap_reqs_tail = NULL;

	flip_stats_init(pScreen);

	return DRI2ScreenInit(pScreen, &info);
}

void DRI2_Fini(ScreenPtr pScreen)
{
	flip_stats_fini(pScreen);
}
