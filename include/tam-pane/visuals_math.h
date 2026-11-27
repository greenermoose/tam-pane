/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure visual-effects math (no wlroots): enabled predicate, corner-radius
 * clamping, shadow bounding boxes, and rounded-box signed distance function
 * (CPU mirror of the GLES2 fragment shader).
 */
#ifndef TAM_PANE_VISUALS_MATH_H
#define TAM_PANE_VISUALS_MATH_H

#include "tam-pane/geom.h"
#include "tam-pane/settings.h"
#include <stdbool.h>
#include <stdint.h>

/* True when any P5 visual effect is active and requires the GLES2 visuals path. */
bool tp_visuals_enabled(const struct tp_settings *s);

/* Clamp corner radius to [0, min(w, h) / 2]. */
float tp_visuals_clamp_radius(int32_t radius, int32_t w, int32_t h);

/* Inner corner radius for client content inset by border_width. */
float tp_visuals_inner_radius(float outer_radius, int32_t border_width);

/* Expanded bounding box for a window's drop shadow quad. */
struct tp_rect tp_visuals_shadow_box(struct tp_rect win, int32_t radius,
									 int32_t offset_x, int32_t offset_y);

/*
 * Signed distance from point (px, py) relative to the center of a rounded box
 * with half-extents (half_w, half_h) and corner radius r.
 * Negative inside, 0 on the boundary, positive outside.
 */
float tp_visuals_sdf_rounded_box(float px, float py, float half_w, float half_h,
								 float r);

#endif
