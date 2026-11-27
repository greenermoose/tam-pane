/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/visuals_math.h"
#include <math.h>

bool tp_visuals_enabled(const struct tp_settings *s) {
	if (!s)
		return false;
	if (s->corner_radius > 0)
		return true;
	if (s->shadow_enabled && s->shadow_radius > 0 &&
		(s->shadow_color[3] > 0.0f || s->shadow_inactive_color[3] > 0.0f))
		return true;
	return false;
}

float tp_visuals_clamp_radius(int32_t radius, int32_t w, int32_t h) {
	if (radius <= 0 || w <= 0 || h <= 0)
		return 0.0f;
	float max_r = 0.5f * (float)(w < h ? w : h);
	float r = (float)radius;
	return r > max_r ? max_r : r;
}

float tp_visuals_inner_radius(float outer_radius, int32_t border_width) {
	if (outer_radius <= 0.0f)
		return 0.0f;
	if (border_width <= 0)
		return outer_radius;
	float r = outer_radius - (float)border_width;
	return r > 0.0f ? r : 0.0f;
}

struct tp_rect tp_visuals_shadow_box(struct tp_rect win, int32_t radius,
									 int32_t offset_x, int32_t offset_y) {
	int32_t pad = radius > 0 ? radius : 0;
	return (struct tp_rect){
		.x = win.x + offset_x - pad,
		.y = win.y + offset_y - pad,
		.w = win.w + 2 * pad,
		.h = win.h + 2 * pad,
	};
}

float tp_visuals_sdf_rounded_box(float px, float py, float half_w, float half_h,
								 float r) {
	float qx = fabsf(px) - (half_w - r);
	float qy = fabsf(py) - (half_h - r);
	float ox = qx > 0.0f ? qx : 0.0f;
	float oy = qy > 0.0f ? qy : 0.0f;
	float outside = sqrtf(ox * ox + oy * oy);
	float maxq = qx > qy ? qx : qy;
	float inside = maxq < 0.0f ? maxq : 0.0f;
	return outside + inside - r;
}
