/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Compositor-independent event types delivered to Lua event hooks (tp.on).
 *
 * Pure data only (no wlroots, no Wayland, no live compositor pointers) so
 * events can be snapshotted safely and drained after wlroots callbacks return,
 * and unit-tested without a display.
 */
#ifndef TAM_PANE_EVENTS_H
#define TAM_PANE_EVENTS_H

#include "tam-pane/geom.h"
#include "tam-pane/tags.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum tp_event_kind {
	TP_EVENT_WINDOW_OPEN,
	TP_EVENT_WINDOW_CLOSE,
	TP_EVENT_WINDOW_FOCUS,
	TP_EVENT_TAG_CHANGE,
	TP_EVENT_MONITOR_ADD,
	TP_EVENT_MONITOR_REMOVE,
	TP_EVENT_MONITOR_FOCUS,
	TP_EVENT_OUTPUT_POWER,
	TP_EVENT_KEYBOARD_LAYOUT,
	TP_EVENT_CONFIG_RELOAD,
	TP_EVENT_CHORD_START,
	TP_EVENT_CHORD_UPDATE,
	TP_EVENT_CHORD_END,
	TP_EVENT_CHORD_CANCEL,
	TP_EVENT_STARTUP, /* once, after the Wayland socket is up */
	TP_EVENT_COUNT,
};

struct tp_event_chord {
	char mode[24];
	char chord[64];
	char filter[64];
	char result[24];
	char action[128];
	size_t matches;
	size_t selected;
};

struct tp_event_window {
	char app_id[256];
	char title[256];
	char monitor[64];
	tp_tags_t tags;
	bool focused; /* true for a focused window, false when focus cleared */
	bool floating;
	bool fullscreen;
	struct tp_rect geom;
};

struct tp_event_tag {
	char monitor[64];
	tp_tags_t tags;
	tp_tags_t prev_tags;
};

struct tp_event_monitor {
	char name[64];
	struct tp_rect area;
	float scale;
};

struct tp_event_output_power {
	char monitor[64];
	bool dpms;
};

struct tp_event_keyboard {
	char name[64];
	char active_keymap[128];
	uint32_t active_layout_index;
};

struct tp_event {
	enum tp_event_kind kind;
	union {
		struct tp_event_window window;
		struct tp_event_tag tag;
		struct tp_event_monitor monitor;
		struct tp_event_output_power output_power;
		struct tp_event_keyboard keyboard;
		struct tp_event_chord chord;
	};
};

/* Canonical Lua name for an event kind, or NULL for invalid values. */
const char *tp_event_kind_name(enum tp_event_kind kind);

/* Parse a Lua event name into *out. Returns false for unknown names. */
bool tp_event_kind_from_name(const char *name, enum tp_event_kind *out);

#endif
