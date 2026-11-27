/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure color-temperature (Kelvin) to RGB and 16-bit LUT ramp math.
 * No wlroots or Wayland dependencies; unit-tested directly in tests/test_gamma.c.
 */
#include "tam-pane/gamma.h"
#include <math.h>

static float clamp01(double v) {
	if (v <= 0.0)
		return 0.0f;
	if (v >= 1.0)
		return 1.0f;
	return (float)v;
}

void tp_gamma_kelvin_rgb(uint32_t kelvin, float rgb[3]) {
	if (!rgb)
		return;
	if (kelvin < TP_GAMMA_TEMP_MIN)
		kelvin = TP_GAMMA_TEMP_MIN;
	if (kelvin > TP_GAMMA_TEMP_MAX)
		kelvin = TP_GAMMA_TEMP_MAX;

	if (kelvin == TP_GAMMA_TEMP_IDENTITY) {
		rgb[0] = 1.0f;
		rgb[1] = 1.0f;
		rgb[2] = 1.0f;
		return;
	}

	/* Tanner Helland Planckian locus approximation (temp in hundreds of K). */
	double t = (double)kelvin / 100.0;
	double r, g, b;

	if (t <= 66.0) {
		r = 255.0;
		g = 99.4708025861 * log(t) - 161.1195681661;
		if (t <= 19.0)
			b = 0.0;
		else
			b = 138.5177312231 * log(t - 10.0) - 305.0447927307;
	} else {
		r = 329.698727446 * pow(t - 60.0, -0.1332047592);
		g = 288.1221695283 * pow(t - 60.0, -0.0755148492);
		b = 255.0;
	}

	rgb[0] = clamp01(r / 255.0);
	rgb[1] = clamp01(g / 255.0);
	rgb[2] = clamp01(b / 255.0);
}

void tp_gamma_fill_lut(uint32_t kelvin, size_t size, uint16_t *r, uint16_t *g,
					   uint16_t *b) {
	if (size < 2 || !r || !g || !b)
		return;

	float rgb[3];
	tp_gamma_kelvin_rgb(kelvin, rgb);

	double denom = (double)(size - 1);
	for (size_t i = 0; i < size; i++) {
		double frac = (double)i / denom;
		long vr = lround(frac * 65535.0 * (double)rgb[0]);
		long vg = lround(frac * 65535.0 * (double)rgb[1]);
		long vb = lround(frac * 65535.0 * (double)rgb[2]);
		r[i] = (uint16_t)(vr < 0 ? 0 : (vr > 65535 ? 65535 : vr));
		g[i] = (uint16_t)(vg < 0 ? 0 : (vg > 65535 ? 65535 : vg));
		b[i] = (uint16_t)(vb < 0 ? 0 : (vb > 65535 ? 65535 : vb));
	}
}
