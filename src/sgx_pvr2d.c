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

#include "fbdev.h"
#include "sgx_dri2.h"
#include "sgx_pvr2d.h"
#include "sgx_pvr2d_alloc.h"
#include "sgx_exa_user.h"
#include <services.h>
#include "pvr_events.h"
#include "perf.h"

#include <exa.h>

struct pvr2d_screen *pvr2d_get_screen(void)
{
	/* These should really be tracked per screen but are global for now */
	static struct pvr2d_screen only_screen;

	return &only_screen;
}

void SysMemInfoChanged(ScrnInfoPtr pScrn)
{
	FBDevPtr fbdev = FBDEVPTR(pScrn);
	struct PVR2DPixmap *ppix = exaGetPixmapDriverPrivate(fbdev->pixmap);
	struct pvr2d_screen *screen = pvr2d_get_screen();

	ppix->pvr2dmem = screen->sys_mem_info;
	if (screen->sys_mem_info)
		ppix->shmaddr = screen->sys_mem_info->pBase;
	else
		ppix->shmaddr = NULL;
}

static int pvr2d_set_frame_buffer(ScrnInfoPtr scrn_info,
				struct pvr2d_screen *screen)
{
	unsigned stride = scrn_info->displayWidth *
			scrn_info->bitsPerPixel >> 3;
	FBDevPtr dev = FBDEVPTR(scrn_info);
	void *bufs[MAX_PAGE_FLIP_BUFFERS];
	int i;

	bufs[0] = dev->fbmem;
	for (i = 1; i < dev->conf.page_flip_bufs; i++)
		bufs[i] = bufs[i - 1] +
			dev->fbmem_len / dev->conf.page_flip_bufs;

	if (pvr2d_page_flip_create(screen->context,
				   dev->fbmem_len / dev->conf.page_flip_bufs,
				   stride, scrn_info->bitsPerPixel,
				   bufs, dev->conf.page_flip_bufs,
				   &screen->page_flip))
		FatalError("unable to create flip buffers\n");

	screen->sys_mem_info =
		screen->page_flip.bufs[screen->page_flip.front_idx].mem_info;

	dev->page_scan_next = screen->page_flip.front_idx;

	return 0;
}

static Bool update_flip_pixmaps(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;
	int i;

	for (i = 0; i < page_flip->num_bufs; i++) {
		if (!sgx_exa_update_pixmap(page_flip->bufs[i].pixmap,
					   pScrn->virtualX, pScrn->virtualY,
					   pScrn->depth, pScrn->bitsPerPixel,
					   pScrn->displayWidth *
					   pScrn->bitsPerPixel >> 3,
					   page_flip->bufs[i].mem_info))
			return FALSE;
	}

	return TRUE;
}

/*
 * PVR2D_PostFBReset
 *
 * Call this function after resetting the framebuffer.
 *
 * Maps the framebuffer.
 * Resets memory mapping in all 'screen' pixmaps to new
 * system memory.
 */
Bool PVR2D_PostFBReset(ScrnInfoPtr scrn_info)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();
	PVR2DMEMINFO *pMemInfo = screen->sys_mem_info;

	if (!screen->sys_mem_info) {
		if (pvr2d_set_frame_buffer(scrn_info, screen))
			return FALSE;
		if (!update_flip_pixmaps(scrn_info->pScreen))
			return FALSE;
	}

	if (pMemInfo != screen->sys_mem_info)
		SysMemInfoChanged(scrn_info);

	return TRUE;
}

/*
 * PVR2D_PreFBReset
 *
 * Call this function before resetting the framebuffer.
 *
 * Assumes that 'pvr2d_page_flip_destroy' makes sure that all blits to the
 * framebuffer are complete before releasing it.
 * Resets memory mapping in all 'screen' pixmaps to NULL.
 */
Bool PVR2D_PreFBReset(ScrnInfoPtr scrn_info)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();

	dri2_kill_swap_reqs();

	pvr2d_page_flip_destroy(screen->context, &screen->page_flip);

	screen->sys_mem_info = NULL;
	SysMemInfoChanged(scrn_info);

	return TRUE;
}

