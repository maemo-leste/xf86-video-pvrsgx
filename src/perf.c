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
#include "perf.h"

struct sgx_perf_counters perf_counters;

static char *alu[] = {
	"GXclear",
	"GXand",
	"GXandReverse",
	"GXcopy",
	"GXandInverted",
	"GXnoop",
	"GXxor",
	"GXor",
	"GXnor",
	"GXequiv",
	"GXinvert",
	"GXorReverse",
	"GXcopyInverted",
	"GXorInverted",
	"GXnand",
	"GXset",
	NULL,
};

static CARD32 perfCountersCallback(OsTimerPtr timer, CARD32 time, pointer arg)
{
	ScrnInfoPtr pScrn = arg;
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	int i, t;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
	           "Memory allocation statistics:\n"
	           "malloc:     %.4f megabytes/%4ld segments\n"
	           "shm:        %.4f megabytes/%4ld segments\n"
	           "total:      %.4f megabytes/%4ld segments (%.4f avg)\n",
	           (float) perf_counters.malloc_bytes / (1024 * 1024),
	           perf_counters.malloc_segments,
	           (float) perf_counters.shm_bytes / (1024 * 1024),
	           perf_counters.shm_segments,
	           (float) (perf_counters.malloc_bytes +
	                    perf_counters.shm_bytes) / (1024 * 1024),
	           perf_counters.malloc_segments + perf_counters.shm_segments,
	           (float) ((perf_counters.malloc_bytes +
	                    perf_counters.shm_bytes) /
	                    (perf_counters.malloc_segments +
	                    perf_counters.shm_segments)) / (1024 * 1024));

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Solid:        %8ld HW        %8ld SW        %8ld ALL\n",
		   perf_counters.hw_solid, perf_counters.sw_solid,
		   perf_counters.hw_solid + perf_counters.sw_solid);
	for (i = GXclear; i <= GXset; i++)
		if (perf_counters.solid_alu[i])
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "        %s: %8ld\n", alu[i],
				   perf_counters.solid_alu[i]);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Copy:         %8ld HW        %8ld SW        %8ld ALL\n",
		   perf_counters.hw_copy, perf_counters.sw_copy,
		   perf_counters.hw_copy + perf_counters.sw_copy);
	for (i = GXclear; i <= GXset; i++)
		if (perf_counters.copy_alu[i])
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "        %s: %8ld\n", alu[i],
				   perf_counters.copy_alu[i]);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Cache:        %8ld FLUSH     %8ld INVAL\n",
		   perf_counters.cache_flush, perf_counters.cache_inval);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Fallback: %8ld GXcopy %8ld bitsPerPixel %8ld isSolid\n",
		   perf_counters.fallback_GXcopy,
		   perf_counters.fallback_bitsPerPixel,
		   perf_counters.fallback_isSolid);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Fallback: Solid: %8ld validateDst %8ld getFormatDst\n",
		   perf_counters.fallback_solid_validateDst,
		   perf_counters.fallback_solid_getFormatDst);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Fallback: Copy: %8ld validateSrc %8ld validateDst %8ld getFormatSrc %8ld getFormatDst\n\n",
		   perf_counters.fallback_copy_validateSrc,
		   perf_counters.fallback_copy_validateDst,
		   perf_counters.fallback_copy_getFormatSrc,
		   perf_counters.fallback_copy_getFormatDst);

	if (xf86IsOptionSet(fbdev->Options, OPTION_PERF_RESET))
		memset(&perf_counters, 0, sizeof(struct sgx_perf_counters));

	xf86GetOptValInteger(fbdev->Options, OPTION_PERF_TIME, &t);
	return t;
}

Bool PVR2D_PerfInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	int t;

	memset(&perf_counters, 0, sizeof(struct sgx_perf_counters));
	if (!xf86GetOptValInteger(fbdev->Options, OPTION_PERF_TIME, &t))
		return FALSE;

	fbdev->perf_timer =
	    TimerSet(fbdev->perf_timer, 0, t, perfCountersCallback, pScrn);

	return TRUE;
}

void PVR2D_PerfFini(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	if (fbdev->perf_timer)
		TimerFree(fbdev->perf_timer);
}
