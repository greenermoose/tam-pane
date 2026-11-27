/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Compositor-owned color temperature / gamma LUT control (0.2.16).
 */
#include "tam-pane/gamma.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include <tam.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_output.h>

static void schedule_all_outputs(struct tp_server *s) {
	if (!s || s->stopping)
		return;
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		if (m->output && m->output->enabled)
			wlr_output_schedule_frame(m->output);
	}
}

void tp_gamma_init(struct tp_server *s) {
	if (!s)
		return;
	s->gamma_temp = TP_GAMMA_TEMP_IDENTITY;
	s->gamma_transform = NULL;
	s->gamma_fallback_warned = false;
}

void tp_gamma_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->gamma_transform) {
		wlr_color_transform_unref(s->gamma_transform);
		s->gamma_transform = NULL;
	}
	s->gamma_temp = TP_GAMMA_TEMP_IDENTITY;
	s->gamma_fallback_warned = false;
}

bool tp_gamma_set(struct tp_server *s, uint32_t kelvin) {
	if (!s || s->stopping)
		return false;
	if (kelvin < TP_GAMMA_TEMP_MIN || kelvin > TP_GAMMA_TEMP_MAX)
		return false;

	struct wlr_color_transform *next = NULL;
	if (kelvin != TP_GAMMA_TEMP_IDENTITY) {
		uint16_t r[TP_GAMMA_LUT_SIZE];
		uint16_t g[TP_GAMMA_LUT_SIZE];
		uint16_t b[TP_GAMMA_LUT_SIZE];
		tp_gamma_fill_lut(kelvin, TP_GAMMA_LUT_SIZE, r, g, b);
		next = wlr_color_transform_init_lut_3x1d(TP_GAMMA_LUT_SIZE, r, g, b);
		if (!next) {
			tam_log_error("gamma: cannot create color transform for %uK",
						  kelvin);
			return false;
		}
	}

	uint32_t prev = s->gamma_temp;
	if (s->gamma_transform)
		wlr_color_transform_unref(s->gamma_transform);
	s->gamma_transform = next;
	s->gamma_temp = kelvin;
	s->gamma_fallback_warned = false;

	schedule_all_outputs(s);
	if (prev != kelvin)
		tp_events_emit_gamma(s, kelvin, kelvin != TP_GAMMA_TEMP_IDENTITY);
	tam_log_info("gamma: set temperature to %uK%s", kelvin,
				 kelvin == TP_GAMMA_TEMP_IDENTITY ? " (identity)" : "");
	return true;
}

void tp_gamma_reset(struct tp_server *s) {
	tp_gamma_set(s, TP_GAMMA_TEMP_IDENTITY);
}
