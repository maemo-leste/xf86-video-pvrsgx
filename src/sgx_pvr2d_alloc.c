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
#include "sgx_pvr2d.h"
#include "sgx_pvr2d_alloc.h"
#include "sgx_dri2.h"
#include "perf.h"

#include <exa.h>
#include "x-hash.h"

/* PVR2D memory can only be freed once all PVR2D operations using it have
 * completed. In order to avoid waiting for this synchronously, defer freeing
 * of PVR2D memory with outstanding operations until an appropriate time.
 */
static struct PVR2DMemDestroy {
	struct PVR2DPixmap pix;
	struct PVR2DMemDestroy *next;
} *DelayedPVR2DMemDestroy;

unsigned int page_size;


static void doDestroyMemory(struct PVR2DPixmap *ppix)
{
	CALLTRACE("%s: Start\n", __func__);

#if SGX_CACHE_SEGMENTS
	if (AddToCache (ppix))
		return;
#endif

	if (ppix->pvr2dmem) {
		PVR2DMemFree(pvr2d_get_screen()->context, ppix->pvr2dmem);
		ppix->pvr2dmem = NULL;
	}

	if (ppix->shmid != -1) {
		shmdt(ppix->shmaddr);
		ppix->shmaddr = NULL;
		ppix->shmid = -1;
		PERF_DECREMENT2(shm_bytes, ppix->shmsize);
		PERF_DECREMENT(shm_segments);
		ppix->shmsize = 0;
	}

	if (ppix->mallocaddr) {
		free(ppix->mallocaddr);
		ppix->mallocaddr = NULL;
		PERF_DECREMENT2(malloc_bytes, ppix->mallocsize);
		PERF_DECREMENT(malloc_segments);
		ppix->mallocsize = 0;
	}
}

Bool PVR2DDelayedMemDestroy(Bool wait)
{
	while (DelayedPVR2DMemDestroy) {
		struct PVR2DMemDestroy *destroy = DelayedPVR2DMemDestroy;
		Bool complete =
		          QueryBlitsComplete(&destroy->pix, wait) == PVR2D_OK;

		if (complete || wait) {
		/* This should never happen, but in case it does... */
			if (!complete)
				ErrorF ("Freeing PVR2D memory %p despite incomplete blits!"
				        " SGX may lock up...\n", destroy->pix.pvr2dmem);

			doDestroyMemory(&destroy->pix);
			DelayedPVR2DMemDestroy = destroy->next;
			free(destroy);
		} else
			return TRUE;
	}
	return FALSE;
}

static void (*SavedBlockHandler) (BLOCKHANDLER_ARGS_DECL);

static void PVR2DBlockHandler(BLOCKHANDLER_ARGS_DECL)
{
	SCREEN_PTR(arg);

	pScreen->BlockHandler = SavedBlockHandler;
	(*pScreen->BlockHandler) (BLOCKHANDLER_ARGS);

	if (PVR2DDelayedMemDestroy(FALSE)) {
		/* More work -> keep block handler registered */
		SavedBlockHandler = pScreen->BlockHandler;
		pScreen->BlockHandler = PVR2DBlockHandler;
	} else {
		/* Nothing to do any more -> remove the block handler */
		SavedBlockHandler = NULL;
	}

}

void DestroyPVR2DMemory(ScreenPtr pScreen, struct PVR2DPixmap *ppix)
{
	struct PVR2DMemDestroy *destroy;

	/*
	 * If pixmap doesn't have size it is flip or root pixmap with
	 * specialized storage. They shouldn't be freed here.
	 */
	if (ppix->mallocsize == 0 && ppix->shmsize == 0)
		return;

	PVR2DDelayedMemDestroy(FALSE);

	/* Can we free PVR2D memory right away? */
	if (!ppix->pvr2dmem || QueryBlitsComplete(ppix, 0) == PVR2D_OK) {
		doDestroyMemory(ppix);
		return;
	}

	/* No, schedule for delayed freeing */
	destroy = malloc(sizeof(*destroy));

	destroy->pix = *ppix;
	destroy->next = DelayedPVR2DMemDestroy;
	DelayedPVR2DMemDestroy = destroy;

	/* the delayed destroy mechanism keeps it's own copy of these. */
	ppix->pvr2dmem = NULL;
	ppix->shmid = -1;
	ppix->shmaddr = NULL;
	ppix->shmsize = 0;
	ppix->mallocaddr = NULL;
	ppix->mallocsize = 0;

	/* Register the block handler to free the memory */
	if (!SavedBlockHandler) {
		SavedBlockHandler = pScreen->BlockHandler;
		pScreen->BlockHandler = PVR2DBlockHandler;
	}
}

