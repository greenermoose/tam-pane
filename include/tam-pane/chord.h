/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * One-handed chord composer and filterable command menu state machine.
 *
 * Pure module (no wlroots dependency; links only xkbcommon and libtam) so
 * unit tests exercise every state transition without a display.
 */
#ifndef TAM_PANE_CHORD_H
#define TAM_PANE_CHORD_H

#include "tam-pane/bindings.h"
#include "tam-pane/lua.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

enum {
	TP_CHORD_FILTER_MAX = 64,
	TP_CHORD_VISIBLE_ROWS = 12,
};

enum tp_chord_mode {
	TP_CHORD_IDLE = 0,
	TP_CHORD_PLURIPOTENT,
	TP_CHORD_BUILDING,
	TP_CHORD_CAPS_LOCK,
};

enum tp_chord_result_kind {
	TP_CHORD_RES_PASSTHROUGH = 0,
	TP_CHORD_RES_CONSUME,
	TP_CHORD_RES_CAPS_LETTER,
	TP_CHORD_RES_FINISH_CHORD,
	TP_CHORD_RES_RUN_ITEM,
};

struct tp_chord_item {
	uint32_t mods;
	xkb_keysym_t sym;
	bool is_lua;
	size_t bind_index;
	struct tp_action action;
	char chord_str[64];
	char desc[128];
};

struct tp_chord_result {
	enum tp_chord_result_kind kind;
	uint32_t mods;
	xkb_keysym_t sym;
	uint32_t keycode;
	struct tp_chord_item item; /* valid for TP_CHORD_RES_RUN_ITEM */
	bool started;
	bool updated;
	bool ended;
	bool cancelled;
	bool caps_changed;
};

struct tp_chord_state {
	bool enabled;
	xkb_keysym_t chord_sym;
	xkb_keysym_t secondary_sym;

	enum tp_chord_mode mode;
	bool return_to_caps_lock;
	bool confirm_with_secondary;
	bool menu_navigated;
	bool secondary_down;
	bool secondary_solo;

	uint32_t mods;
	xkb_keysym_t sym;
	uint32_t keycode;
	char filter[TP_CHORD_FILTER_MAX];
	size_t filter_len;

	struct tp_chord_item *items;
	size_t items_len;
	size_t items_cap;

	size_t *filtered;
	size_t filtered_len;
	size_t filtered_cap;
	size_t selected;
	size_t scroll;
};

void tp_chord_init(struct tp_chord_state *st);
void tp_chord_finish(struct tp_chord_state *st);

/* Apply chord settings. The legacy cancel key is accepted for compatibility. */
void tp_chord_configure(struct tp_chord_state *st, bool enabled,
						const char *chord_key, const char *secondary_key,
						const char *cancel_key);

/* True when the chord composer overlay should be visible. */
bool tp_chord_is_composing(const struct tp_chord_state *st);

/* True if the current menu selection is ready to run. */
bool tp_chord_menu_ready(const struct tp_chord_state *st);

/* True when Regular Caps Lock mode is active. */
bool tp_chord_is_caps_lock(const struct tp_chord_state *st);

/* Human-readable mode name ("idle", "pluripotent", "building", "caps_lock"). */
const char *tp_chord_mode_name(enum tp_chord_mode mode);

/* Format (mods, sym) as "Meta+Shift+1" (or "Meta+" if sym == NoSymbol). */
void tp_chord_format(uint32_t mods, xkb_keysym_t sym, char *buf,
					 size_t buf_size);

/* Rebuild the menu catalog from active Lua bindings and refilter. */
void tp_chord_rebuild_catalog(struct tp_chord_state *st,
							  const struct tp_lua *lua);

/* Programmatically open (from IPC/dispatch) or cancel chord composing. */
bool tp_chord_open(struct tp_chord_state *st, const struct tp_lua *lua,
				   bool pluripotent);
bool tp_chord_cancel(struct tp_chord_state *st);

/*
 * Process one keyboard key press/release through the chord state machine.
 * - raw_sym: keysym before lowercasing (for Alt_R, Caps_Lock, Escape, etc.)
 * - sym: level-0 lowercased keysym (for binding/letter matching)
 * - keycode: Linux evdev keycode
 * - pressed: true on press, false on release
 * - phys_mods: currently held physical TP_MOD_* modifiers (excluding this key)
 */
struct tp_chord_result tp_chord_on_key(struct tp_chord_state *st,
									   const struct tp_lua *lua,
									   xkb_keysym_t raw_sym, xkb_keysym_t sym,
									   uint32_t keycode, bool pressed,
									   uint32_t phys_mods);

#endif