static void pvr2d_wakeup_handler(pointer data, int err, pointer p)
{
	int fd = (int)data;
	fd_set *read_mask = p;
	char buf[1024];
	int len;
	int i;
	const struct pvr_event *e;

	if (err <= 0 || !FD_ISSET(fd, read_mask))
		return;

	len = read(fd, buf, sizeof(buf));

	if (len == 0 || len < sizeof(struct pvr_event))
		return;

	for (i = 0; i < len; i += e->length) {
		struct pvr2d_screen *screen = pvr2d_get_screen();

		e = (const struct pvr_event *)&buf[i];

		if (e->length < sizeof(struct pvr_event)
			|| e->length + i > len)
			break;

		if (e->type == PVR_EVENT_SYNC) {
			const struct pvr_event_sync *sync =
				(const struct pvr_event_sync *)e;

			if (screen->sync_event_handler != 0)
				screen->sync_event_handler(fd, sync->sync_info,
						sync->tv_sec, sync->tv_usec,
						sync->user_data);
		} else if (e->type == PVR_EVENT_FLIP ||
			   e->type == PVR_EVENT_UPDATE) {
			const struct pvr_event_flip *flip =
				(const struct pvr_event_flip *)e;

			if (screen->flip_event_handler != 0)
				screen->flip_event_handler(fd,
						flip->overlay,
						flip->tv_sec, flip->tv_usec,
						flip->user_data);
		}
	}
}

Bool PVR2D_Init(ScrnInfoPtr scrn_info)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();
	PVR2DERROR ePVR2DStatus;
	PVR2DDEVICEINFO *pDevInfo = 0;
	int nDeviceNum;
	int nDevices;
	long lRevMajor = 0;
	long lRevMinor = 0;

	PVR2DGetAPIRev(&lRevMajor, &lRevMinor);

	if (lRevMajor != PVR2D_REV_MAJOR || lRevMinor != PVR2D_REV_MINOR) {
		xf86DrvMsg(scrn_info->scrnIndex, X_WARNING,
			"PVR2D API revision mismatch\n");
	}

	/* Get number of devices */
	nDevices = PVR2DEnumerateDevices(0);

	if ((nDevices < PVR2D_OK) || (nDevices == 0)) {
		ErrorF("PowerVR device not found\n");
		goto err_out;
	}

	/* Allocate memory for devices */
	pDevInfo =
	    (PVR2DDEVICEINFO *) malloc(nDevices * sizeof(PVR2DDEVICEINFO));

	if (!pDevInfo) {
		ErrorF("malloc failed\n");
		goto err_out;
	}

	/* Get the devices */
	ePVR2DStatus = PVR2DEnumerateDevices(pDevInfo);

	if (ePVR2DStatus != PVR2D_OK) {
		ErrorF("PVR2DEnumerateDevices failed\n");
		goto err_out;
	}

	/* Choose the first display device */
	nDeviceNum = pDevInfo[0].ulDevID;

	free(pDevInfo);
	pDevInfo = NULL;

	/* Create the device context */
	if (PVR2DCreateDeviceContext(nDeviceNum, &screen->context, 0) !=
		PVR2D_OK)
		goto err_out;

	if (pvr2d_set_frame_buffer(scrn_info, screen))
		goto destroy_context;

#if SGX_CACHE_SEGMENTS
	InitSharedSegments();
#endif

	screen->fd = PVR2DGetFileHandle(screen->context);
	if (screen->fd < 0)
		goto destroy_page_flip;

	AddGeneralSocket(screen->fd);

	if (!RegisterBlockAndWakeupHandlers((BlockHandlerProcPtr)NoopDDA,
					    pvr2d_wakeup_handler,
					    (pointer)screen->fd))
		goto remove_socket;

	return TRUE;

 remove_socket:
	RemoveGeneralSocket(screen->fd);
 destroy_page_flip:
#if SGX_CACHE_SEGMENTS
	DeInitSharedSegments();
#endif
	pvr2d_page_flip_destroy(screen->context, &screen->page_flip);
 destroy_context:
	PVR2DDestroyDeviceContext(screen->context);
 err_out:
	free(pDevInfo);
	return FALSE;
}

void PVR2D_DeInit(void)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();

	RemoveGeneralSocket(screen->fd);

#if SGX_CACHE_SEGMENTS
	DeInitSharedSegments();
#endif

	pvr2d_page_flip_destroy(screen->context, &screen->page_flip);

	PVR2DDestroyDeviceContext(screen->context);
}

/* returns how much memory PVR2DFlushCache would flush */
int PVR2DGetFlushSize(struct PVR2DPixmap *ppix)
{
	if (ppix->pvr2dmem == pvr2d_get_screen()->sys_mem_info ||
		ppix->shmid == -1 || !ppix->shmaddr || !ppix->shmsize)
		return 0;
	if ((ppix->owner == PVR2D_OWNER_GPU) && (ppix->pvr2dmem)) {
		PVRSRV_CLIENT_MEM_INFO *pMemInfo =
		    (PVRSRV_CLIENT_MEM_INFO *) ppix->pvr2dmem->hPrivateData;
		IMG_UINT32 ui32WriteOpsComplete =
		    pMemInfo->psClientSyncInfo->psSyncData->
		    ui32WriteOpsComplete;
		if (ui32WriteOpsComplete == ppix->ui32WriteOpsComplete)
			return 0;
	}
	if ((ppix->owner == PVR2D_OWNER_CPU) && (!ppix->bCPUWrites)) {
		return 0;
	}
	return ppix->shmsize;
}

