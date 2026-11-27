/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Deferred compositor event queue for Lua hooks.
 *
 * Events are snapshotted as plain values after the compositor finishes its
 * own state change and drained from a Wayland idle callback so Lua hooks
 * never run re-entrantly inside a wlroots signal handler.
 *
 * Events that hooks cause while they run are drained by a timer instead,
 * never by a new idle source: libwayland runs idle sources until none are
 * left, so re-adding one would starve epoll. Each event carries the depth
 * of the hook cascade that caused it, and the cascade is cut after
 * TP_EVENT_CASCADE_MAX rounds, so a hook that re-triggers itself costs a
 * few milliseconds rather than the session.
 */
#ifndef TAM_PANE_SERVER_EVENTS_H
#define TAM_PANE_SERVER_EVENTS_H

#include "tam-pane/events.h"
#include <stdbool.h>
#include <stddef.h>

struct tp_client;
struct tp_monitor;
struct tp_server;
struct wl_event_source;

enum {
	TP_EVENT_CASCADE_MAX = 16, /* rounds of hook-caused events allowed */
};

struct tp_queued_event {
	struct tp_event ev;
	unsigned int depth; /* 0: caused by the compositor; n: by round n */
};

struct tp_event_queue {
	struct tp_queued_event *items;
	size_t head;
	size_t len;
	size_t cap;
	struct wl_event_source *idle;  /* first batch, after this dispatch */
	struct wl_event_source *timer; /* hook-caused batches, after epoll */
	bool draining;				   /* hooks are running now */
	unsigned int depth;			   /* depth of the event being handled */
};

void tp_events_init(struct tp_server *s);
void tp_events_finish(struct tp_server *s);

/* Enqueue a pre-built event for deferred idle dispatch. */
void tp_events_emit(struct tp_server *s, const struct tp_event *ev);

/* Snapshot helpers for the window, tag and monitor events; tp_server_start
 * emits the startup event directly. */
void tp_events_emit_window(struct tp_server *s, enum tp_event_kind kind,
						   const struct tp_client *c, bool focused);
void tp_events_emit_tag(struct tp_server *s, const struct tp_monitor *m,
						tp_tags_t prev_tags, tp_tags_t new_tags);
void tp_events_emit_monitor(struct tp_server *s, enum tp_event_kind kind,
							const char *name, struct tp_rect area, float scale);
void tp_events_emit_monitor_focus(struct tp_server *s, const char *name);
void tp_events_emit_output_power(struct tp_server *s, const char *monitor,
								 bool dpms);
void tp_events_emit_keyboard_layout(struct tp_server *s, const char *name,
									const char *active_keymap,
									uint32_t active_layout_index);
void tp_events_emit_config_reload(struct tp_server *s);
void tp_events_emit_chord(struct tp_server *s, enum tp_event_kind kind,
						  const char *result, const char *action);
void tp_events_emit_session_lock(struct tp_server *s, bool locked);
void tp_events_emit_gamma(struct tp_server *s, uint32_t temperature,
						  bool enabled);

#endif
