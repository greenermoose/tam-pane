/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Native color temperature / gamma LUT control (0.2.16).
 *
 * Pure Planckian color-temperature math lives in src/gamma_math.c (no wlroots,
 * unit-tested without a display). Compositor state and wlr_color_transform
 * integration live in src/gamma.c.
 */
#ifndef TAM_PANE_GAMMA_H
#define TAM_PANE_GAMMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tp_server;

enum {
	TP_GAMMA_TEMP_MIN = 1000,
	TP_GAMMA_TEMP_MAX = 25000,
	TP_GAMMA_TEMP_IDENTITY = 6500,
	TP_GAMMA_LUT_SIZE = 256,
};

/*
 * Pure color-temperature math: convert Kelvin (clamped to [1000, 25000]) into
 * RGB channel multipliers in [0.0, 1.0]. 6500 K yields exact {1.0, 1.0, 1.0}.
 */
void tp_gamma_kelvin_rgb(uint32_t kelvin, float rgb[3]);

/*
 * Fill three 16-bit 1D LUT ramps (0..65535) of length `size` (must be >= 2)
 * for `kelvin`.
 */
void tp_gamma_fill_lut(uint32_t kelvin, size_t size, uint16_t *r, uint16_t *g,
					   uint16_t *b);

/* Compositor-side gamma lifecycle and control. */
void tp_gamma_init(struct tp_server *s);
void tp_gamma_finish(struct tp_server *s);
bool tp_gamma_set(struct tp_server *s, uint32_t kelvin);
void tp_gamma_reset(struct tp_server *s);

#endif