void PVR2DFlushCache(struct PVR2DPixmap *ppix)
{
	struct pvr2d_screen *screen = pvr2d_get_screen();
	unsigned int cflush_type;
	unsigned long cflush_virt;
	unsigned int cflush_length;
	Bool bNeedFlush = FALSE;

	if (ppix->pvr2dmem == screen->sys_mem_info || ppix->shmid == -1 ||
		!ppix->shmaddr || !ppix->shmsize)
		return;

	/* check if pixmap was modified
	 * by GPU: ui32WriteOpsComplete changed
	 * by CPU: PrepareAccess was called with EXA_PREPARE_DEST
	 */
	if ((ppix->owner == PVR2D_OWNER_CPU) && (ppix->pvr2dmem)) {
		bNeedFlush = ppix->bCPUWrites;
	} else if ((ppix->owner == PVR2D_OWNER_GPU) && (ppix->pvr2dmem)) {
		PVRSRV_CLIENT_MEM_INFO *pMemInfo =
		    (PVRSRV_CLIENT_MEM_INFO *) ppix->pvr2dmem->hPrivateData;
		IMG_UINT32 ui32WriteOpsComplete =
		    pMemInfo->psClientSyncInfo->psSyncData->
		    ui32WriteOpsComplete;
		bNeedFlush = ui32WriteOpsComplete != ppix->ui32WriteOpsComplete;
		ppix->ui32WriteOpsComplete = ui32WriteOpsComplete;
	}
	//ErrorF("%s: Need flush? %s\n",__func__, (bNeedFlush ? "true" : "false") );

	if (bNeedFlush) {
		ppix->bCPUWrites = FALSE;
		cflush_type =
		    ppix->owner ==
		    PVR2D_OWNER_GPU ? DRM_PVR2D_CFLUSH_FROM_GPU :
		    DRM_PVR2D_CFLUSH_TO_GPU;
		cflush_virt = (uint32_t) ppix->shmaddr;
		cflush_length = ppix->shmsize;

		if (cflush_type == DRM_PVR2D_CFLUSH_FROM_GPU)
			PERF_INCREMENT(cache_inval);
		else
			PERF_INCREMENT(cache_flush);

		if (PVR2D_OK != PVR2DCacheFlushDRI(screen->context, cflush_type,
			cflush_virt, cflush_length))
			xf86DrvMsg(0, X_ERROR,
				"DRM_PVR2D_CFLUSH ioctl failed\n");
	}
}

PVR2DERROR QueryBlitsComplete(struct PVR2DPixmap *ppix, unsigned int wait)
{
	if (ppix->owner == PVR2D_OWNER_CPU)
		return PVR2D_OK;

	return PVR2DQueryBlitsComplete(pvr2d_get_screen()->context,
					ppix->pvr2dmem, wait);
}

/* PVR2DInvalidate
 * Unmap the pixmap from GPU.
 */
void PVR2DInvalidate(struct PVR2DPixmap *ppix)
{
	if ((ppix->shmid != -1) && (ppix->pvr2dmem)) {
		DBG("%s: size %u\n", __func__, ppix->shmsize);
		PVR2DMemFree(pvr2d_get_screen()->context, ppix->pvr2dmem);
		ppix->pvr2dmem = NULL;
	}
}

/*
 * Transfer ownership of a pixmap to GPU
 * It makes sure the cache is flushed.
 * Call PVR2DValidate before calling this
 */
void PVR2DPixmapOwnership_GPU(struct PVR2DPixmap *ppix)
{
	if (!ppix) {
		DBG("%s(%p) => FALSE\n", __func__, ppix);
		return;
	}

	if (ppix->owner != PVR2D_OWNER_GPU) {
		PVR2DFlushCache(ppix);
		ppix->owner = PVR2D_OWNER_GPU;
	}
}

/*
 * Transfer ownership of a pixmap to CPU
 * It makes sure the cache is flushed.
 */
