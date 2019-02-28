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

#ifndef OMAP_TVOUT_H
#define OMAP_TVOUT_H

#include "fbdev.h"

void omap_tvout_calc_scaling(Bool widescreen,
			     enum omap_tv_standard tv_std,
			     unsigned int tv_scale,
			     unsigned int in_w,
			     unsigned int in_h,
			     unsigned int *out_x,
			     unsigned int *out_y,
			     unsigned int *out_w,
			     unsigned int *out_h,
			     CARD32 *out_wss,
			     int maxwidth,
			     int maxheight,
			     int xoffset,
			     int yoffset,
			     Bool dynamic_aspect_ratio);

#endif
