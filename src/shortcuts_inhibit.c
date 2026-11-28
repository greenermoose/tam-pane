/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/shortcuts_inhibit.h"
#include "tam-pane/input.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_seat.h>

struct tp_shortcut_inhibitor {
	struct wl_list link; /* tp_server.shortcut_inhibitors */
	struct tp_server *server;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
};

void tp_shortcuts_inhibit_update(struct tp_server *s) {
	if (!s || s->stopping || !s->shortcuts_inhibit_mgr)
		return;

	struct wlr_surface *focused =
		(!s->locked && s->seat) ? s->seat->keyboard_state.focused_surface : NULL;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *want = NULL;

	if (focused) {
		struct tp_shortcut_inhibitor *item;
		wl_list_for_each(item, &s->shortcut_inhibitors, link) {
			if (item->inhibitor->surface == focused &&
				item->inhibitor->seat == s->seat) {
				want = item->inhibitor;
				break;
			}
		}
	}

	if (s->active_shortcut_inhibitor == want)
		return;

	if (s->active_shortcut_inhibitor && s->active_shortcut_inhibitor->active)
		wlr_keyboard_shortcuts_inhibitor_v1_deactivate(
			s->active_shortcut_inhibitor);

	s->active_shortcut_inhibitor = want;

	if (want) {
		tp_keyboard_cancel_pending_releases(s);
		if (!want->active)
			wlr_keyboard_shortcuts_inhibitor_v1_activate(want);
	}
}

bool tp_shortcuts_inhibited(const struct tp_server *s) {
	if (!s || s->locked || !s->seat || !s->active_shortcut_inhibitor)
		return false;
	return s->active_shortcut_inhibitor->active &&
		   s->active_shortcut_inhibitor->surface ==
			   s->seat->keyboard_state.focused_surface;
}

static void on_inhibitor_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct tp_shortcut_inhibitor *item = wl_container_of(l, item, destroy);
	struct tp_server *s = item->server;
	if (s->active_shortcut_inhibitor == item->inhibitor)
		s->active_shortcut_inhibitor = NULL;
	wl_list_remove(&item->destroy.link);
	wl_list_remove(&item->link);
	free(item);
	if (!s->stopping)
		tp_shortcuts_inhibit_update(s);
}

static void on_new_inhibitor(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_shortcut_inhibitor);
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;
	struct tp_shortcut_inhibitor *item = calloc(1, sizeof(*item));
	if (!item)
		return;
	item->server = s;
	item->inhibitor = inhibitor;
	item->destroy.notify = on_inhibitor_destroy;
	wl_signal_add(&inhibitor->events.destroy, &item->destroy);
	wl_list_insert(&s->shortcut_inhibitors, &item->link);
	tp_shortcuts_inhibit_update(s);
}

static void on_kb_focus_change(struct wl_listener *l, void *data) {
	(void)data;
	struct tp_server *s = wl_container_of(l, s, kb_focus_change);
	tp_shortcuts_inhibit_update(s);
}

bool tp_shortcuts_inhibit_init(struct tp_server *s) {
	wl_list_init(&s->shortcut_inhibitors);
	s->active_shortcut_inhibitor = NULL;
	s->shortcuts_inhibit_mgr =
		wlr_keyboard_shortcuts_inhibit_v1_create(s->display);
	if (!s->shortcuts_inhibit_mgr) {
		tam_log_error("cannot create zwp_keyboard_shortcuts_inhibit_manager_v1");
		return false;
	}
	s->new_shortcut_inhibitor.notify = on_new_inhibitor;
	wl_signal_add(&s->shortcuts_inhibit_mgr->events.new_inhibitor,
				  &s->new_shortcut_inhibitor);
	s->kb_focus_change.notify = on_kb_focus_change;
	wl_signal_add(&s->seat->keyboard_state.events.focus_change,
				  &s->kb_focus_change);
	return true;
}

void tp_shortcuts_inhibit_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->kb_focus_change.notify) {
		wl_list_remove(&s->kb_focus_change.link);
		s->kb_focus_change.notify = NULL;
	}
	if (s->new_shortcut_inhibitor.notify) {
		wl_list_remove(&s->new_shortcut_inhibitor.link);
		s->new_shortcut_inhibitor.notify = NULL;
	}
	s->active_shortcut_inhibitor = NULL;
	if (s->shortcut_inhibitors.next) {
		struct tp_shortcut_inhibitor *item, *tmp;
		wl_list_for_each_safe(item, tmp, &s->shortcut_inhibitors, link) {
			wl_list_remove(&item->destroy.link);
			wl_list_remove(&item->link);
			free(item);
		}
	}
}