Bool PVR2DPixmapOwnership_CPU(struct PVR2DPixmap *ppix)
{
	PVR2DCONTEXTHANDLE context = pvr2d_get_screen()->context;

	if (!ppix) {
		DBG("%s(%p) => FALSE\n", __func__, ppix);
		return FALSE;
	}

	if (ppix->pvr2dmem) {
		if (PVR2DQueryBlitsComplete(context, ppix->pvr2dmem, 0) !=
			PVR2D_OK) {
			DBG("%s: Pending blits!\n", __func__);
			PVR2DQueryBlitsComplete(context, ppix->pvr2dmem, 1);
		}
	}

	if (ppix->owner != PVR2D_OWNER_CPU) {
		PVR2DFlushCache(ppix);
		ppix->owner = PVR2D_OWNER_CPU;
	}

	//DBG("%s(%p, %d) => TRUE (%p)\n", __func__, pPix, index, pPix->devPrivate.ptr);

	return TRUE;
}

/* There is a limit to the surface size that SGX supports. */
Bool PVR2DCheckSizeLimits(int width, int height)
{
	return width <= EURASIA_RENDERSIZE_MAXX &&
		height <= EURASIA_RENDERSIZE_MAXY;
}

/* PVR2DValidate
 * Validate the pixmap for use in SGX
 * if PVR2DMemWrap fails and cleanup is set, then try to release
 * all memory mapped to GPU and try PVR2DMemWrap again */
Bool PVR2DValidate(PixmapPtr pPixmap, struct PVR2DPixmap * ppix, Bool cleanup)
{
	PVR2DCONTEXTHANDLE context = pvr2d_get_screen()->context;
	unsigned int num_pages;
	unsigned int contiguous = PVR2D_WRAPFLAG_NONCONTIGUOUS;

	if (!ppix) {
		DBG("%s: !pPix: FALSE\n", __func__);
		return FALSE;
	}

	if (ppix->pvr2dmem) {
		DBG("%s: pPix->pvr2dmem: TRUE\n", __func__);
		return TRUE;
	}

	if (!ppix->shmsize || ppix->shmid == -1 || !ppix->shmaddr) {
		DBG("%s: !SHM: FALSE\n", __func__);
		return FALSE;
	}

	assert(PVR2DCheckSizeLimits(pPixmap->drawable.width,
				    pPixmap->drawable.height));

	/* shmsize should already be page-aligned, but just to be safe... */
	num_pages = ALIGN(ppix->shmsize, getpagesize()) / getpagesize();
	if (num_pages == 1)
		contiguous = PVR2D_WRAPFLAG_CONTIGUOUS;

	if (PVR2DMemWrap(context, ppix->shmaddr, contiguous, ppix->shmsize,
				NULL, &ppix->pvr2dmem) == PVR2D_OK)
		return TRUE;

	/* Try again after freeing PVR2D memory asynchronously */
	PVR2DDelayedMemDestroy(FALSE);

	if (PVR2DMemWrap(context, ppix->shmaddr, contiguous, ppix->shmsize,
				NULL, &ppix->pvr2dmem) == PVR2D_OK)
		return TRUE;

	/* Last resort, try again after freeing PVR2D memory synchronously */
	PVR2DDelayedMemDestroy(TRUE);

	if (PVR2DMemWrap(context, ppix->shmaddr, contiguous, ppix->shmsize,
		NULL, &ppix->pvr2dmem) == PVR2D_OK)
		return TRUE;

	if (cleanup) {
#if SGX_CACHE_SEGMENTS
		CleanupSharedSegments();
#endif
		PVR2DUnmapAllPixmaps();

		if (PVR2DMemWrap(context, ppix->shmaddr, contiguous,
			ppix->shmsize, NULL, &ppix->pvr2dmem) != PVR2D_OK) {
			ErrorF("%s: Memory wrapping failed\n", __func__);
			ppix->pvr2dmem = NULL;
			return FALSE;
		}
	}

	return TRUE;
}

Bool PVR2DCreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;
	int i;

	for (i = 0; i < page_flip->num_bufs; i++) {
		page_flip->bufs[i].pixmap =
			sgx_exa_create_pixmap(pScreen,
					      pScrn->virtualX, pScrn->virtualY,
					      pScrn->depth, pScrn->bitsPerPixel,
					      pScrn->displayWidth *
					      pScrn->bitsPerPixel >> 3,
					      page_flip->bufs[i].mem_info);
		if (!page_flip->bufs[i].pixmap)
			goto error;
	}

	return TRUE;
 error:
	while (--i >= 0) {
		(*pScreen->DestroyPixmap)(page_flip->bufs[i].pixmap);
		page_flip->bufs[i].pixmap = NULL;
	}

	return FALSE;
}

void PVR2DCloseScreen(ScreenPtr pScreen)
{
	struct pvr2d_page_flip *page_flip = &pvr2d_get_screen()->page_flip;
	int i;

	for (i = 0; i < page_flip->num_bufs; i++) {
		(*pScreen->DestroyPixmap)(page_flip->bufs[i].pixmap);
		page_flip->bufs[i].pixmap = NULL;
	}
}
