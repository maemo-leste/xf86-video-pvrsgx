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

#ifndef SGX_PVR2D_H
#define SGX_PVR2D_H

#include <xorg-server.h>
#include <xf86.h>

#define PVR2D_EXT_BLIT 1
#include <pvr2d.h>
#include <sgxfeaturedefs.h>
#include <sgxdefs.h>

#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>

#include "sgx_exa.h"
#include "sgx_cache.h"
#include "sgx_pvr2d_flip.h"

struct PVR2DPixmap {
	PVR2DMEMINFO *pvr2dmem;
	enum {
		PVR2D_OWNER_CPU,
		PVR2D_OWNER_GPU
	} owner;

	/* how many operations GPU has completed on the pixmap */
	IMG_UINT32 ui32WriteOpsComplete;

	/* has PreparedAccess been called with EXA_PREPARE_DEST */
	Bool bCPUWrites;

	/* SHM backed pixmap */
	int shmid;
	int shmsize;
	void *shmaddr;

	/* malloc() backed pixmap */
	int mallocsize;
	void *mallocaddr;
};

enum drm_pvr2d_cflush_type {
	DRM_PVR2D_CFLUSH_FROM_GPU = 1,
	DRM_PVR2D_CFLUSH_TO_GPU = 2
};

struct pvr2d_screen {
	PVR2DCONTEXTHANDLE context;
	PVR2DMEMINFO *sys_mem_info;
	struct pvr2d_page_flip page_flip;

	void (*sync_event_handler)(int fd, const void *sync_info,
			unsigned tv_sec, unsigned tv_usec,
			unsigned long user_data);
	void (*flip_event_handler)(int fd, unsigned overlay,
			unsigned tv_sec, unsigned tv_usec,
			unsigned long user_data);
	int fd;
};

struct pvr2d_screen *pvr2d_get_screen(void);

Bool PVR2D_Init(ScrnInfoPtr scrn_info);
void PVR2D_DeInit(void);
void SysMemInfoChanged(ScrnInfoPtr pScrn);
Bool PVR2D_PostFBReset(ScrnInfoPtr scrn_info);
Bool PVR2D_PreFBReset(ScrnInfoPtr scrn_info);
int PVR2DGetFlushSize(struct PVR2DPixmap *ppix);
void PVR2DFlushCache(struct PVR2DPixmap *ppix);
PVR2DERROR QueryBlitsComplete(struct PVR2DPixmap *ppix, unsigned int wait);
void PVR2DInvalidate(struct PVR2DPixmap *ppix);
void PVR2DPixmapOwnership_GPU(struct PVR2DPixmap *ppix);
Bool PVR2DPixmapOwnership_CPU(struct PVR2DPixmap *ppix);
Bool PVR2DValidate(PixmapPtr pPixmap, struct PVR2DPixmap *ppix, Bool cleanup);
Bool PVR2DCreateScreenResources(ScreenPtr pScreen);
void PVR2DCloseScreen(ScreenPtr pScreen);
Bool PVR2DCheckSizeLimits(int width, int height);

#endif /* SGX_PVR2D_H */
