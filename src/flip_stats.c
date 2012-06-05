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

#include "fbdev.h"
#include "flip_stats.h"
#include "sgx_dri2.h"

struct flip_stats flip_stats;

void flip_stats_render_completed(struct dri2_swap_request *req)
{
	struct dri2_swap_request *i;

	assert(req->render_done);

	/*
	 * Check if there are unfinished renders
	 * ahead of this request in the queue.
	 */
	for (i = swap_reqs_head; i; i = i->next) {
		if (!i->render_done)
			continue;

		if (i == req)
			flip_stats.renders_completed_in_order++;
		else
			flip_stats.renders_completed_out_of_order++;
		break;
	}
}

void flip_stats_swap_killed(struct dri2_swap_request *req)
{
	if (!req->render_done)
		flip_stats.swaps_killed_before_render_done++;
	else if (!req->flip_issued)
		flip_stats.swaps_killed_before_flip_issued++;
	else if (!req->flip_done)
		flip_stats.swaps_killed_before_flip_done++;
}

static CARD32 statsCallback(OsTimerPtr timer, CARD32 time, pointer arg)
{
	ScrnInfoPtr pScrn = arg;
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	struct dri2_swap_request *req;
	int i, t;
	Bool reset = xf86IsOptionSet(fbdev->Options, OPTION_FLIP_STATS_RESET);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "\n"
		   "swaps requested                  : %u\n"
		   "swaps completed                  : %u\n"
		   "swaps completed dead             : %u\n"
		   "swaps completed from swap request: %u\n"
		   "swaps completed from flip handler: %u\n"
		   "flips completed                  : %u\n"
		   "flips completed dead             : %u\n"
		   "flips completed from flip handler: %u\n"
		   "flips issued                     : %u\n"
		   "flips issued from render handler : %u\n"
		   "flips issued from flip handler   : %u\n"
		   "flips issued from damage handler : %u\n"
		   "renders completed                : %u\n"
		   "renders completed dead           : %u\n"
		   "renders completed in order       : %u\n"
		   "renders completed out of order   : %u\n"
		   "swaps killed                     : %u\n"
		   "swaps killed before render done  : %u\n"
		   "swaps killed before flip issued  : %u\n"
		   "swaps killed before flip done    : %u\n"
		   "blits while fullscreen           : %u\n",
		   flip_stats.swaps_requested,
		   flip_stats.swaps_completed_dead +
		   flip_stats.swaps_completed_from_swap_request +
		   flip_stats.swaps_completed_from_flip_handler,
		   flip_stats.swaps_completed_dead,
		   flip_stats.swaps_completed_from_swap_request,
		   flip_stats.swaps_completed_from_flip_handler,
		   flip_stats.flips_completed_dead +
		   flip_stats.flips_completed_from_flip_handler,
		   flip_stats.flips_completed_dead,
		   flip_stats.flips_completed_from_flip_handler,
		   flip_stats.flips_issued_from_render_handler +
		   flip_stats.flips_issued_from_flip_handler +
		   flip_stats.flips_issued_from_damage_handler,
		   flip_stats.flips_issued_from_render_handler,
		   flip_stats.flips_issued_from_flip_handler,
		   flip_stats.flips_issued_from_damage_handler,
		   flip_stats.renders_completed_dead +
		   flip_stats.renders_completed_in_order +
		   flip_stats.renders_completed_out_of_order,
		   flip_stats.renders_completed_dead,
		   flip_stats.renders_completed_in_order,
		   flip_stats.renders_completed_out_of_order,
		   flip_stats.swaps_killed_before_render_done +
		   flip_stats.swaps_killed_before_flip_issued +
		   flip_stats.swaps_killed_before_flip_done,
		   flip_stats.swaps_killed_before_render_done,
		   flip_stats.swaps_killed_before_flip_issued,
		   flip_stats.swaps_killed_before_flip_done,
		   flip_stats.blits_while_fullscreen);

	if (reset)
		memset(&flip_stats, 0, sizeof flip_stats);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "swap requests still in the queue:\n");

	i = 0;
	for (req = swap_reqs_head; req; req = req->next) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "%d: render_done=%d, flip_issued=%d, "
			   "flip_done=%d, complete_done=%d\n",
			   i++, req->render_done, req->flip_done,
			   req->flip_issued, req->complete_done);

		/* Fix up counters so that totals keep consistent */
		if (reset) {
			if (req->type == SWAP_FLIP)
				flip_stats.swaps_requested++;
			if (req->type == SWAP_FLIP && req->complete_done)
				flip_stats_swap_completed_from_swap_request();
			if (req->type == SWAP_FLIP && req->render_done)
				flip_stats_render_completed(req);
			if (req->type == SWAP_FLIP && req->flip_issued)
				flip_stats_flip_issued_from_render_handler();
			if (req->type == SWAP_EXTFB && req->flip_issued)
				flip_stats_flip_issued_from_damage_handler();
			if (req->dead)
				flip_stats_swap_killed(req);
		}
	}

	xf86GetOptValInteger(fbdev->Options, OPTION_FLIP_STATS_TIME, &t);

	return t;
}

Bool flip_stats_init(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	int t;

	memset(&flip_stats, 0, sizeof flip_stats);

	if (!xf86GetOptValInteger(fbdev->Options, OPTION_FLIP_STATS_TIME, &t))
		return FALSE;

	fbdev->flip_stats_timer =
	    TimerSet(fbdev->flip_stats_timer, 0, t, statsCallback, pScrn);

	return TRUE;
}

void flip_stats_fini(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	FBDevPtr fbdev = FBDEVPTR(pScrn);

	if (fbdev->flip_stats_timer)
		TimerFree(fbdev->flip_stats_timer);
}
