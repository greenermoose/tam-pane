/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Deferred compositor event queue for Lua hooks.
 *
 * Events are snapshotted as plain values after the compositor finishes its
 * own state change and drained from a Wayland idle callback so Lua hooks
 * never run re-entrantly inside a wlroots signal handler.
 */
#ifndef TAM_PANE_SERVER_EVENTS_H
#define TAM_PANE_SERVER_EVENTS_H

#include "tam-pane/events.h"
#include <stddef.h>

struct tp_client;
struct tp_monitor;
struct tp_server;
struct wl_event_source;

struct tp_event_queue {
	struct tp_event *items;
	size_t head;
	size_t len;
	size_t cap;
	struct wl_event_source *idle;
};

void tp_events_init(struct tp_server *s);
void tp_events_finish(struct tp_server *s);

/* Enqueue a pre-built event for deferred idle dispatch. */
void tp_events_emit(struct tp_server *s, const struct tp_event *ev);

/* Snapshot helpers for the six compositor events. */
void tp_events_emit_window(struct tp_server *s, enum tp_event_kind kind,
						   const struct tp_client *c, bool focused);
void tp_events_emit_tag(struct tp_server *s, const struct tp_monitor *m,
						tp_tags_t prev_tags, tp_tags_t new_tags);
void tp_events_emit_monitor(struct tp_server *s, enum tp_event_kind kind,
							const char *name, struct tp_rect area, float scale);

#endif
