/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure unit tests for P5 visual-effects math (no wlroots).
 */
#include "check.h"
#include "tam-pane/settings.h"
#include "tam-pane/visuals_math.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_enabled_predicate(void) {
	struct tp_settings s;
	tp_settings_defaults(&s);
	CHECK(!tp_visuals_enabled(NULL));
	CHECK(!tp_visuals_enabled(&s));

	s.corner_radius = 12;
	CHECK(tp_visuals_enabled(&s));

	s.corner_radius = 0;
	s.shadow_enabled = true;
	CHECK(tp_visuals_enabled(&s));

	s.shadow_radius = 0;
	CHECK(!tp_visuals_enabled(&s));

	s.shadow_radius = 16;
	s.shadow_color[3] = 0.0f;
	s.shadow_inactive_color[3] = 0.0f;
	CHECK(!tp_visuals_enabled(&s));

	s.shadow_inactive_color[3] = 0.25f;
	CHECK(tp_visuals_enabled(&s));

	s.shadow_enabled = false;
	CHECK(!tp_visuals_enabled(&s));
	CHECK(!tp_visuals_blur_active(&s));

	s.blur_enabled = true;
	CHECK(tp_visuals_blur_active(&s));
	CHECK(tp_visuals_enabled(&s));

	s.blur_radius = 0;
	CHECK(!tp_visuals_blur_active(&s));
	CHECK(!tp_visuals_enabled(&s));

	s.blur_radius = 8;
	s.blur_passes = 0;
	CHECK(!tp_visuals_blur_active(&s));
	CHECK(!tp_visuals_enabled(&s));
}

static void test_blur_and_damage_math(void) {
	CHECK(tp_visuals_blur_offset(0, 2, 1.0f) == 0.0f);
	CHECK(tp_visuals_blur_offset(8, 0, 1.0f) == 0.0f);
	CHECK(fabsf(tp_visuals_blur_offset(8, 2, 1.0f) - 2.0f) < 1e-6f);
	CHECK(fabsf(tp_visuals_blur_offset(8, 2, 2.0f) - 4.0f) < 1e-6f);
	CHECK(fabsf(tp_visuals_blur_offset(1, 6, 1.0f) - 0.5f) < 1e-6f);
	CHECK(fabsf(tp_visuals_blur_offset(32, 1, 2.0f) - 4.0f) < 1e-6f);

	CHECK(tp_visuals_blur_expand(0, 2) == 0);
	CHECK(tp_visuals_blur_expand(8, 0) == 0);
	CHECK(tp_visuals_blur_expand(8, 2) == 16);
	CHECK(tp_visuals_blur_expand(8, 3) == 32);
	CHECK(tp_visuals_blur_expand(1, 1) == 8);
	CHECK(tp_visuals_blur_expand(32, 6) == 256);

	struct tp_settings s;
	tp_settings_defaults(&s);
	CHECK(tp_visuals_effect_pad(&s) == 0);
	s.shadow_enabled = true;
	s.shadow_radius = 16;
	s.shadow_offset_y = 6;
	CHECK(tp_visuals_effect_pad(&s) == 22);
	s.blur_enabled = true;
	s.blur_radius = 8;
	s.blur_passes = 3;
	CHECK(tp_visuals_effect_pad(&s) == 32);

	struct tp_rect a = {10, 10, 50, 40};
	struct tp_rect b = {40, 30, 50, 40};
	struct tp_rect c = {100, 100, 20, 20};
	CHECK(tp_visuals_rect_intersects(a, b));
	CHECK(!tp_visuals_rect_intersects(a, c));
	struct tp_rect inter = tp_visuals_rect_intersect(a, b);
	CHECK(inter.x == 40 && inter.y == 30 && inter.w == 20 && inter.h == 20);
	struct tp_rect empty = tp_visuals_rect_intersect(a, c);
	CHECK(empty.w == 0 && empty.h == 0);

	struct tp_rect bounds = {0, 0, 640, 480};
	struct tp_rect exp =
		tp_visuals_damage_expand((struct tp_rect){5, 5, 20, 20}, 16, bounds);
	CHECK(exp.x == 0 && exp.y == 0 && exp.w == 41 && exp.h == 41);
}

static void test_radius_clamping(void) {
	CHECK(tp_visuals_clamp_radius(0, 200, 100) == 0.0f);
	CHECK(tp_visuals_clamp_radius(-4, 200, 100) == 0.0f);
	CHECK(tp_visuals_clamp_radius(12, 0, 100) == 0.0f);
	CHECK(fabsf(tp_visuals_clamp_radius(12, 200, 100) - 12.0f) < 1e-6f);
	CHECK(fabsf(tp_visuals_clamp_radius(64, 200, 80) - 40.0f) < 1e-6f);
	CHECK(fabsf(tp_visuals_clamp_radius(64, 60, 200) - 30.0f) < 1e-6f);

	CHECK(fabsf(tp_visuals_inner_radius(12.0f, 3) - 9.0f) < 1e-6f);
	CHECK(tp_visuals_inner_radius(3.0f, 3) == 0.0f);
	CHECK(tp_visuals_inner_radius(2.0f, 5) == 0.0f);
	CHECK(fabsf(tp_visuals_inner_radius(12.0f, 0) - 12.0f) < 1e-6f);
}

