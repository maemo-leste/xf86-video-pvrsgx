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
#include "sgx_pvr2d.h"
#include "services.h"
#include "sgx_pvr2d_alloc.h"

typedef struct _segments {
	int count;		//current number of elements on the list;
	int maxsize;		//maximum number of elements on the list
	struct PVR2DPixmap *table;
} cache_segment;

/*
 * we keep a pool of SHM segments for a few, small and frequently used segment
 * sizes
 */
#define NUM_CACHE_SEGS	2
static cache_segment segments[NUM_CACHE_SEGS];
static int default_window_size;

static unsigned CacheSegmentGetId(int size)
{

	if (size == default_window_size) {
		return 0;
	} else if (size == 2*default_window_size) {
		return 1;
	}
	return (unsigned)-1;
}

static int InitCacheSegment(cache_segment * seg, int size)
{
	CALLTRACE("%s:Init segment %p\n", __func__, seg);
	seg->table = 0;
	seg->table = malloc(size * sizeof(seg->table[0]));
	if (!seg->table)
		return 0;
	seg->count = 0;
	seg->maxsize = size;
	return 1;
}

static void CleanupCacheSegment(cache_segment * seg)
{
	int i;
	for (i = 0; i < seg->count; i++) {
		if (seg->table[i].shmaddr) {
			shmdt(seg->table[i].shmaddr);
			shmctl(seg->table[i].shmid, IPC_RMID, NULL);
		}
		if (seg->table[i].pvr2dmem) {
			PVR2DMemFree(pvr2d_get_screen()->context,
				     seg->table[i].pvr2dmem);
		}
		if (seg->table[i].mallocaddr) {
			free(seg->table[i].mallocaddr);
		}
	}
	seg->count = 0;
}

static void DeInitCacheSegment(cache_segment * seg)
{
	/*
	 * At this point the memory should be detached
	 */
	CALLTRACE("%s:DeInit segment %p\n", __func__, seg);
	CleanupCacheSegment(seg);
	if (seg->table)
		free(seg->table);
	seg->table = 0;
	seg->maxsize = 0;
}

void SetWindowSizeSharedSegments(int window_size)
{
	default_window_size = ALIGN(window_size, page_size);
}

int InitSharedSegments(void)
{
	int i;
	int ret = 0;

	for (i = 0; i < NUM_CACHE_SEGS; i++)
		ret |= InitCacheSegment(&segments[i], 6);

	return ret;
}

void DeInitSharedSegments(void)
{
	int i;

	for (i = 0; i < NUM_CACHE_SEGS; i++)
		DeInitCacheSegment(&segments[i]);
}

void CleanupSharedSegments(void)
{
	int i;

	for (i = 0; i < NUM_CACHE_SEGS; i++)
		CleanupCacheSegment(&segments[i]);
}

/*
 * Try to add an SHM to the cache
 * inputs:
 *	ppix:	pixmap private
 * returns:
 * 	0:	SHM has not been added
 * 	1:	SHM has been stored in cache
 */
int AddToCache(struct PVR2DPixmap *ppix)
{
	unsigned cache_id = CacheSegmentGetId(ppix->shmsize);
	CALLTRACE("%s: Start %i\n", __func__, cache_id);
	if ((ppix->shmid < 0) || (!ppix->shmaddr))
		return 0;

	assert(ppix->shmid >= 0);
	assert(ppix->shmaddr);
	assert(ppix->shmsize);
	assert(ppix->shmsize == ALIGN(ppix->shmsize, page_size));
	assert(!ppix->mallocaddr);
	assert(!ppix->mallocsize);

	// check if this segments of this size are cached
	if (cache_id >= NUM_CACHE_SEGS)
		return 0;

	CALLTRACE("%s: Mid 1\n", __func__);
	CALLTRACE("%s: Mid 2\n", __func__);

	cache_segment *seg = &segments[cache_id];
	CALLTRACE("%s: Seg %p \n", __func__, seg);
	CALLTRACE("%s: count %i, maxsize %i\n", __func__, seg->count,
		  seg->maxsize);
	// check if there is space in the list
	if (seg->count == seg->maxsize)
		return 0;
	seg->table[seg->count] = *ppix;
	seg->count++;
	DebugF("%s: Added %i,%p, size %i pages\n", __func__,
	       ppix->shmid, ppix->shmaddr, cache_id);
	return 1;
}

/*
 * Try to add an SHM to the cache
 * inputs:
 *	ppix:	pixmap private
 * outputs:	
 *	ppix:	pixmap private
 * returns:
 *      0:      no shared segment of this size is available
 *      1:      a segment has been found and the outputs have been filled
 */

int GetFromCache(struct PVR2DPixmap *ppix)
{
	unsigned cache_id = CacheSegmentGetId(ppix->shmsize);
	CALLTRACE("%s: Start %i\n", __func__, cache_id);

	// check if segments of this size are cached
	if (cache_id >= NUM_CACHE_SEGS) {
		CALLTRACE("%s: size %i, return\n", __func__, cache_id);
		return 0;
	}

	CALLTRACE("%s: Mid %i\n", __func__, cache_id);
	CALLTRACE("%s: Mid 2 %i\n", __func__, cache_id);
	cache_segment *seg = &segments[cache_id];
	CALLTRACE("%s: Seg %p \n", __func__, seg);
	CALLTRACE("%s: count %i, maxsize %i\n", __func__, seg->count, seg->maxsize);

	// check if the list is non-empty
	if (seg->count == 0)
		return 0;
	seg->count--;

	*ppix = seg->table[seg->count];

	DebugF("%s: Reused %i,%p, size %i pages\n",
	       __func__, ppix->shmid, ppix->shmaddr, cache_id);

	return 1;
}
