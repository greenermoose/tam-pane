/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/focus.h"
#include "tam-pane/client.h"
#include "tam-pane/foreign_toplevel.h"
#include "tam-pane/layer.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/layout/layout.h"
#include "tam-pane/monitor.h"
#include "tam-pane/security.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/text_input.h"
#include "tam-pane/xwayland.h"
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

static void enter_keyboard(struct tp_server *s, struct wlr_surface *surface) {
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, surface, kb->keycodes,
									   kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
}

static struct tp_layer_surface *
exclusive_on_monitor(struct tp_server *s, struct tp_monitor *m,
					 enum zwlr_layer_shell_v1_layer l) {
	struct tp_layer_surface *ls;
	wl_list_for_each(ls, &m->layers, link) {
		if (!ls->mapped || ls->layer != (uint32_t)l ||
			ls->layer_surface->current.keyboard_interactive !=
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
			continue;
		if (l == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
			struct wl_client *cl =
				wl_resource_get_client(ls->layer_surface->resource);
			if (!tp_security_allow_exclusive_overlay(s, cl))
				continue;
		}
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
				exclusive_on_monitor(s, s->selmon, order[i]);
			if (ls)
				return ls;
		}
		struct tp_monitor *m;
		wl_list_for_each(m, &s->monitors, link) {
			if (m == s->selmon)
				continue;
			struct tp_layer_surface *ls =
				exclusive_on_monitor(s, m, order[i]);
			if (ls)
				return ls;
		}
	}
	return NULL;
}

struct tp_client *tp_focused(struct tp_server *s) {
	return s ? s->focused_client : NULL;
}

void tp_focus_layer(struct tp_server *s, struct tp_layer_surface *ls) {
	if (!ls || !ls->mapped || s->locked)
		return;
	struct tp_client *old = s->focused_client;
	if (old) {
		tp_client_set_activated(old, false);
		tp_foreign_toplevel_set_activated(old, false);
	}
	s->focused_client = NULL;
	s->focused_layer = ls;
	if (ls->mon)
		s->selmon = ls->mon;
	enter_keyboard(s, ls->layer_surface->surface);
	tp_text_input_set_focus(s, ls->layer_surface->surface);
	if (old)
		tp_events_emit_window(s, TP_EVENT_WINDOW_FOCUS, NULL, false);
}

void tp_focus(struct tp_server *s, struct tp_client *c) {
	if (s->locked)
		return;
	struct tp_layer_surface *excl = exclusive_layer(s);
	if (excl) {
		if (s->focused_layer != excl)
			tp_focus_layer(s, excl);
		return;
	}

	struct tp_client *old = s->focused_client;
	if (old && old != c) {
		tp_client_set_activated(old, false);
		tp_foreign_toplevel_set_activated(old, false);
	}
	s->focused_layer = NULL;
	s->focused_client = c;

	if (!c) {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
		tp_text_input_set_focus(s, NULL);
		if (old)
			tp_events_emit_window(s, TP_EVENT_WINDOW_FOCUS, NULL, false);
		return;
	}

	wl_list_remove(&c->flink);
	wl_list_insert(&s->focus_stack, &c->flink);
	if (c->tree)
		wlr_scene_node_raise_to_top(&c->tree->node);
	tp_client_set_activated(c, true);
	tp_foreign_toplevel_set_activated(c, true);
	if (c->mon)
		s->selmon = c->mon;
	if (c->mon && tp_client_visible(c) && !c->scratchpad)
		c->mon->focus[tp_tags_slot(tp_tagview_current(&c->mon->view))] = c;
	if (c->mon && !c->floating && !c->fullscreen && !c->scratchpad) {
		enum tp_layout_kind kind = tp_monitor_layout(c->mon)->kind;
		if (kind == TP_LAYOUT_SCROLLER || kind == TP_LAYOUT_MONOCLE)
			tp_arrange(c->mon);
	}

	struct wlr_surface *surf = tp_client_surface(c);
	if (surf && (!tp_client_is_xwayland(c) || tp_xwayland_accepts_keyboard(c))) {
		enter_keyboard(s, surf);
		tp_text_input_set_focus(s, surf);
	} else {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
		tp_text_input_set_focus(s, NULL);
	}
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
	if (s->locked)
		return;
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

void tp_focus_dir(struct tp_server *s, enum tp_dir dir) {
	struct tp_client *cur = tp_focused(s);
	if (!cur || !cur->mon) {
		struct tp_monitor *adj = tp_monitor_in_dir(s, s->selmon, dir);
		if (adj)
			tp_monitor_focus(s, adj);
		return;
	}
	int64_t cx = cur->geom.x + cur->geom.w / 2;
	int64_t cy = cur->geom.y + cur->geom.h / 2;
	struct tp_client *best = NULL;
	int64_t best_score = 0;

	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) {
		if (c == cur || c->mon != cur->mon || !tp_client_visible(c))
			continue;
		int64_t dx = (c->geom.x + c->geom.w / 2) - cx;
		int64_t dy = (c->geom.y + c->geom.h / 2) - cy;
		int64_t primary = 0, cross = 0;
		switch (dir) {
		case TP_DIR_LEFT:
			primary = -dx;
			cross = dy < 0 ? -dy : dy;
			break;
		case TP_DIR_RIGHT:
			primary = dx;
			cross = dy < 0 ? -dy : dy;
			break;
		case TP_DIR_UP:
			primary = -dy;
			cross = dx < 0 ? -dx : dx;
			break;
		case TP_DIR_DOWN:
			primary = dy;
			cross = dx < 0 ? -dx : dx;
			break;
		}
		if (primary <= 0)
			continue;
		int64_t score = primary + cross * 2;
		if (!best || score < best_score) {
			best = c;
			best_score = score;
		}
	}
	if (best) {
		tp_focus(s, best);
		return;
	}
	struct tp_monitor *adj = tp_monitor_in_dir(s, cur->mon, dir);
	if (adj)
		tp_monitor_focus(s, adj);
}
