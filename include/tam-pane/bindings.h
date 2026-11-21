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
};

struct tp_action {
	enum tp_action_kind kind;
	int dir;		  /* +1/-1 for focus/monitor, enum tp_dir for swap/move/resize */
	unsigned int tag; /* 1..TP_TAG_COUNT for tag actions */
	int32_t step;	  /* optional resize step (0 = use settings.resize_step) */
	char cmd[256];	  /* command string for TP_ACT_SPAWN */
};

struct tp_key_spec {
	uint32_t mods;
	xkb_keysym_t sym;
};

/*
 * Parse a chord string such as "Meta+Shift+e", "Meta+Return", "Meta+,", or
 * "XF86AudioRaiseVolume" into (mods, canonical level-0 keysym). Returns false
 * and writes a diagnostic to err (if non-NULL) on invalid syntax or names.
 */
bool tp_binding_parse_chord(const char *str, struct tp_key_spec *out, char *err,
							size_t err_size);

/* Execute a single compositor dispatcher action on s. */
void tp_action_run(struct tp_server *s, const struct tp_action *act);

/* Run the binding for (mods, sym) using TP_MOD_* bits. True when matched. */
bool tp_bindings_handle(struct tp_server *s, uint32_t mods, xkb_keysym_t sym);

#endif
