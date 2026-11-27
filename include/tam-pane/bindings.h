/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Key bindings and compositor dispatcher actions.
 *
 * Pure value types and chord parsing live here without wlroots dependencies so
 * unit tests exercise the exact same parser and action representation as the
 * live compositor.
 */
#ifndef TAM_PANE_BINDINGS_H
#define TAM_PANE_BINDINGS_H

#include "tam-pane/geom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct tp_server;

/* Compositor-independent modifier bitmask (Caps/NumLock excluded). */
enum tp_mod {
	TP_MOD_SHIFT = 1u << 0,
	TP_MOD_CTRL = 1u << 1,
	TP_MOD_ALT = 1u << 2,
	TP_MOD_META = 1u << 3,
};

enum tp_action_kind {
	TP_ACT_SPAWN,
	TP_ACT_TERMINAL,
	TP_ACT_CLOSE,
	TP_ACT_QUIT,
	TP_ACT_FOCUS,
	TP_ACT_SWAP,
	TP_ACT_MOVE,
	TP_ACT_RESIZE,
	TP_ACT_TOGGLE_FLOAT,
	TP_ACT_TOGGLE_FULLSCREEN,
	TP_ACT_TAG_VIEW,
	TP_ACT_TAG_TOGGLE_VIEW,
	TP_ACT_TAG_MOVE,
	TP_ACT_TAG_TOGGLE_CLIENT,
	TP_ACT_TAG_BACK,
	TP_ACT_MONITOR_FOCUS,
	TP_ACT_MONITOR_MOVE,
	TP_ACT_RELOAD,
	TP_ACT_LAYOUT_SET,
	TP_ACT_LAYOUT_CYCLE,
	TP_ACT_LAYOUT_NMASTER,
	TP_ACT_SCRATCHPAD_TOGGLE,
	TP_ACT_SCRATCHPAD_MOVE,
	TP_ACT_CHORD_TOGGLE,
	TP_ACT_CHORD_CANCEL,
	TP_ACT_DPMS,
	TP_ACT_FOCUS_APP,
	TP_ACT_KEYBOARD_NEXT,
	TP_ACT_FOCUS_DIR,
	TP_ACT_SEND_KEY,
	TP_ACT_CAPTURE,
	TP_ACT_MONITOR_SCALE,
	TP_ACT_MONITOR_MODE,
	TP_ACT_MOUSE_MOVE,
	TP_ACT_MOUSE_RESIZE,
};

struct tp_action {
	enum tp_action_kind kind;
	int dir;		  /* +1/-1 for focus/monitor, enum tp_dir for swap/move/resize */
	unsigned int tag; /* 1..TP_TAG_COUNT for tag actions */
	int32_t step;	  /* optional resize step (0 = use settings.resize_step) */
	uint32_t mods;	  /* TP_MOD_* for TP_ACT_SEND_KEY */
	xkb_keysym_t sym; /* keysym for TP_ACT_SEND_KEY */
	float scale;	  /* scale for TP_ACT_MONITOR_SCALE */
	/* TP_ACT_MONITOR_MODE: size, and refresh in mHz (0 = the highest). */
	int32_t mode_w, mode_h, mode_refresh;
	char cmd[256];	  /* command string for TP_ACT_SPAWN/CAPTURE/DPMS/FOCUS_APP */
};

struct tp_key_spec {
	uint32_t mods;
	xkb_keysym_t sym;
	uint32_t keycode; /* 0 = keysym bind; 9..255 = XKB keycode (evdev + 8) */
};

enum {
	TP_MOUSE_WHEEL_UP = 1,
	TP_MOUSE_WHEEL_DOWN = 2,
};

struct tp_mouse_spec {
	uint32_t mods;
	uint32_t button; /* Linux BTN_* (e.g. 272, 273) or TP_MOUSE_WHEEL_* */
};

/*
 * Parse a chord string such as "Meta+Shift+e", "Meta+Return", "Meta+,",
 * "Meta+code:10", or "XF86AudioRaiseVolume" into (mods, canonical level-0
 * keysym or XKB keycode). Returns false and writes a diagnostic to err on
 * invalid syntax or names.
 */
bool tp_binding_parse_chord(const char *str, struct tp_key_spec *out, char *err,
							size_t err_size);

/*
 * Parse a mouse chord string such as "Meta+mouse:273", "Meta+Alt+mouse:273",
 * "Meta+mouse_up", or "Meta+mouse_down".
 */
bool tp_binding_parse_mouse(const char *str, struct tp_mouse_spec *out,
							char *err, size_t err_size);

/* Execute a single compositor dispatcher action on s. */
void tp_action_run(struct tp_server *s, const struct tp_action *act);

/* Run the binding for (mods, sym) using TP_MOD_* bits. True when matched. */
bool tp_bindings_handle(struct tp_server *s, uint32_t mods, xkb_keysym_t sym);

/* Extended key handler supporting XKB keycodes and press/release binds. */
bool tp_bindings_handle_key(struct tp_server *s, uint32_t mods,
							xkb_keysym_t sym, uint32_t keycode, bool released);

/* Run the mouse binding for (mods, button). If the binding is a drag grab
 * (mouse_move or mouse_resize), writes the action to *out_grab and returns true. */
bool tp_bindings_handle_mouse(struct tp_server *s, uint32_t mods,
							  uint32_t button, struct tp_action *out_grab);

#endif
