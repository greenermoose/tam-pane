/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/focus.h"
#include "tam-pane/client.h"
#include "tam-pane/layer.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

static void enter_keyboard(struct tp_server *s, struct wlr_surface *surface) {
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, surface, kb->keycodes,
									   kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
}

static struct tp_layer_surface *
exclusive_on_monitor(struct tp_monitor *m, enum zwlr_layer_shell_v1_layer l) {
	struct tp_layer_surface *ls;
	wl_list_for_each(ls, &m->layers, link) {
		if (ls->mapped && ls->layer == (uint32_t)l &&
			ls->layer_surface->current.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
			return ls;
	}
	return NULL;
}

static struct tp_layer_surface *exclusive_layer(struct tp_server *s) {
	static const enum zwlr_layer_shell_v1_layer order[2] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	for (int i = 0; i < 2; i++) {
		if (s->selmon) {
			struct tp_layer_surface *ls =
				exclusive_on_monitor(s->selmon, order[i]);
			if (ls)
				return ls;
		}
		struct tp_monitor *m;
		wl_list_for_each(m, &s->monitors, link) {
			if (m == s->selmon)
				continue;
			struct tp_layer_surface *ls = exclusive_on_monitor(m, order[i]);
			if (ls)
				return ls;
		}
	}
	return NULL;
}

struct tp_client *tp_focused(struct tp_server *s) {
	struct wlr_surface *surface = s->seat->keyboard_state.focused_surface;
	if (!surface || wl_list_empty(&s->focus_stack))
		return NULL;
	struct tp_client *top = wl_container_of(s->focus_stack.next, top, flink);
	return top->toplevel->base->surface == surface ? top : NULL;
}

void tp_focus_layer(struct tp_server *s, struct tp_layer_surface *ls) {
	if (!ls || !ls->mapped)
		return;
	struct tp_client *old = tp_focused(s);
	if (old)
		wlr_xdg_toplevel_set_activated(old->toplevel, false);
	s->focused_layer = ls;
	if (ls->mon)
		s->selmon = ls->mon;
	enter_keyboard(s, ls->layer_surface->surface);
	if (old)
		tp_events_emit_window(s, TP_EVENT_WINDOW_FOCUS, NULL, false);
}

void tp_focus(struct tp_server *s, struct tp_client *c) {
	struct tp_layer_surface *excl = exclusive_layer(s);
	if (excl) {
		if (s->focused_layer != excl)
			tp_focus_layer(s, excl);
		return;
	}

	struct tp_client *old = tp_focused(s);
	bool had_surface = s->seat->keyboard_state.focused_surface != NULL;
	bool had_layer = s->focused_layer != NULL;
	if (old && old != c)
		wlr_xdg_toplevel_set_activated(old->toplevel, false);
	s->focused_layer = NULL;

	if (!c) {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
		if (old || (had_surface && !had_layer))
			tp_events_emit_window(s, TP_EVENT_WINDOW_FOCUS, NULL, false);
		return;
	}

	wl_list_remove(&c->flink);
	wl_list_insert(&s->focus_stack, &c->flink);
	wlr_scene_node_raise_to_top(&c->tree->node);
	wlr_xdg_toplevel_set_activated(c->toplevel, true);
	if (c->mon)
		s->selmon = c->mon;
	if (c->mon && tp_client_visible(c))
		c->mon->focus[tp_tags_slot(tp_tagview_current(&c->mon->view))] = c;

	enter_keyboard(s, c->toplevel->base->surface);
	if (old != c)
		tp_events_emit_window(s, TP_EVENT_WINDOW_FOCUS, c, true);
}

struct tp_client *tp_focus_top(struct tp_server *s, struct tp_monitor *m) {
	struct tp_client *c;
	wl_list_for_each(c, &s->focus_stack, flink) {
		if (c->mon == m && tp_client_visible(c))
			return c;
	}
	return NULL;
}

void tp_focus_refresh(struct tp_server *s) {
	struct tp_layer_surface *excl = exclusive_layer(s);
	if (excl) {
		tp_focus_layer(s, excl);
		return;
	}
	struct tp_monitor *m = s->selmon;
	struct tp_client *c =
		m ? m->focus[tp_tags_slot(tp_tagview_current(&m->view))] : NULL;
	if (!c || !c->mapped || c->mon != m || !tp_client_visible(c))
		c = m ? tp_focus_top(s, m) : NULL;
	tp_focus(s, c);
}

void tp_focus_cycle(struct tp_server *s, int dir) {
	struct tp_client *cur = tp_focused(s);
	if (!cur)
		return;
	/* Walk the client list in the chosen direction, skipping the list
	 * head and windows that are not visible on this monitor. */
	struct wl_list *pos = &cur->link;
	do {
		pos = dir > 0 ? pos->next : pos->prev;
		if (pos == &s->clients)
			continue;
		struct tp_client *c = wl_container_of(pos, c, link);
		if (c->mon == cur->mon && tp_client_visible(c)) {
			tp_focus(s, c);
			return;
		}
	} while (pos != &cur->link);
}
