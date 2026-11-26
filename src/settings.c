/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/settings.h"
#include <stdio.h>
#include <stdlib.h>

void tp_settings_defaults(struct tp_settings *s) {
	const char *term = getenv("TERMINAL");
	if (!term || !*term)
		term = "foot";

	*s = (struct tp_settings){
		.gap_inner = 8,
		.gap_outer = 8,
		.split_ratio = 0.5f,
		.width_bias = 1.0f,
		.preserve_split = false,
		.smart_split = false,
		.proportional_split = false,
		.layout = 0, /* TP_LAYOUT_DWINDLE */
		.master_ratio = 0.55f,
		.master_count = 1,
		.scroller_width = 0.5f,
		.resize_step = 40,
		.cursor_size = 24,
		.repeat_rate = 25,
		.repeat_delay = 600,
		.numlock_by_default = false,
		.natural_scroll = false,
		.scroll_factor = 1.0f,
		.chord_enabled = true,
		.chord_overlay_builtin = true,
	};
	snprintf(s->terminal, sizeof(s->terminal), "%s", term);
	s->cursor_theme[0] = '\0';
	snprintf(s->chord_key, sizeof(s->chord_key), "Caps_Lock");
	snprintf(s->chord_secondary_key, sizeof(s->chord_secondary_key), "Alt_R");
	snprintf(s->chord_cancel_key, sizeof(s->chord_cancel_key), "Escape");
}
