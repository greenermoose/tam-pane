/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/focus.h"
#include "tam-pane/client.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

struct tp_client *tp_focused(struct tp_server *s) {
	struct wlr_surface *surface = s->seat->keyboard_state.focused_surface;
	if (!surface || wl_list_empty(&s->focus_stack))
		return NULL;
	struct tp_client *top = wl_container_of(s->focus_stack.next, top, flink);
	return top->toplevel->base->surface == surface ? top : NULL;
}

void tp_focus(struct tp_server *s, struct tp_client *c) {
	struct tp_client *old = tp_focused(s);
	if (old && old != c)
		wlr_xdg_toplevel_set_activated(old->toplevel, false);

	if (!c) {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
		return;
	}

	wl_list_remove(&c->flink);
	wl_list_insert(&s->focus_stack, &c->flink);
	wlr_scene_node_raise_to_top(&c->tree->node);
	wlr_xdg_toplevel_set_activated(c->toplevel, true);
	if (c->mon)
		s->selmon = c->mon;

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	struct wlr_surface *surface = c->toplevel->base->surface;
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, surface, kb->keycodes,
									   kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
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
	tp_focus(s, s->selmon ? tp_focus_top(s, s->selmon) : NULL);
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
