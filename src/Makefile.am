#  Copyright 2005 Adam Jackson.
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  on the rights to use, copy, modify, merge, publish, distribute, sub
#  license, and/or sell copies of the Software, and to permit persons to whom
#  the Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
#  ADAM JACKSON BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
#  IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
#  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

# this is obnoxious:
# -module lets us name the module exactly how we want
# -avoid-version prevents gratuitous .0.0.0 version numbers on the end
# _ladir passes a dummy rpath to libtool so the thing will actually link
# TODO: -nostdlib/-Bstatic/-lgcc platform magic, not installing the .a, etc.
PVR2D_SGX = 530
PVR2D_REV = 125
PVR2D_CFLAGS = \
	       -DLINUX \
	       -DSGX_CORE_REV=$(PVR2D_REV) \
	       -DSGX$(PVR2D_SGX) \
	       -DSGX_CACHE_SEGMENTS=1 \
	       -I/usr/include/SGX/hwdefs \
	       -I/usr/include/SGX/include4 \
	       -I/usr/src/kernel-headers/include

AM_CFLAGS = @XORG_CFLAGS@ $(PVR2D_CFLAGS)

fbdev_drv_la_LTLIBRARIES = fbdev_drv.la
fbdev_drv_la_LDFLAGS = -module -avoid-version -lm -lpvr2d
fbdev_drv_ladir = @moduledir@/drivers

fbdev_drv_la_SOURCES = \
		       crtc.c \
		       extfb.c \
		       extfb.h \
		       fbdev.c \
		       fbdev.h \
		       flip_stats.h \
		       omap.c \
		       omap.h \
		       omap_sysfs.c \
		       omap_sysfs.h \
		       omap_tvout.c \
		       omap_tvout.h \
		       omap_video.c \
		       omap_video_formats.c \
		       omap_video_formats.h \
		       output.c \
		       perf.h \
		       sgx_cache.c \
		       sgx_cache.h \
		       sgx_dri2.c \
		       sgx_dri2.h \
		       sgx_exa_user.c \
		       sgx_exa_user.h \
		       sgx_exa.c \
		       sgx_exa.h \
		       sgx_pvr2d.c \
		       sgx_pvr2d.h \
		       sgx_pvr2d_alloc.c \
		       sgx_pvr2d_alloc.h \
		       sgx_pvr2d_flip.c \
		       sgx_pvr2d_flip.h \
		       sgx_xv.c \
		       sgx_xv.h \
		       x-hash.c \
		       x-hash.h \
		       x-list.c \
		       x-list.h

if PERF
fbdev_drv_la_SOURCES += perf.c
endif

if FLIP_STATS
fbdev_drv_la_SOURCES += flip_stats.c
endif
