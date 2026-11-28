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
#include "tam-pane/tags.h"
#include <stdbool.h>
#include <stdint.h>

/* True when any P5 visual effect is active and requires the GLES2 visuals path. */
bool tp_visuals_enabled(const struct tp_settings *s);

/* True when backdrop blur is enabled with non-zero radius and passes. */
bool tp_visuals_blur_active(const struct tp_settings *s);

/* Dual-Kawase per-pass shader offset in texels for the given radius, passes, and scale. */
float tp_visuals_blur_offset(int32_t radius, int32_t passes, float scale);

/* Conservative logical-pixel halo margin needed around a blurred region. */
int32_t tp_visuals_blur_expand(int32_t radius, int32_t passes);

/* Maximum logical-pixel damage expansion needed for active effects (shadow/blur). */
int32_t tp_visuals_effect_pad(const struct tp_settings *s);

/* True when two non-empty rectangles overlap. */
bool tp_visuals_rect_intersects(struct tp_rect a, struct tp_rect b);

/* Intersection of two rectangles, or {0, 0, 0, 0} when disjoint. */
struct tp_rect tp_visuals_rect_intersect(struct tp_rect a, struct tp_rect b);

/* Expand a damage rectangle by `pad` on all sides and clamp to `bounds`. */
struct tp_rect tp_visuals_damage_expand(struct tp_rect box, int32_t pad,
										struct tp_rect bounds);

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

/* True when animations are configured on with a positive duration. */
bool tp_visuals_anim_configured(const struct tp_settings *s);

/* True when animations should run given current settings and battery state. */
bool tp_visuals_anim_allowed(const struct tp_settings *s, bool on_battery);

/* Parse/format animation curve names ("ease_out", "ease_in_out", "linear"). */
bool tp_visuals_parse_curve(const char *name, int *out);
const char *tp_visuals_curve_name(int curve);

/* Easing function mapping linear progress t in [0, 1] to eased [0, 1]. */
float tp_visuals_anim_ease(int curve, float t);

/* Normalized elapsed progress in [0, 1] for monotonic millisecond timestamps. */
float tp_visuals_anim_progress(uint64_t start_ms, uint64_t now_ms,
							   int32_t duration_ms);

/* Linearly interpolate two logical rectangles at factor t in [0, 1]. */
struct tp_rect tp_visuals_rect_lerp(struct tp_rect from, struct tp_rect to,
									float t);

/* Scale a rectangle around its center by `scale` (> 0). */
struct tp_rect tp_visuals_rect_scale_center(struct tp_rect box, float scale);

/*
 * Slide direction for a tag view change:
 * +1 when moving to a higher tag, -1 for a lower tag, 0 when the lowest active
 * tag bit is unchanged (e.g. multi-tag toggle).
 */
int tp_visuals_tag_slide_dir(tp_tags_t prev_tags, tp_tags_t new_tags);

/*
 * Inspect a Linux power_supply sysfs directory (e.g. "/sys/class/power_supply")
 * and return true when the host is running on battery power. Desktops without
 * any Battery entry and laptops with online Mains/USB return false.
 */
bool tp_visuals_detect_battery_dir(const char *sysfs_dir);

#endif
