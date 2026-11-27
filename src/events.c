/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/server_events.h"
#include "tam-pane/client.h"
#include "tam-pane/ipc.h"
#include "tam-pane/layout/arrange.h"
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
	if (c && c->mon && c->mon->arrange_dirty && kind != TP_EVENT_WINDOW_CLOSE &&
		kind != TP_EVENT_WINDOW_CHANGE)
		tp_arrange_flush(s);

	struct tp_event ev = {.kind = kind};
	ev.window.focused = focused;
	if (c) {
		copy_str(ev.window.app_id, sizeof(ev.window.app_id),
				 tp_client_app_id(c));
		copy_str(ev.window.title, sizeof(ev.window.title), tp_client_title(c));
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

void tp_events_emit_monitor_focus(struct tp_server *s, const char *name) {
	if (!event_has_listeners(s, TP_EVENT_MONITOR_FOCUS))
		return;
	struct tp_event ev = {.kind = TP_EVENT_MONITOR_FOCUS};
	copy_str(ev.monitor.name, sizeof(ev.monitor.name), name);
	tp_events_emit(s, &ev);
}

void tp_events_emit_output_power(struct tp_server *s, const char *monitor,
								 bool dpms) {
	if (!event_has_listeners(s, TP_EVENT_OUTPUT_POWER))
		return;
	struct tp_event ev = {.kind = TP_EVENT_OUTPUT_POWER};
	copy_str(ev.output_power.monitor, sizeof(ev.output_power.monitor), monitor);
	ev.output_power.dpms = dpms;
	tp_events_emit(s, &ev);
}

void tp_events_emit_keyboard_layout(struct tp_server *s, const char *name,
									const char *active_keymap,
									uint32_t active_layout_index) {
	if (!event_has_listeners(s, TP_EVENT_KEYBOARD_LAYOUT))
		return;
	struct tp_event ev = {.kind = TP_EVENT_KEYBOARD_LAYOUT};
	copy_str(ev.keyboard.name, sizeof(ev.keyboard.name), name);
	copy_str(ev.keyboard.active_keymap, sizeof(ev.keyboard.active_keymap),
			 active_keymap);
	ev.keyboard.active_layout_index = active_layout_index;
	tp_events_emit(s, &ev);
}

void tp_events_emit_config_reload(struct tp_server *s) {
	if (!event_has_listeners(s, TP_EVENT_CONFIG_RELOAD))
		return;
	struct tp_event ev = {.kind = TP_EVENT_CONFIG_RELOAD};
	tp_events_emit(s, &ev);
}

void tp_events_emit_chord(struct tp_server *s, enum tp_event_kind kind,
						  const char *result, const char *action) {
	if (!event_has_listeners(s, kind))
		return;

	struct tp_event ev = {.kind = kind};
	copy_str(ev.chord.mode, sizeof(ev.chord.mode),
			 tp_chord_mode_name(s->chord.mode));
	tp_chord_format(s->chord.mods, s->chord.sym, ev.chord.chord,
					sizeof(ev.chord.chord));
	copy_str(ev.chord.filter, sizeof(ev.chord.filter), s->chord.filter);
	copy_str(ev.chord.result, sizeof(ev.chord.result), result);
	copy_str(ev.chord.action, sizeof(ev.chord.action), action);
	ev.chord.matches = s->chord.filtered_len;
	ev.chord.selected = s->chord.selected;
	tp_events_emit(s, &ev);
}

void tp_events_emit_session_lock(struct tp_server *s, bool locked) {
	enum tp_event_kind kind =
		locked ? TP_EVENT_SESSION_LOCK : TP_EVENT_SESSION_UNLOCK;
	if (!event_has_listeners(s, kind))
		return;
	struct tp_event ev = {.kind = kind};
	ev.session_lock.locked = locked;
	tp_events_emit(s, &ev);
}

void tp_events_emit_gamma(struct tp_server *s, uint32_t temperature,
						  bool enabled) {
	if (!event_has_listeners(s, TP_EVENT_GAMMA_CHANGE))
		return;
	struct tp_event ev = {.kind = TP_EVENT_GAMMA_CHANGE};
	ev.gamma.temperature = temperature;
	ev.gamma.enabled = enabled;
	tp_events_emit(s, &ev);
}
