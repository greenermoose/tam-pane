/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Settings: compiled-in defaults until Lua configuration arrives in 0.1.0
 * (plans/P2-lua.md). Everything a user may change lives here, in one
 * small struct, rather than in a table of thousands of options.
 */
#ifndef TAM_PANE_SETTINGS_H
#define TAM_PANE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

struct tp_settings {
	int32_t gap_inner;	 /* pixels between tiled windows */
	int32_t gap_outer;	 /* pixels between windows and the monitor edge */
	float split_ratio;	 /* first child's share for a new split */
	float width_bias;	 /* dwindle: side by side when w > h * width_bias */
	bool preserve_split; /* dwindle: keep each split's first orientation */
	bool smart_split;	 /* dwindle: pointer direction picks the edge */
	const char *terminal;
	const char *cursor_theme; /* NULL: the XCURSOR_THEME default */
	uint32_t cursor_size;
	int32_t repeat_rate;  /* keys per second */
	int32_t repeat_delay; /* milliseconds */
};

void tp_settings_defaults(struct tp_settings *s);

#endif
