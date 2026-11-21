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
		.resize_step = 40,
		.cursor_size = 24,
		.repeat_rate = 25,
		.repeat_delay = 600,
	};
	snprintf(s->terminal, sizeof(s->terminal), "%s", term);
	s->cursor_theme[0] = '\0';
}
