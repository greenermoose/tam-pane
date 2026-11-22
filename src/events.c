/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/server_events.h"
#include "tam-pane/client.h"
#include "tam-pane/ipc.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>

enum {
	TP_EVENT_QUEUE_MAX = 256,
};

static void copy_str(char *dst, size_t dst_size, const char *src) {
	if (!dst_size)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_size, "%s", src);
}

static int drain_events_timer(void *data);

/* Hook-caused events wait for the timer, so epoll runs between batches. */
static void schedule_later(struct tp_server *s) {
	struct tp_event_queue *q = &s->events;
	if (!q->timer)
		q->timer = wl_event_loop_add_timer(s->loop, drain_events_timer, s);
	if (q->timer)
		wl_event_source_timer_update(q->timer, 1);
}

static bool event_has_listeners(const struct tp_server *s,
								enum tp_event_kind kind) {
	if (!s || s->stopping)
		return false;
	if (s->lua && tp_lua_has_hooks(s->lua, kind))
		return true;
	return tp_ipc_has_subscribers(s, kind);
}

/* Deliver the events queued now; events the hooks cause go to a later
 * batch, one cascade round deeper. */
static void drain_events(struct tp_server *s) {
	struct tp_event_queue *q = &s->events;
	if (s->stopping)
		return;

	size_t to_drain = q->len;
	q->draining = true;
	for (size_t i = 0; i < to_drain && q->len > 0 && !s->stopping; i++) {
		struct tp_queued_event item = q->items[q->head];
		q->head = (q->head + 1) % q->cap;
		q->len--;
		q->depth = item.depth;
		if (s->lua && tp_lua_has_hooks(s->lua, item.ev.kind))
			tp_lua_emit_event(s->lua, &item.ev);
		tp_ipc_emit_event(s, &item.ev);
	}
	q->draining = false;
	q->depth = 0;

	if (q->len > 0 && !s->stopping && s->loop)
		schedule_later(s);
}

static void drain_events_idle(void *data) {
	struct tp_server *s = data;
	s->events.idle = NULL;
	drain_events(s);
}

static int drain_events_timer(void *data) {
	drain_events(data);
	return 0;
}

void tp_events_init(struct tp_server *s) {
	s->events = (struct tp_event_queue){0};
}

void tp_events_finish(struct tp_server *s) {
	struct tp_event_queue *q = &s->events;
	if (q->idle) {
		wl_event_source_remove(q->idle);
		q->idle = NULL;
	}
	if (q->timer) {
		wl_event_source_remove(q->timer);
		q->timer = NULL;
	}
	free(q->items);
	q->items = NULL;
	q->head = 0;
	q->len = 0;
	q->cap = 0;
}

void tp_events_emit(struct tp_server *s, const struct tp_event *ev) {
	if (!s || !ev || s->stopping || !s->loop)
		return;
	if (!event_has_listeners(s, ev->kind))
		return;

	struct tp_event_queue *q = &s->events;
	const char *name = tp_event_kind_name(ev->kind);
	unsigned int depth = q->draining ? q->depth + 1 : 0;
	if (depth > TP_EVENT_CASCADE_MAX) {
		tam_log_warn("lua hooks: dropping %s after %d rounds of events "
					 "caused by hooks (does a hook re-trigger itself?)",
					 name ? name : "event", TP_EVENT_CASCADE_MAX);
		return;
	}
	if (q->len >= TP_EVENT_QUEUE_MAX) {
		tam_log_warn("event queue full (%d); dropping %s", TP_EVENT_QUEUE_MAX,
					 name ? name : "event");
		return;
	}

	if (q->len == q->cap) {
		size_t next_cap = q->cap ? q->cap * 2 : 16;
		if (next_cap > TP_EVENT_QUEUE_MAX)
			next_cap = TP_EVENT_QUEUE_MAX;
		struct tp_queued_event *grown = malloc(next_cap * sizeof(*grown));
		if (!grown) {
			tam_log_error("event queue: out of memory");
			return;
		}
		for (size_t i = 0; i < q->len; i++)
			grown[i] = q->items[(q->head + i) % q->cap];
		free(q->items);
		q->items = grown;
		q->head = 0;
		q->cap = next_cap;
	}

	q->items[(q->head + q->len) % q->cap] =
		(struct tp_queued_event){.ev = *ev, .depth = depth};
	q->len++;

	/* While hooks run, drain_events schedules the next batch itself. */
	if (!q->draining && !q->idle)
		q->idle = wl_event_loop_add_idle(s->loop, drain_events_idle, s);
}

void tp_events_emit_window(struct tp_server *s, enum tp_event_kind kind,
						   const struct tp_client *c, bool focused) {
	if (!event_has_listeners(s, kind))
		return;

	struct tp_event ev = {.kind = kind};
	ev.window.focused = focused;
	if (c) {
		if (c->toplevel) {
			copy_str(ev.window.app_id, sizeof(ev.window.app_id),
					 c->toplevel->app_id);
			copy_str(ev.window.title, sizeof(ev.window.title),
					 c->toplevel->title);
		}
		if (c->mon && c->mon->output)
			copy_str(ev.window.monitor, sizeof(ev.window.monitor),
					 c->mon->output->name);
		ev.window.tags = c->tags;
		ev.window.floating = c->floating;
		ev.window.fullscreen = c->fullscreen;
		ev.window.geom = c->geom;
	} else if (s->selmon && s->selmon->output) {
		copy_str(ev.window.monitor, sizeof(ev.window.monitor),
				 s->selmon->output->name);
	}
	tp_events_emit(s, &ev);
}

void tp_events_emit_tag(struct tp_server *s, const struct tp_monitor *m,
						tp_tags_t prev_tags, tp_tags_t new_tags) {
	if (!event_has_listeners(s, TP_EVENT_TAG_CHANGE))
		return;

	struct tp_event ev = {.kind = TP_EVENT_TAG_CHANGE};
	if (m && m->output)
		copy_str(ev.tag.monitor, sizeof(ev.tag.monitor), m->output->name);
	ev.tag.tags = new_tags;
	ev.tag.prev_tags = prev_tags;
	tp_events_emit(s, &ev);
}

void tp_events_emit_monitor(struct tp_server *s, enum tp_event_kind kind,
							const char *name, struct tp_rect area,
							float scale) {
	if (!event_has_listeners(s, kind))
		return;

	struct tp_event ev = {.kind = kind};
	copy_str(ev.monitor.name, sizeof(ev.monitor.name), name);
	ev.monitor.area = area;
	ev.monitor.scale = scale;
	tp_events_emit(s, &ev);
}
