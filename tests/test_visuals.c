/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure unit tests for P5 visual-effects math (no wlroots).
 */
#include "check.h"
#include "tam-pane/settings.h"
#include "tam-pane/visuals_math.h"
#include <math.h>

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

int main(void) {
	test_enabled_predicate();
	test_blur_and_damage_math();
	test_radius_clamping();
	test_shadow_box();
	test_sdf_rounded_box();
	CHECK_DONE("test_visuals");
}
