/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/idle.h"
#include "tam-pane/client.h"
#include "tam-pane/layer.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>

struct tp_idle_inhibitor {
	struct wl_list link; /* tp_server.idle_inhibitors */
	struct tp_server *server;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	struct wl_listener destroy;
};

static bool surface_visible(struct tp_server *s, struct wlr_surface *surface) {
	if (!surface)
		return false;

	struct wlr_surface *root = surface;
	for (;;) {
		struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(root);
		if (!sub || !sub->parent)
			break;
		root = sub->parent;
	}

	if (s->locked) {
		return wlr_session_lock_surface_v1_try_from_wlr_surface(root) != NULL &&
			   root->mapped;
	}

	struct wlr_xdg_surface *xs = wlr_xdg_surface_try_from_wlr_surface(root);
	while (xs && xs->role == WLR_XDG_SURFACE_ROLE_POPUP && xs->popup &&
		   xs->popup->parent) {
		root = xs->popup->parent;
		xs = wlr_xdg_surface_try_from_wlr_surface(root);
	}
	if (xs && xs->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		struct wlr_scene_tree *tree = xs->data;
		struct tp_client *c = tree ? tree->node.data : NULL;
		return c && tp_client_visible(c);
	}

	struct wlr_layer_surface_v1 *wlr_ls =
		wlr_layer_surface_v1_try_from_wlr_surface(root);
	if (wlr_ls) {
		struct tp_layer_surface *ls = wlr_ls->data;
		return ls && ls->mapped;
	}

	return root->mapped;
}

void tp_idle_update_inhibit(struct tp_server *s) {
	if (!s || !s->idle_notifier || !s->idle_inhibit_mgr)
		return;
	bool inhibited = false;
	struct wlr_idle_inhibitor_v1 *inh;
	wl_list_for_each(inh, &s->idle_inhibit_mgr->inhibitors, link) {
		if (surface_visible(s, inh->surface)) {
			inhibited = true;
			break;
		}
	}
	wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, inhibited);
}

static void on_inhibitor_destroy(struct wl_listener *l, void *data) {
	struct tp_idle_inhibitor *item = wl_container_of(l, item, destroy);
	struct tp_server *s = item->server;
	wl_list_remove(&item->destroy.link);
	wl_list_remove(&item->link);
	free(item);
	if (!s->stopping)
		tp_idle_update_inhibit(s);
}

static void on_new_inhibitor(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *inhibitor = data;
	struct tp_idle_inhibitor *item = calloc(1, sizeof(*item));
	if (!item)
		return;
	item->server = s;
	item->inhibitor = inhibitor;
	item->destroy.notify = on_inhibitor_destroy;
	wl_signal_add(&inhibitor->events.destroy, &item->destroy);
	wl_list_insert(&s->idle_inhibitors, &item->link);
	tp_idle_update_inhibit(s);
}

bool tp_idle_init(struct tp_server *s) {
	wl_list_init(&s->idle_inhibitors);
	s->idle_notifier = wlr_idle_notifier_v1_create(s->display);
	if (!s->idle_notifier) {
		tam_log_error("cannot create ext_idle_notifier_v1");
		return false;
	}
	s->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(s->display);
	if (!s->idle_inhibit_mgr) {
		tam_log_error("cannot create zwp_idle_inhibit_manager_v1");
		return false;
	}
	s->new_idle_inhibitor.notify = on_new_inhibitor;
	wl_signal_add(&s->idle_inhibit_mgr->events.new_inhibitor,
				  &s->new_idle_inhibitor);
	return true;
}

void tp_idle_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->new_idle_inhibitor.notify) {
		wl_list_remove(&s->new_idle_inhibitor.link);
		s->new_idle_inhibitor.notify = NULL;
	}
	struct tp_idle_inhibitor *item, *tmp;
	wl_list_for_each_safe(item, tmp, &s->idle_inhibitors, link) {
		wl_list_remove(&item->destroy.link);
		wl_list_remove(&item->link);
		free(item);
	}
}

void tp_idle_notify_activity(struct tp_server *s) {
	if (!s || !s->idle_notifier || !s->seat)
		return;
	wlr_idle_notifier_v1_notify_activity(s->idle_notifier, s->seat);
}