static Bool PVR2DAllocSHM(struct PVR2DPixmap *ppix)
{
	CALLTRACE("%s: Start\n", __func__);

	assert(!ppix->pvr2dmem);
	assert(ppix->shmid < 0);
	assert(!ppix->shmaddr);
	assert(!ppix->mallocaddr);
	assert(!ppix->mallocsize);

	if (!ppix->shmsize)
		return TRUE;

	assert(ppix->shmsize == ALIGN(ppix->shmsize, page_size));

#if SGX_CACHE_SEGMENTS
	if (GetFromCache (ppix))
	    if (ppix->shmaddr)
			return TRUE;
#endif

	ppix->shmid = shmget(IPC_PRIVATE, ppix->shmsize, IPC_CREAT | 0666);

	if (ppix->shmid == -1) {
		perror("shmget failed");
		return FALSE;
	}

	/* lock the SHM segment. This prevents swapping out the memory.
	 * Cache flush/invalidate could cause unhandled page fault
	 * if shared memory was swapped out
	 */
	if (0 != shmctl(ppix->shmid, SHM_LOCK, 0))
		ErrorF("shmctl(SHM_LOCK) failed\n");

	ppix->shmaddr = shmat(ppix->shmid, NULL, 0);

	/*
	 * Mark the segment as destroyed immediately. POSIX doesn't allow
	 * shmat() after this, but Linux does. This is much nicer since
	 * the segment can't be leaked if the X server crashes.
	 */
	shmctl(ppix->shmid, IPC_RMID, NULL);

	if (!ppix->shmaddr) {
		perror("shmat failed");
		ppix->shmid = -1;
		return FALSE;
	}

	/* Allocating memory makes it CPU owned and dirty */
	ppix->owner = PVR2D_OWNER_CPU;
	ppix->bCPUWrites = TRUE;

	PERF_INCREMENT2(shm_bytes, ppix->shmsize);
	PERF_INCREMENT(shm_segments);

	return TRUE;
}

static Bool PVR2DAllocNormal(struct PVR2DPixmap *ppix)
{
	assert(!ppix->pvr2dmem);
	assert(ppix->shmid < 0);
	assert(!ppix->shmaddr);
	assert(!ppix->shmsize);
	assert(!ppix->mallocaddr);

	if (!ppix->mallocsize)
		return TRUE;

#if SGX_CACHE_SEGMENTS
	if (GetFromCache (ppix))
		return TRUE;
#endif

	ppix->mallocaddr = calloc(1, ppix->mallocsize);
	if (!ppix->mallocaddr)
		return FALSE;

	/* Allocating memory makes it CPU owned and dirty */
	ppix->owner = PVR2D_OWNER_CPU;
	ppix->bCPUWrites = TRUE;

	return TRUE;
}

Bool PVR2DAllocatePixmapMem(PixmapPtr pPixmap, struct PVR2DPixmap * ppix,
			   int width, int height, int pitch, pointer pPixData)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();
	ScreenPtr pScreen = pPixmap->drawable.pScreen;
	struct PVR2DPixmap newpix = {
		.shmid = -1,
	};

	assert(!pPixData);

	if (!page_size)
		page_size = getpagesize();

	/* Using PVR2D flipping support is special, there the pixel memory can
	 * come from system surfaces which are not for individual applications
	 * to allocate or dispose. Here one simply does not touch the details,
	 * but leaves the pixmap for later context specific (such as DRI2)
	 * handling.
	 */
	if (pPixmap->usage_hint == SGX_EXA_CREATE_PIXMAP_FLIP) {
		assert(ppix->shmid < 0);
		assert(!ppix->shmsize);
		assert(!ppix->mallocaddr);
		assert(!ppix->mallocsize);

		assert(PVR2DCheckSizeLimits(width, height));

		return TRUE;
	}

	/*
	 * Handle the screen pixmap after the flip pixmaps as the
	 * screen pixmap actually points to the same pvr2dmem as
	 * one of the flip pixmaps. So one of the flip pixmaps would
	 * be misidentified as the screen pixmaps otherwise.
	 */
	if (ppix->pvr2dmem == screen->sys_mem_info) {
		assert(ppix->shmid < 0);
		assert(!ppix->shmsize);
		assert(!ppix->mallocaddr);
		assert(!ppix->mallocsize);

		assert(ppix->shmaddr == screen->sys_mem_info->pBase);

		assert(PVR2DCheckSizeLimits(width, height));

#ifdef SGX_CACHE_SEGMENTS
		SetWindowSizeSharedSegments(height * pitch);
#endif
		return TRUE;
	}

	if (pPixmap->usage_hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP &&
	    PVR2DCheckSizeLimits(width, height)) {
		newpix.shmsize = ALIGN(pitch * height, page_size);

		if (!PVR2DAllocSHM(&newpix))
			return FALSE;
	} else {
		newpix.mallocsize = pitch * height;

		if (!PVR2DAllocNormal(&newpix))
			return FALSE;
	}

	DestroyPVR2DMemory(pScreen, ppix);
	*ppix = newpix;

	return TRUE;
}

Bool pvr2d_dri2_migrate_pixmap(PixmapPtr pPixmap, struct PVR2DPixmap *pix)
{
	if (!page_size)
		page_size = getpagesize();

	if (pix->mallocaddr &&
	    PVR2DCheckSizeLimits(pPixmap->drawable.width,
				 pPixmap->drawable.height)) {
		struct PVR2DPixmap newpix = {
			.shmid = -1,
			.shmsize = ALIGN(pix->mallocsize, page_size),
		};

		assert(!pix->pvr2dmem);

		assert(pix->shmid < 0);
		assert(!pix->shmaddr);
		assert(!pix->shmsize);

		assert(pix->mallocaddr);
		assert(pix->mallocsize);

		if (!PVR2DAllocSHM(&newpix))
			return FALSE;

		DebugF("%s: memcpy %d bytes from malloc (%p) to SHM (%p)\n",
		       __func__, pix->mallocsize, pix->mallocaddr, newpix.shmaddr);

		PVR2DPixmapOwnership_CPU(&newpix);
		memcpy(newpix.shmaddr, pix->mallocaddr, pix->mallocsize);

		DestroyPVR2DMemory(pPixmap->drawable.pScreen, pix);
		*pix = newpix;
	}

	if (!PVR2DValidate(pPixmap, pix, TRUE)) {
		ErrorF("%s: !PVR2DValidate()\n", __func__);
		return FALSE;
	}

	PVR2DPixmapOwnership_GPU(pix);

	return TRUE;
}
