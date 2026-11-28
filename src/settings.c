/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_nibble(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

bool tp_settings_parse_color(const char *str, float out[4]) {
	if (!str || !out)
		return false;
	if (str[0] == '\0' || strcmp(str, "none") == 0) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return true;
	}
	const char *hex = NULL;
	if (str[0] == '#')
		hex = str + 1;
	else if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
		hex = str + 2;
	else
		return false;

	size_t len = strlen(hex);
	if (len != 6 && len != 8)
		return false;
	uint8_t bytes[4] = {0, 0, 0, 255};
	for (size_t i = 0; i < len / 2; i++) {
		int hi = hex_nibble(hex[i * 2]);
		int lo = hex_nibble(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return false;
		bytes[i] = (uint8_t)((hi << 4) | lo);
	}
	float a = (float)bytes[3] / 255.0f;
	/* wlroots wlr_scene_rect expects premultiplied RGBA. */
	out[0] = ((float)bytes[0] / 255.0f) * a;
	out[1] = ((float)bytes[1] / 255.0f) * a;
	out[2] = ((float)bytes[2] / 255.0f) * a;
	out[3] = a;
	return true;
}

void tp_settings_defaults(struct tp_settings *s) {
	const char *term = getenv("TERMINAL");
	if (!term || !*term)
		term = "foot";

	*s = (struct tp_settings){
		.gap_inner = 8,
		.gap_outer = 8,
		.border_color_focused = {0.0f, 1.0f, 0.0f, 1.0f},
		.border_color_unfocused = {0.0f, 0.0f, 0.0f, 0.0f},
		.corner_radius = 0,
		.shadow_enabled = false,
		.shadow_radius = 16,
		.shadow_offset_x = 0,
		.shadow_offset_y = 4,
		.shadow_color = {0.0f, 0.0f, 0.0f, 0.45f},
		.shadow_inactive_color = {0.0f, 0.0f, 0.0f, 0.25f},
		.blur_enabled = false,
		.blur_radius = 8,
		.blur_passes = 2,
		.blur_panels = true,
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
