/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Settings: compiled-in defaults overlaid by ~/.config/tam-pane/init.lua
 * (plans/P2-lua.md). Everything a user may change lives here, in one
 * small value struct, rather than in a table of thousands of options.
 */
#ifndef TAM_PANE_SETTINGS_H
#define TAM_PANE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	TP_SETTINGS_TERMINAL_MAX = 256,
	TP_SETTINGS_CURSOR_THEME_MAX = 128,
	TP_SETTINGS_KEY_MAX = 64,
	TP_SETTINGS_XKB_MAX = 64,
	TP_SETTINGS_XKB_OPTS_MAX = 256,
};

struct tp_settings {
	int32_t gap_inner;	 /* pixels between tiled windows */
	int32_t gap_outer;	 /* pixels between windows and the monitor edge */
	int32_t border_width;
	bool focus_follows_mouse;
	char locker[TP_SETTINGS_TERMINAL_MAX];
	float split_ratio;	 /* first child's share for a new split */
	float width_bias;	 /* dwindle: side by side when w > h * width_bias */
	bool preserve_split;	 /* dwindle: keep each split's first orientation */
	bool smart_split;		 /* dwindle: pointer direction picks the edge */
	bool proportional_split; /* dwindle: keep same-direction runs proportional */
	int layout;				 /* default enum tp_layout_kind for new tag slots */
	float master_ratio;		 /* master-stack: initial master pane fraction */
	int master_count;		 /* master-stack: initial master window count */
	float scroller_width;	 /* scroller: default column width fraction */
	int32_t resize_step;	 /* pixels per keyboard resize step */
	char terminal[TP_SETTINGS_TERMINAL_MAX];
	char cursor_theme[TP_SETTINGS_CURSOR_THEME_MAX]; /* empty: XCURSOR_THEME */
	uint32_t cursor_size;
	int32_t repeat_rate;  /* keys per second */
	int32_t repeat_delay; /* milliseconds */
	char kb_layout[TP_SETTINGS_XKB_MAX];
	char kb_variant[TP_SETTINGS_XKB_MAX];
	char kb_model[TP_SETTINGS_XKB_MAX];
	char kb_options[TP_SETTINGS_XKB_OPTS_MAX];
	char kb_rules[TP_SETTINGS_XKB_MAX];
	bool numlock_by_default;
	bool natural_scroll;
	float scroll_factor;
	bool chord_enabled;
	bool chord_overlay_builtin;
	char chord_key[TP_SETTINGS_KEY_MAX];
	char chord_secondary_key[TP_SETTINGS_KEY_MAX];
	char chord_cancel_key[TP_SETTINGS_KEY_MAX];
};

void tp_settings_defaults(struct tp_settings *s);

static inline const char *tp_settings_cursor_theme(const struct tp_settings *s) {
	return s->cursor_theme[0] ? s->cursor_theme : NULL;
}

#endif
