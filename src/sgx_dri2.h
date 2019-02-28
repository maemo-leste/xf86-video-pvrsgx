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

#ifndef SGX_DRI2_H

#define SGX_DRI2_H 1

#include <stdbool.h>

#include "dri2.h"

struct dri2_swap_request {
	enum {
		SWAP_FLIP,
		SWAP_EXTFB,
	} type;
	XID drawable_id;
	ClientPtr client;
	ScreenPtr screen;

	/* for swaps & flips only */
	DRI2SwapEventPtr event_complete;
	void *event_data;
	DRI2BufferPtr front;
	DRI2BufferPtr back;

	int num_flips_pending;

	unsigned int front_idx;
	unsigned int back_idx;
	unsigned int display_update_idx;

	/* track the request's progress */
	bool render_done;
	bool flip_done;
	bool complete_done;
	bool flip_issued;
	bool dead;

	/* Used to store extfb damage */
	RegionRec update_region;

	/* head -> ... -> tail -> NULL */
	struct dri2_swap_request *next;
};

/*
 * queue of in flight swap requests.
 * head == tail == NULL when empty.
 */
extern struct dri2_swap_request *swap_reqs_head;
extern struct dri2_swap_request *swap_reqs_tail;

static const unsigned int SWAP_INVALID_IDX = (unsigned int)-1;

enum {
	DRI2_SWAP_CONTROL_FLIP,
	DRI2_SWAP_CONTROL_BLIT,
};

extern Bool DRI2_Init(ScreenPtr pScreen);
extern void DRI2_Fini(ScreenPtr pScreen);

void dri2_kill_swap_reqs(void);

extern bool pvr2d_dri2_schedule_damage(DrawablePtr draw, RegionPtr region);

#endif /* SGX_DRI2_H */