static void test_shadow_box(void) {
	struct tp_rect win = {100, 50, 400, 300};
	struct tp_rect sb = tp_visuals_shadow_box(win, 16, 0, 4);
	CHECK(sb.x == 84);
	CHECK(sb.y == 38);
	CHECK(sb.w == 432);
	CHECK(sb.h == 332);

	struct tp_rect neg = tp_visuals_shadow_box(win, 10, -6, -4);
	CHECK(neg.x == 84);
	CHECK(neg.y == 36);
	CHECK(neg.w == 420);
	CHECK(neg.h == 320);

	struct tp_rect zero = tp_visuals_shadow_box(win, 0, 2, 3);
	CHECK(zero.x == 102 && zero.y == 53 && zero.w == 400 && zero.h == 300);
}

static void test_sdf_rounded_box(void) {
	/* 200x100 box (half = 100x50), radius = 10. */
	float d_center = tp_visuals_sdf_rounded_box(0.0f, 0.0f, 100.0f, 50.0f, 10.0f);
	CHECK(d_center < -49.0f);

	/* Midpoint of right edge: exactly on boundary (d == 0). */
	float d_edge = tp_visuals_sdf_rounded_box(100.0f, 0.0f, 100.0f, 50.0f, 10.0f);
	CHECK(fabsf(d_edge) < 1e-5f);

	/* Sharp top-right corner (100, 50) is OUTSIDE the rounded arc by (sqrt(2)-1)*r. */
	float d_corner =
		tp_visuals_sdf_rounded_box(100.0f, 50.0f, 100.0f, 50.0f, 10.0f);
	float expected = (sqrtf(2.0f) - 1.0f) * 10.0f;
	CHECK(fabsf(d_corner - expected) < 1e-4f);

	/* Point on the 45-degree arc of the rounded corner: d == 0. */
	float arc_x = 90.0f + 10.0f / sqrtf(2.0f);
	float arc_y = 40.0f + 10.0f / sqrtf(2.0f);
	float d_arc =
		tp_visuals_sdf_rounded_box(arc_x, arc_y, 100.0f, 50.0f, 10.0f);
	CHECK(fabsf(d_arc) < 1e-4f);

	/* Sharp box (r == 0): corner (100, 50) is on boundary (d == 0). */
	float d_sharp =
		tp_visuals_sdf_rounded_box(100.0f, 50.0f, 100.0f, 50.0f, 0.0f);
	CHECK(fabsf(d_sharp) < 1e-5f);
}

static void write_text_file(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	fputs(text, f);
	fclose(f);
}

