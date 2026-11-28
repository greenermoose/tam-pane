/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/xdg_activation.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/visuals.h"
#include "tam-pane/workspace.h"
#include "tam-pane/xwayland.h"
#include <tam.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

static void on_request_activate(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *ev = data;
	if (!s || s->locked || s->stopping || !ev || !ev->surface)
		return;

	struct tp_client *c = NULL;
	struct wlr_xdg_surface *xs =
		wlr_xdg_surface_try_from_wlr_surface(ev->surface);
	if (xs && xs->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xs->data) {
		struct wlr_scene_tree *tree = xs->data;
		c = tree->node.data;
	} else {
		c = tp_xwayland_client_from_surface(ev->surface);
	}
	if (!c || !c->mapped)
		return;

	if (c->mon && !tp_client_visible(c)) {
		unsigned int first = tp_tags_first(c->tags);
		if (first > 0) {
			tp_tags_t prev = tp_tagview_current(&c->mon->view);
			if (tp_tagview_show(&c->mon->view, tp_tag_mask(first))) {
				tp_visuals_notify_tag_change(
					s, c->mon, prev, tp_tagview_current(&c->mon->view));
				tp_arrange(c->mon);
				tp_events_emit_tag(s, c->mon, prev,
								   tp_tagview_current(&c->mon->view));
				tp_workspace_sync_monitor(c->mon);
			}
		}
	}
	tp_focus(s, c);
}

bool tp_xdg_activation_init(struct tp_server *s) {
	s->xdg_activation = wlr_xdg_activation_v1_create(s->display);
	if (!s->xdg_activation) {
		tam_log_error("cannot create xdg_activation_v1");
		return false;
	}
	s->request_activate.notify = on_request_activate;
	wl_signal_add(&s->xdg_activation->events.request_activate,
				  &s->request_activate);
	return true;
}

void tp_xdg_activation_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->request_activate.notify) {
		wl_list_remove(&s->request_activate.link);
		s->request_activate.notify = NULL;
	}
}
