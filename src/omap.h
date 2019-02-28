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

#ifndef OMAP_H
#define OMAP_H

#include <stdint.h>
#include <stdbool.h>

struct omap_fb;
struct omap_overlay;
struct omap_output;

enum omap_format {
	OMAP_FORMAT_XRGB4444,
	OMAP_FORMAT_ARGB4444,
	OMAP_FORMAT_RGB565,
	OMAP_FORMAT_RGB888,
	OMAP_FORMAT_XRGB8888,
	OMAP_FORMAT_ARGB8888,
	OMAP_FORMAT_RGBX8888,
	OMAP_FORMAT_RGBA8888,
	OMAP_FORMAT_YUY2,
	OMAP_FORMAT_UYVY,
};

enum omap_rotate {
	OMAP_ROTATE_0,
	OMAP_ROTATE_90,
	OMAP_ROTATE_180,
	OMAP_ROTATE_270,
};

enum omap_mirror {
	OMAP_MIRROR_NONE = 0x0,
	OMAP_MIRROR_HORZ = 0x1,
	OMAP_MIRROR_VERT = 0x2,
	OMAP_MIRROR_BOTH = OMAP_MIRROR_HORZ | OMAP_MIRROR_VERT,
};

enum omap_tv_standard {
	OMAP_TV_STANDARD_PAL,
	OMAP_TV_STANDARD_NTSC,
};

enum omap_output_update {
	OMAP_OUTPUT_UPDATE_AUTO,
	OMAP_OUTPUT_UPDATE_MANUAL,
};

enum omap_output_tear {
	OMAP_OUTPUT_TEAR_NONE,
	OMAP_OUTPUT_TEAR_SYNC,
};

/* Framebuffers */

struct omap_fb *omap_fb_new(void);
void omap_fb_del(struct omap_fb *fb);

bool omap_fb_open(struct omap_fb *fb,
		  const char *device);
bool omap_fb_close(struct omap_fb *fb);

bool omap_fb_alloc(struct omap_fb *fb,
		   unsigned int w,
		   unsigned int h,
		   enum omap_format format,
		   unsigned int num_buffers,
		   unsigned int buffer_align,
		   unsigned int pitch_align);
bool omap_fb_free(struct omap_fb *fb);

/* Returns true if fb size matches */
bool omap_fb_check_size(struct omap_fb *fb,
			unsigned int w,
			unsigned int h,
			enum omap_format format,
			unsigned int num_buffers,
			unsigned int buffer_align,
			unsigned int pitch_align);

/* Assign the overlay to scan out of this framebuffer */
bool omap_fb_assign_overlay(struct omap_fb *fb,
			    struct omap_overlay *ovl);

/* Get information about the size of vram */
bool omap_fb_get_size_info(struct omap_fb *fb,
			   unsigned int *ret_total,
			   unsigned int *ret_free,
			   unsigned int *ret_largest_free_block);

/* Overlays */

struct omap_overlay *omap_overlay_new(void);
void omap_overlay_del(struct omap_overlay *ovl);

bool omap_overlay_open(struct omap_overlay *ovl,
		       const char *device,
		       const char *name);
bool omap_overlay_close(struct omap_overlay *ovl);

/* Returns the framebuffer device index for this overlay */
unsigned int omap_overlay_id(struct omap_overlay *ovl);

bool omap_fb_map(struct omap_fb *fb,
		 void **ret_ptr,
		 size_t *ret_len);
void omap_fb_unmap(struct omap_fb *fb);

/* Refresh the framebuffer information from the overlay device */
bool omap_fb_refresh_info(struct omap_fb *fb, struct omap_overlay *ovl);

/* Get information on the layout of this framebuffer */
bool omap_fb_get_info(struct omap_fb *fb,
		      unsigned int *ret_w,
		      unsigned int *ret_h,
		      unsigned int *ret_pitch);

bool omap_overlay_pan(struct omap_overlay *ovl,
		      unsigned int buffer,
		      unsigned int sx, unsigned int sy,
		      unsigned int sw, unsigned int sh);

