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

#ifndef FLIP_STATS_H
#define FLIP_STATS_H

#include "fbdev.h"

struct dri2_swap_request;

#if FLIP_STATS

struct flip_stats {
	unsigned int swaps_requested;
	unsigned int swaps_completed_dead;
	unsigned int swaps_completed_from_swap_request;
	unsigned int swaps_completed_from_flip_handler;
	unsigned int flips_completed_dead;
	unsigned int flips_completed_from_flip_handler;
	unsigned int flips_issued_from_render_handler;
	unsigned int flips_issued_from_flip_handler;
	unsigned int flips_issued_from_damage_handler;
	unsigned int renders_completed_dead;
	unsigned int renders_completed_in_order;
	unsigned int renders_completed_out_of_order;
	unsigned int swaps_killed_before_render_done;
	unsigned int swaps_killed_before_flip_issued;
	unsigned int swaps_killed_before_flip_done;
	unsigned int blits_while_fullscreen;
};

extern struct flip_stats flip_stats;

static inline void flip_stats_swap_requested(void)
{
	flip_stats.swaps_requested++;
}

static inline void flip_stats_flip_completed_dead(void)
{
	flip_stats.flips_completed_dead++;
}

static inline void flip_stats_flip_completed_from_flip_handler(void)
{
	flip_stats.flips_completed_from_flip_handler++;
}

static inline void flip_stats_flip_issued_from_render_handler(void)
{
	flip_stats.flips_issued_from_render_handler++;
}

static inline void flip_stats_flip_issued_from_flip_handler(void)
{
	flip_stats.flips_issued_from_flip_handler++;
}

static inline void flip_stats_flip_issued_from_damage_handler(void)
{
	flip_stats.flips_issued_from_damage_handler++;
}

static inline void flip_stats_swap_completed_dead(void)
{
	flip_stats.swaps_completed_dead++;
}

static inline void flip_stats_swap_completed_from_swap_request(void)
{
	flip_stats.swaps_completed_from_swap_request++;
}

static inline void flip_stats_swap_completed_from_flip_handler(void)
{
	flip_stats.swaps_completed_from_flip_handler++;
}

static inline void flip_stats_render_completed_dead(void)
{
	flip_stats.renders_completed_dead++;
}

void flip_stats_render_completed(struct dri2_swap_request *req);

void flip_stats_swap_killed(struct dri2_swap_request *req);

static inline void flip_stats_blit_while_fullscreen(void)
{
	flip_stats.blits_while_fullscreen++;
}

Bool flip_stats_init(ScreenPtr pScreen);
void flip_stats_fini(ScreenPtr pScreen);

#else /* FLIP_STATS */

static inline void flip_stats_swap_requested(void) {}
static inline void flip_stats_flip_completed_dead(void) {}
static inline void flip_stats_flip_completed_from_flip_handler(void) {}
static inline void flip_stats_flip_issued_from_render_handler(void) {}
static inline void flip_stats_flip_issued_from_flip_handler(void) {}
static inline void flip_stats_flip_issued_from_damage_handler(void) {}
static inline void flip_stats_swap_completed_dead(void) {}
static inline void flip_stats_swap_completed_from_swap_request(void) {}
static inline void flip_stats_swap_completed_from_flip_handler(void) {}
static inline void flip_stats_render_completed_dead(void) {}
static inline void flip_stats_render_completed(struct dri2_swap_request *req) {}
static inline void flip_stats_swap_killed(struct dri2_swap_request *req) {}
static inline void flip_stats_blit_while_fullscreen(void) {}
static inline Bool flip_stats_init(ScreenPtr pScreen) { return FALSE; }
static inline void flip_stats_fini(ScreenPtr pScreen) {}


#endif /* FLIP_STATS */

#endif