static void test_animation_math_and_battery(void) {
	struct tp_settings s;
	tp_settings_defaults(&s);
	CHECK(!tp_visuals_anim_configured(&s));
	CHECK(!tp_visuals_anim_allowed(&s, false));
	CHECK(!tp_visuals_anim_allowed(&s, true));

	s.animations_enabled = true;
	CHECK(tp_visuals_anim_configured(&s));
	CHECK(tp_visuals_anim_allowed(&s, false));
	CHECK(!tp_visuals_anim_allowed(&s, true)); /* off by default on battery */

	s.animations_on_battery = true;
	CHECK(tp_visuals_anim_allowed(&s, true));

	s.animation_duration_ms = 0;
	CHECK(!tp_visuals_anim_configured(&s));
	CHECK(!tp_visuals_anim_allowed(&s, false));

	/* Curve parsing and names */
	int curve = -1;
	CHECK(tp_visuals_parse_curve("ease_out", &curve) &&
		  curve == TP_ANIM_CURVE_EASE_OUT);
	CHECK(tp_visuals_parse_curve("ease_in_out", &curve) &&
		  curve == TP_ANIM_CURVE_EASE_IN_OUT);
	CHECK(tp_visuals_parse_curve("linear", &curve) &&
		  curve == TP_ANIM_CURVE_LINEAR);
	CHECK(!tp_visuals_parse_curve("bounce", &curve));
	CHECK(strcmp(tp_visuals_curve_name(TP_ANIM_CURVE_EASE_OUT), "ease_out") == 0);
	CHECK(strcmp(tp_visuals_curve_name(TP_ANIM_CURVE_EASE_IN_OUT), "ease_in_out") == 0);
	CHECK(strcmp(tp_visuals_curve_name(TP_ANIM_CURVE_LINEAR), "linear") == 0);

	/* Easing curves */
	CHECK(tp_visuals_anim_ease(TP_ANIM_CURVE_EASE_OUT, -0.5f) == 0.0f);
	CHECK(tp_visuals_anim_ease(TP_ANIM_CURVE_EASE_OUT, 0.0f) == 0.0f);
	CHECK(tp_visuals_anim_ease(TP_ANIM_CURVE_EASE_OUT, 1.0f) == 1.0f);
	CHECK(tp_visuals_anim_ease(TP_ANIM_CURVE_EASE_OUT, 1.5f) == 1.0f);
	CHECK(fabsf(tp_visuals_anim_ease(TP_ANIM_CURVE_EASE_OUT, 0.5f) - 0.875f) < 1e-5f);
	CHECK(fabsf(tp_visuals_anim_ease(TP_ANIM_CURVE_EASE_IN_OUT, 0.5f) - 0.5f) < 1e-5f);
	CHECK(fabsf(tp_visuals_anim_ease(TP_ANIM_CURVE_EASE_IN_OUT, 0.25f) - 0.0625f) < 1e-5f);
	CHECK(fabsf(tp_visuals_anim_ease(TP_ANIM_CURVE_LINEAR, 0.35f) - 0.35f) < 1e-5f);

	/* Progress clamping */
	CHECK(tp_visuals_anim_progress(100, 50, 200) == 0.0f);
	CHECK(tp_visuals_anim_progress(100, 100, 200) == 0.0f);
	CHECK(fabsf(tp_visuals_anim_progress(100, 200, 200) - 0.5f) < 1e-5f);
	CHECK(tp_visuals_anim_progress(100, 300, 200) == 1.0f);
	CHECK(tp_visuals_anim_progress(100, 999, 200) == 1.0f);
	CHECK(tp_visuals_anim_progress(100, 100, 0) == 1.0f);

	/* Rect interpolation and center scale */
	struct tp_rect a = {0, 10, 100, 200};
	struct tp_rect b = {100, 50, 200, 100};
	struct tp_rect mid = tp_visuals_rect_lerp(a, b, 0.5f);
	CHECK(mid.x == 50 && mid.y == 30 && mid.w == 150 && mid.h == 150);
	struct tp_rect sc = tp_visuals_rect_scale_center((struct tp_rect){100, 100, 200, 100}, 0.9f);
	CHECK(sc.w == 180 && sc.h == 90 && sc.x == 110 && sc.y == 105);

	/* Tag slide direction */
	CHECK(tp_visuals_tag_slide_dir(tp_tag_mask(1), tp_tag_mask(2)) == 1);
	CHECK(tp_visuals_tag_slide_dir(tp_tag_mask(3), tp_tag_mask(1)) == -1);
	CHECK(tp_visuals_tag_slide_dir(tp_tag_mask(2), tp_tag_mask(2) | tp_tag_mask(4)) == 0);

	/* Battery sysfs detection using mock directories */
	CHECK(!tp_visuals_detect_battery_dir(NULL));
	CHECK(!tp_visuals_detect_battery_dir("/nonexistent/tam-pane-power-dir"));

	char tmpl[] = "/tmp/tam-pane-batt-XXXXXX";
	char *root = mkdtemp(tmpl);
	CHECK(root != NULL);

	/* Empty power_supply dir (desktop): not on battery. */
	CHECK(!tp_visuals_detect_battery_dir(root));

	char ac_dir[256], bat_dir[256], p1[300], p2[300], p3[300], p4[300];
	snprintf(ac_dir, sizeof(ac_dir), "%s/AC", root);
	snprintf(bat_dir, sizeof(bat_dir), "%s/BAT0", root);
	CHECK(mkdir(ac_dir, 0700) == 0);
	CHECK(mkdir(bat_dir, 0700) == 0);
	snprintf(p1, sizeof(p1), "%s/type", ac_dir);
	snprintf(p2, sizeof(p2), "%s/online", ac_dir);
	snprintf(p3, sizeof(p3), "%s/type", bat_dir);
	snprintf(p4, sizeof(p4), "%s/status", bat_dir);

	/* AC online + Battery discharging -> AC wins (not on battery). */
	write_text_file(p1, "Mains\n");
	write_text_file(p2, "1\n");
	write_text_file(p3, "Battery\n");
	write_text_file(p4, "Discharging\n");
	CHECK(!tp_visuals_detect_battery_dir(root));

	/* AC offline + Battery discharging -> on battery. */
	write_text_file(p2, "0\n");
	CHECK(tp_visuals_detect_battery_dir(root));

	/* AC offline + Battery charging/full -> not on battery. */
	write_text_file(p4, "Charging\n");
	CHECK(!tp_visuals_detect_battery_dir(root));
	write_text_file(p4, "Full\n");
	CHECK(!tp_visuals_detect_battery_dir(root));

	unlink(p1);
	unlink(p2);
	unlink(p3);
	unlink(p4);
	rmdir(ac_dir);
	rmdir(bat_dir);
	rmdir(root);
}

int main(void) {
	test_enabled_predicate();
	test_blur_and_damage_math();
	test_radius_clamping();
	test_shadow_box();
	test_sdf_rounded_box();
	test_animation_math_and_battery();
	CHECK_DONE("test_visuals");
}
