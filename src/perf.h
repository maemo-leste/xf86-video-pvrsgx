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

#ifndef PERF_H
#define PERF_H

#include "fbdev.h"

#ifdef PERF
#define PERF_INCREMENT(x)	(perf_counters.x++)
#define PERF_DECREMENT2(x, y)	(perf_counters.x -= (y))
#define PERF_DECREMENT(x)	(perf_counters.x--)
#define PERF_INCREMENT2(x, y)	(perf_counters.x += (y))
#else
#define PERF_INCREMENT(x)	do {} while(0)
#define PERF_DECREMENT2(x, y)	do {} while(0)
#define PERF_DECREMENT(x)	do {} while(0)
#define PERF_INCREMENT2(x, y)	do {} while(0)
#endif

struct sgx_perf_counters {
	unsigned long hw_solid;	/* hardware solid fill operation */
	unsigned long sw_solid;	/* software solid fill operation */
	unsigned long hw_copy;	/* hardware copy operation */
	unsigned long sw_copy;	/* software copy operation */
	unsigned long cache_flush;	/* cache flush operation */
	unsigned long cache_inval;	/* cache invalidate operation */
	/* solid fill and copy ALU operation counters */
	unsigned long solid_alu[GXset + 1];
	unsigned long copy_alu[GXset + 1];
	/* fallback reason counters */
	unsigned long fallback_GXcopy;	/* operation was not GXcopy */
	unsigned long fallback_bitsPerPixel;	/* less than 8 bpp */
	unsigned long fallback_isSolid;	/* plane mask was not solid */

	unsigned long fallback_solid_validateDst;
	unsigned long fallback_solid_getFormatDst;

	unsigned long fallback_copy_validateSrc;
	unsigned long fallback_copy_getFormatSrc;
	unsigned long fallback_copy_validateDst;
	unsigned long fallback_copy_getFormatDst;

	/* system resource counters */
	unsigned long malloc_bytes;
	unsigned long malloc_segments;
	unsigned long shm_bytes;
	unsigned long shm_segments;
};

extern struct sgx_perf_counters perf_counters;

#ifdef PERF
Bool PVR2D_PerfInit(ScreenPtr pScreen);
void PVR2D_PerfFini(ScreenPtr pScreen);
#else
static inline Bool PVR2D_PerfInit(ScreenPtr pScreen) { return FALSE; }
static inline void PVR2D_PerfFini(ScreenPtr pScreen) {}
#endif

#endif
