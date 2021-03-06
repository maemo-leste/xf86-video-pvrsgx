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
#
# Process this file with autoconf to produce a configure script

# Initialize Autoconf
AC_PREREQ([2.60])
AC_INIT([xf86-video-pvrsgx], [0.4.4], [spinal.by@gmail.com])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR(.)

# Initialize Automake
AM_INIT_AUTOMAKE([foreign dist-bzip2])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# Prevent producing broken configure script
m4_ifndef([PKG_PROG_PKG_CONFIG],
          [m4_fatal([must install pkg-config before running autoconf/autogen])])
m4_ifndef([XORG_DRIVER_CHECK_EXT],
          [m4_fatal([must install xserver-xorg-dev before running autoconf/autogen])])

# Require X.Org macros 1.8 or later for MAN_SUBSTS set by XORG_MANPAGE_SECTIONS
m4_ifndef([XORG_MACROS_VERSION],
          [m4_fatal([must install xorg-macros 1.8 or later before running autoconf/autogen])])
XORG_MACROS_VERSION(1.8)
XORG_DEFAULT_OPTIONS

# Initialize libtool
AC_DISABLE_STATIC
AC_PROG_LIBTOOL

AH_TOP([#include "xorg-server.h"])

# Define a configure option for an alternate module directory
AC_ARG_WITH(xorg-module-dir,
            AS_HELP_STRING([--with-xorg-module-dir=DIR],
                           [Default xorg module directory [[default=$libdir/xorg/modules]]]),
            [moduledir="$withval"],
            [moduledir="$libdir/xorg/modules"])
AC_ARG_ENABLE(perf,          AS_HELP_STRING([--enable-perf],
                             [Enable performance counter framework (default: enabled)]),
			     [PERF=$enableval], [PERF=yes])
AC_ARG_ENABLE(flip-stats,    AS_HELP_STRING([--enable-flip-stats],
                             [Enable flip statistics framework (default: enabled)]),
			     [FLIP_STATS=$enableval], [FLIP_STATS=yes])

# Store the list of server defined optional extensions in REQUIRED_MODULES
XORG_DRIVER_CHECK_EXT(RANDR, randrproto)
XORG_DRIVER_CHECK_EXT(RENDER, renderproto)
XORG_DRIVER_CHECK_EXT(XV, videoproto)
XORG_DRIVER_CHECK_EXT(DRI2, dri2proto)

# Obtain compiler/linker options for the driver dependencies
PKG_CHECK_MODULES(XORG, [xorg-server >= 1.0.99.901 xproto fontsproto $REQUIRED_MODULES])

AM_CONDITIONAL(PERF, [test "x$PERF" = xyes])
if test "x$PERF" = xyes; then
    AC_DEFINE(PERF, 1, [Enable performance counter framework])
fi

AM_CONDITIONAL(FLIP_STATS, [test "x$FLIP_STATS" = xyes])
if test "x$FLIP_STATS" = xyes; then
    AC_DEFINE(FLIP_STATS, 1, [Enable flip statistics framework])
fi

# Check for headers
AC_CHECK_HEADER(pvr2d.h, , AC_MSG_ERROR(PowerVR SGX developement headers not found))

AC_SUBST([moduledir])

DRIVER_NAME=pvrsgx
AC_SUBST([DRIVER_NAME])

AC_CONFIG_FILES([
                Makefile
                src/Makefile
                etc/Makefile
                man/Makefile
])
AC_OUTPUT
