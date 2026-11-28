/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/visuals_math.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

bool tp_visuals_anim_configured(const struct tp_settings *s) {
	return s && s->animations_enabled && s->animation_duration_ms > 0;
}

bool tp_visuals_anim_allowed(const struct tp_settings *s, bool on_battery) {
	if (!tp_visuals_anim_configured(s))
		return false;
	if (on_battery && !s->animations_on_battery)
		return false;
	return true;
}

bool tp_visuals_parse_curve(const char *name, int *out) {
	if (!name || !out)
		return false;
	if (strcmp(name, "ease_out") == 0) {
		*out = TP_ANIM_CURVE_EASE_OUT;
		return true;
	}
	if (strcmp(name, "ease_in_out") == 0) {
		*out = TP_ANIM_CURVE_EASE_IN_OUT;
		return true;
	}
	if (strcmp(name, "linear") == 0) {
		*out = TP_ANIM_CURVE_LINEAR;
		return true;
	}
	return false;
}

const char *tp_visuals_curve_name(int curve) {
	switch (curve) {
	case TP_ANIM_CURVE_EASE_IN_OUT:
		return "ease_in_out";
	case TP_ANIM_CURVE_LINEAR:
		return "linear";
	case TP_ANIM_CURVE_EASE_OUT:
	default:
		return "ease_out";
	}
}

float tp_visuals_anim_ease(int curve, float t) {
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	switch (curve) {
	case TP_ANIM_CURVE_LINEAR:
		return t;
	case TP_ANIM_CURVE_EASE_IN_OUT:
		if (t < 0.5f)
			return 4.0f * t * t * t;
		{
			float u = -2.0f * t + 2.0f;
			return 1.0f - (u * u * u) * 0.5f;
		}
	case TP_ANIM_CURVE_EASE_OUT:
	default: {
		float u = 1.0f - t;
		return 1.0f - u * u * u;
	}
	}
}

float tp_visuals_anim_progress(uint64_t start_ms, uint64_t now_ms,
							   int32_t duration_ms) {
	if (duration_ms <= 0)
		return 1.0f;
	if (now_ms <= start_ms)
		return 0.0f;
	uint64_t elapsed = now_ms - start_ms;
	if (elapsed >= (uint64_t)duration_ms)
		return 1.0f;
	return (float)elapsed / (float)duration_ms;
}

struct tp_rect tp_visuals_rect_lerp(struct tp_rect from, struct tp_rect to,
									float t) {
	if (t <= 0.0f)
		return from;
	if (t >= 1.0f)
		return to;
	struct tp_rect r;
	r.x = (int32_t)lroundf((float)from.x + (float)(to.x - from.x) * t);
	r.y = (int32_t)lroundf((float)from.y + (float)(to.y - from.y) * t);
	r.w = (int32_t)lroundf((float)from.w + (float)(to.w - from.w) * t);
	r.h = (int32_t)lroundf((float)from.h + (float)(to.h - from.h) * t);
	if (r.w < 1 && (from.w > 0 || to.w > 0))
		r.w = 1;
	if (r.h < 1 && (from.h > 0 || to.h > 0))
		r.h = 1;
	return r;
}

struct tp_rect tp_visuals_rect_scale_center(struct tp_rect box, float scale) {
	if (box.w <= 0 || box.h <= 0)
		return box;
	if (scale < 0.01f)
		scale = 0.01f;
	int32_t nw = (int32_t)lroundf((float)box.w * scale);
	int32_t nh = (int32_t)lroundf((float)box.h * scale);
	if (nw < 1)
		nw = 1;
	if (nh < 1)
		nh = 1;
	int32_t nx = box.x + (box.w - nw) / 2;
	int32_t ny = box.y + (box.h - nh) / 2;
	return (struct tp_rect){nx, ny, nw, nh};
}

int tp_visuals_tag_slide_dir(tp_tags_t prev_tags, tp_tags_t new_tags) {
	unsigned int prev_first = tp_tags_first(prev_tags);
	unsigned int new_first = tp_tags_first(new_tags);
	if (prev_first == 0 || new_first == 0 || prev_first == new_first)
		return 0;
	return new_first > prev_first ? 1 : -1;
}

static bool read_sysfs_trimmed(const char *path, char *buf, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	if (!fgets(buf, (int)size, f)) {
		fclose(f);
		return false;
	}
	fclose(f);
	size_t len = strlen(buf);
	while (len > 0 && isspace((unsigned char)buf[len - 1]))
		buf[--len] = '\0';
	return len > 0;
}

bool tp_visuals_detect_battery_dir(const char *sysfs_dir) {
	if (!sysfs_dir || !*sysfs_dir)
		return false;
	DIR *dir = opendir(sysfs_dir);
	if (!dir)
		return false;

	bool any_ac_online = false;
	bool any_battery = false;
	bool any_discharging = false;
	bool any_charging_or_full = false;

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		char path[512];
		char type[64];
		int n = snprintf(path, sizeof(path), "%s/%s/type", sysfs_dir,
						 de->d_name);
		if (n < 0 || (size_t)n >= sizeof(path))
			continue;
		if (!read_sysfs_trimmed(path, type, sizeof(type)))
			continue;

		if (strcmp(type, "Mains") == 0 || strcmp(type, "USB") == 0) {
			char val[32];
			n = snprintf(path, sizeof(path), "%s/%s/online", sysfs_dir,
						 de->d_name);
			if (n > 0 && (size_t)n < sizeof(path) &&
				read_sysfs_trimmed(path, val, sizeof(val)) &&
				strcmp(val, "1") == 0) {
				any_ac_online = true;
			}
		} else if (strcmp(type, "Battery") == 0) {
			any_battery = true;
			char status[64];
			n = snprintf(path, sizeof(path), "%s/%s/status", sysfs_dir,
						 de->d_name);
			if (n > 0 && (size_t)n < sizeof(path) &&
				read_sysfs_trimmed(path, status, sizeof(status))) {
				if (strcmp(status, "Discharging") == 0)
					any_discharging = true;
				else if (strcmp(status, "Charging") == 0 ||
						 strcmp(status, "Full") == 0 ||
						 strcmp(status, "Not charging") == 0)
					any_charging_or_full = true;
			}
		}
	}
	closedir(dir);

	if (any_ac_online || !any_battery)
		return false;
	if (any_discharging)
		return true;
	if (any_charging_or_full)
		return false;
	return true;
}
