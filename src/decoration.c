/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/decoration.h"
#include "tam-pane/client.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

struct tp_decoration {
	struct tp_client *client;
	struct wlr_xdg_toplevel_decoration_v1 *wlr;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

static enum wlr_xdg_toplevel_decoration_v1_mode
choose_mode(const struct tp_decoration *d) {
	if (d->client && d->client->floating &&
		d->wlr->requested_mode ==
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
		return WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	return WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

void tp_decoration_update(struct tp_client *c) {
	if (!c || !c->decoration || !c->decoration->wlr)
		return;
	if (!c->toplevel->base->initialized)
		return;
	wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration->wlr,
											choose_mode(c->decoration));
}

void tp_decoration_detach_client(struct tp_client *c) {
	if (!c || !c->decoration)
		return;
	c->decoration->client = NULL;
	c->decoration = NULL;
}

static void on_request_mode(struct wl_listener *l, void *data) {
	struct tp_decoration *d = wl_container_of(l, d, request_mode);
	if (d->client) {
		tp_decoration_update(d->client);
	} else if (d->wlr->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(
			d->wlr, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct tp_decoration *d = wl_container_of(l, d, destroy);
	wl_list_remove(&d->request_mode.link);
	wl_list_remove(&d->destroy.link);
	if (d->client && d->client->decoration == d)
		d->client->decoration = NULL;
	free(d);
}

static void on_new_decoration(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *wlr = data;
	struct tp_decoration *d = calloc(1, sizeof(*d));
	if (!d)
		return;
	d->wlr = wlr;
	if (wlr->toplevel && wlr->toplevel->base && wlr->toplevel->base->data) {
		struct wlr_scene_tree *tree = wlr->toplevel->base->data;
		d->client = tree->node.data;
		if (d->client)
			d->client->decoration = d;
	}
	d->request_mode.notify = on_request_mode;
	wl_signal_add(&wlr->events.request_mode, &d->request_mode);
	d->destroy.notify = on_destroy;
	wl_signal_add(&wlr->events.destroy, &d->destroy);

	if (d->client)
		tp_decoration_update(d->client);
	else if (wlr->toplevel && wlr->toplevel->base &&
			 wlr->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(
			wlr, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

bool tp_decoration_init(struct tp_server *s) {
	s->xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
	if (!s->xdg_decoration_mgr) {
		tam_log_error("cannot create zxdg_decoration_manager_v1");
		return false;
	}
	s->new_xdg_decoration.notify = on_new_decoration;
	wl_signal_add(&s->xdg_decoration_mgr->events.new_toplevel_decoration,
				  &s->new_xdg_decoration);
	return true;
}

void tp_decoration_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->new_xdg_decoration.notify) {
		wl_list_remove(&s->new_xdg_decoration.link);
		s->new_xdg_decoration.notify = NULL;
	}
}
