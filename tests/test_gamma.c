/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure unit tests for color-temperature (Kelvin) to RGB and 16-bit LUT math.
 */
#include "check.h"
#include "tam-pane/gamma.h"
#include <math.h>
#include <stdint.h>

static void test_identity(void) {
	float rgb[3] = {0};
	tp_gamma_kelvin_rgb(TP_GAMMA_TEMP_IDENTITY, rgb);
	CHECK(fabsf(rgb[0] - 1.0f) < 1e-6f);
	CHECK(fabsf(rgb[1] - 1.0f) < 1e-6f);
	CHECK(fabsf(rgb[2] - 1.0f) < 1e-6f);

	uint16_t r[TP_GAMMA_LUT_SIZE];
	uint16_t g[TP_GAMMA_LUT_SIZE];
	uint16_t b[TP_GAMMA_LUT_SIZE];
	tp_gamma_fill_lut(TP_GAMMA_TEMP_IDENTITY, TP_GAMMA_LUT_SIZE, r, g, b);
	CHECK(r[0] == 0 && g[0] == 0 && b[0] == 0);
	CHECK(r[TP_GAMMA_LUT_SIZE - 1] == 65535);
	CHECK(g[TP_GAMMA_LUT_SIZE - 1] == 65535);
	CHECK(b[TP_GAMMA_LUT_SIZE - 1] == 65535);
	for (size_t i = 1; i < TP_GAMMA_LUT_SIZE; i++) {
		CHECK(r[i] >= r[i - 1]);
		CHECK(g[i] >= g[i - 1]);
		CHECK(b[i] >= b[i - 1]);
		CHECK(r[i] == g[i] && g[i] == b[i]);
	}
}

static void test_warm_and_cool(void) {
	float warm[3] = {0};
	tp_gamma_kelvin_rgb(4000, warm);
	CHECK(fabsf(warm[0] - 1.0f) < 1e-6f);
	CHECK(warm[1] > 0.7f && warm[1] < 0.9f);
	CHECK(warm[2] > 0.4f && warm[2] < warm[1]);

	float very_warm[3] = {0};
	tp_gamma_kelvin_rgb(1900, very_warm);
	CHECK(fabsf(very_warm[0] - 1.0f) < 1e-6f);
	CHECK(very_warm[1] < warm[1]);
	CHECK(very_warm[2] == 0.0f);

	float cool[3] = {0};
	tp_gamma_kelvin_rgb(10000, cool);
	CHECK(fabsf(cool[2] - 1.0f) < 1e-6f);
	CHECK(cool[0] < 1.0f && cool[0] > 0.5f);
	CHECK(cool[1] < 1.0f && cool[1] > cool[0]);

	uint16_t r[64], g[64], b[64];
	tp_gamma_fill_lut(4000, 64, r, g, b);
	CHECK(r[0] == 0 && g[0] == 0 && b[0] == 0);
	CHECK(r[63] == 65535);
	CHECK(g[63] < r[63]);
	CHECK(b[63] < g[63]);
	for (size_t i = 1; i < 64; i++) {
		CHECK(r[i] >= r[i - 1]);
		CHECK(g[i] >= g[i - 1]);
		CHECK(b[i] >= b[i - 1]);
	}
}

static void test_clamping(void) {
	float low0[3] = {0}, lowmin[3] = {0};
	tp_gamma_kelvin_rgb(0, low0);
	tp_gamma_kelvin_rgb(TP_GAMMA_TEMP_MIN, lowmin);
	CHECK(fabsf(low0[0] - lowmin[0]) < 1e-6f);
	CHECK(fabsf(low0[1] - lowmin[1]) < 1e-6f);
	CHECK(fabsf(low0[2] - lowmin[2]) < 1e-6f);

	float highmax[3] = {0}, highover[3] = {0};
	tp_gamma_kelvin_rgb(TP_GAMMA_TEMP_MAX, highmax);
	tp_gamma_kelvin_rgb(99999, highover);
	CHECK(fabsf(highmax[0] - highover[0]) < 1e-6f);
	CHECK(fabsf(highmax[1] - highover[1]) < 1e-6f);
	CHECK(fabsf(highmax[2] - highover[2]) < 1e-6f);
}

int main(void) {
	test_identity();
	test_warm_and_cool();
	test_clamping();
	CHECK_DONE("test_gamma");
}
