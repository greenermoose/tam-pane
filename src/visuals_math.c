/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/visuals_math.h"
#include <math.h>

bool tp_visuals_blur_active(const struct tp_settings *s) {
	return s && s->blur_enabled && s->blur_radius > 0 && s->blur_passes > 0;
}

bool tp_visuals_enabled(const struct tp_settings *s) {
	if (!s)
		return false;
	if (s->corner_radius > 0)
		return true;
	if (s->shadow_enabled && s->shadow_radius > 0 &&
		(s->shadow_color[3] > 0.0f || s->shadow_inactive_color[3] > 0.0f))
		return true;
	if (tp_visuals_blur_active(s))
		return true;
	return false;
}

float tp_visuals_blur_offset(int32_t radius, int32_t passes, float scale) {
	if (radius <= 0 || passes <= 0)
		return 0.0f;
	if (scale <= 0.0f)
		scale = 1.0f;
	int32_t p = passes > 6 ? 6 : passes;
	float denom = (float)(1 << (p > 1 ? p - 1 : 0));
	float off = ((float)radius * 0.5f * scale) / denom;
	if (off < 0.5f)
		off = 0.5f;
	if (off > 4.0f)
		off = 4.0f;
	return off;
}

int32_t tp_visuals_blur_expand(int32_t radius, int32_t passes) {
	if (radius <= 0 || passes <= 0)
		return 0;
	int32_t p = passes > 6 ? 6 : passes;
	int32_t pad = radius * (1 << (p > 1 ? p - 1 : 0));
	int32_t min_pad = 4 * (1 << p);
	if (pad < min_pad)
		pad = min_pad;
	if (pad > 256)
		pad = 256;
	return pad;
}

int32_t tp_visuals_effect_pad(const struct tp_settings *s) {
	if (!s)
		return 0;
	int32_t pad = 0;
	if (s->shadow_enabled && s->shadow_radius > 0 &&
		(s->shadow_color[3] > 0.0f || s->shadow_inactive_color[3] > 0.0f)) {
		int32_t ox = s->shadow_offset_x < 0 ? -s->shadow_offset_x : s->shadow_offset_x;
		int32_t oy = s->shadow_offset_y < 0 ? -s->shadow_offset_y : s->shadow_offset_y;
		int32_t spad = s->shadow_radius + (ox > oy ? ox : oy);
		if (spad > pad)
			pad = spad;
	}
	if (tp_visuals_blur_active(s)) {
		int32_t bpad = tp_visuals_blur_expand(s->blur_radius, s->blur_passes);
		if (bpad > pad)
			pad = bpad;
	}
	return pad;
}

bool tp_visuals_rect_intersects(struct tp_rect a, struct tp_rect b) {
	return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w &&
		   a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

struct tp_rect tp_visuals_rect_intersect(struct tp_rect a, struct tp_rect b) {
	if (!tp_visuals_rect_intersects(a, b))
		return (struct tp_rect){0, 0, 0, 0};
	int32_t x0 = a.x > b.x ? a.x : b.x;
	int32_t y0 = a.y > b.y ? a.y : b.y;
	int32_t x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
	int32_t y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
	return (struct tp_rect){x0, y0, x1 - x0, y1 - y0};
}

struct tp_rect tp_visuals_damage_expand(struct tp_rect box, int32_t pad,
										struct tp_rect bounds) {
	if (box.w <= 0 || box.h <= 0 || bounds.w <= 0 || bounds.h <= 0)
		return (struct tp_rect){0, 0, 0, 0};
	int32_t p = pad > 0 ? pad : 0;
	struct tp_rect exp = {
		.x = box.x - p,
		.y = box.y - p,
		.w = box.w + 2 * p,
		.h = box.h + 2 * p,
	};
	return tp_visuals_rect_intersect(exp, bounds);
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