/* Setup the overlay */
bool omap_overlay_setup(struct omap_overlay *ovl,
			unsigned int buffer,
			unsigned int sx, unsigned int sy,
			unsigned int sw, unsigned int sh,
			unsigned int dx, unsigned int dy,
			unsigned int dw, unsigned int dh,
			enum omap_mirror mirror,
			enum omap_rotate rotate);

/* show the overlay */
bool omap_overlay_enable(struct omap_overlay *ovl);
/* Hide the overlay */
bool omap_overlay_disable(struct omap_overlay *ovl);

/* Is the overlay visible? */
bool omap_overlay_enabled(struct omap_overlay *ovl);

struct omap_output *omap_overlay_get_output(struct omap_overlay *ovl);
struct omap_fb *omap_overlay_get_fb(struct omap_overlay *ovl);

/* Wait for overlay config changes to happen */
bool omap_overlay_wait(struct omap_overlay *ovl);

/* Set global alpha blending factor */
bool omap_overlay_global_alpha(struct omap_overlay *ovl,
			       unsigned int alpha);

/* Outputs */

struct omap_output *omap_output_new(void);
void omap_output_del(struct omap_output *out);

bool omap_output_open(struct omap_output *out,
		      const char *name);
bool omap_output_close(struct omap_output *out);

/* Assign the overlay to the output */
bool omap_output_assign_overlay(struct omap_output *out,
				struct omap_overlay *ovl);

/* Enable the output */
bool omap_output_enable(struct omap_output *out);

/* Disable the output */
bool omap_output_disable(struct omap_output *out);

/* Setup the output's mirroring mode and rotation angle */
bool omap_output_setup(struct omap_output *out,
		       enum omap_mirror mirror,
		       enum omap_rotate rotate);

/* Get the output's current mirroring mode and rotation angle */
bool omap_output_get_setup(struct omap_output *out,
			   enum omap_mirror *ret_mirror,
			   enum omap_rotate *ret_rotate);

/* Check if the output can handle mirroring and rotation */
bool omap_output_check(struct omap_output *out,
		       enum omap_mirror mirror,
		       enum omap_rotate rotate);

/* Set the wide screen signalling data (for TV-out) */
bool omap_output_wss(struct omap_output *out,
		     uint32_t wss);

/* Set the TV standard (for TV-out) */
bool omap_output_tv_standard(struct omap_output *out,
			     enum omap_tv_standard std);

/* Get the output capabilites */
bool omap_output_get_caps(struct omap_output *out,
			  enum omap_output_update *ret_update_mode,
			  enum omap_output_tear *ret_tearsync);

/* Enabled or disable tearsync */
bool omap_output_set_tearsync(struct omap_output *out,
			      enum omap_output_tear tearsync);

/* Set the update mode (manual vs. auto) */
bool omap_output_set_update_mode(struct omap_output *out,
				 enum omap_output_update update_mode);
/* Get the update mode (manual vs. auto) */
bool omap_output_get_update_mode(struct omap_output *out,
				 enum omap_output_update *ret_update_mode);

/* Initiate a display update (for manual update displays) */
bool omap_output_update(struct omap_output *out,
			unsigned int x,
			unsigned int y,
			unsigned int w,
			unsigned int h);

/* Wait for the display update to finish */
bool omap_output_wait_update(struct omap_output *out);

/* Configure color keying */
bool omap_output_color_key(struct omap_output *out,
			   bool enable, uint32_t key);

/* Configure alpha blending */
bool omap_output_alpha_blending(struct omap_output *out,
				bool enable);

/* Get the mode timings */
bool omap_output_get_timings(struct omap_output *out,
			     unsigned int *ret_pixclk,
			     unsigned int *ret_hdisp,
			     unsigned int *ret_hfp,
			     unsigned int *ret_hsw,
			     unsigned int *ret_hbp,
			     unsigned int *ret_vdisp,
			     unsigned int *ret_vfp,
			     unsigned int *ret_vsw,
			     unsigned int *ret_vbp);

/* Get the physical size */
bool omap_output_get_size(struct omap_output *out,
			  unsigned int *ret_xres,
			  unsigned int *ret_yres,
			  unsigned int *ret_width_um,
			  unsigned int *ret_height_um);

#endif
