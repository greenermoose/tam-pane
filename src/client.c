/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

/* ---- popups: menus and tooltips ride on their parent's scene tree ---- */

struct tp_popup {
	struct wlr_xdg_popup *popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void popup_commit(struct wl_listener *l, void *data) {
	struct tp_popup *p = wl_container_of(l, p, commit);
	if (p->popup->base->initial_commit)
		wlr_xdg_surface_schedule_configure(p->popup->base);
}

static void popup_destroy(struct wl_listener *l, void *data) {
	struct tp_popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void on_new_popup(struct wl_listener *l, void *data) {
	struct wlr_xdg_popup *popup = data;
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (!parent || !parent->data)
		return; /* layer-shell popups arrive with layer-shell in 0.0.5 */

	struct tp_popup *p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->popup = popup;
	popup->base->data = wlr_scene_xdg_surface_create(parent->data, popup->base);
	p->commit.notify = popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
}

/* ---- toplevels ---- */

/* Put the window in its layer and tell it whether its edges are tiled, so
 * it draws square corners and no shadows when tiled. */
static void apply_floating(struct tp_client *c) {
	const uint32_t all = WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
						 WLR_EDGE_RIGHT;
	wlr_scene_node_reparent(
		&c->tree->node,
		c->server->layer[c->floating ? TP_LAYER_FLOAT : TP_LAYER_TILE]);
	wlr_xdg_toplevel_set_tiled(c->toplevel, c->floating ? 0 : all);
}

static void on_commit(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, commit);
	/* The first commit asks for a configure; 0x0 lets the client choose
	 * until the layout places it. */
	if (c->toplevel->base->initial_commit)
		wlr_xdg_toplevel_set_size(c->toplevel, 0, 0);
}

static void on_map(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, map);
	struct tp_server *s = c->server;

	c->mapped = true;
	c->mon = s->selmon;
	c->tags = c->mon ? tp_tagview_current(&c->mon->view) : tp_tag_mask(1);
	struct wlr_box g = c->toplevel->base->geometry;
	c->geom = c->float_geom = (struct tp_rect){0, 0, g.width, g.height};
	/* Dialogs and fixed-size windows float, as in every tiling manager. */
	c->floating = c->toplevel->parent != NULL ||
				  (c->toplevel->current.min_width > 0 &&
				   c->toplevel->current.min_width ==
					   c->toplevel->current.max_width &&
				   c->toplevel->current.min_height ==
					   c->toplevel->current.max_height);
	if (c->floating && c->mon) {
		c->float_geom.x = c->mon->area.x + (c->mon->area.w - g.width) / 2;
		c->float_geom.y = c->mon->area.y + (c->mon->area.h - g.height) / 2;
	}
	apply_floating(c);

	wl_list_insert(&s->clients, &c->link);
	wl_list_insert(&s->focus_stack, &c->flink);
	if (c->mon)
		tp_arrange(c->mon);
	tp_focus(s, c);
	tam_log_debug("window mapped: %s, tags 0x%x, %s",
				  c->toplevel->app_id ? c->toplevel->app_id : "(no app id)",
				  c->tags, c->floating ? "floating" : "tiled");
}

static void on_unmap(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, unmap);
	struct tp_server *s = c->server;
	bool had_focus = tp_focused(s) == c;

	c->mapped = false;
	if (c->mon)
		for (unsigned int i = 0; i < TP_TAG_SLOTS; i++)
			if (c->mon->focus[i] == c)
				c->mon->focus[i] = NULL;
	wl_list_remove(&c->link);
	wl_list_remove(&c->flink);
	if (c->mon) {
		tp_monitor_forget(c->mon, c);
		tp_arrange(c->mon);
	}
	if (had_focus)
		tp_focus_refresh(s);
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, destroy);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->commit.link);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->request_fullscreen.link);
	wl_list_remove(&c->request_maximize.link);
	free(c);
}

static void on_request_fullscreen(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, request_fullscreen);
	if (c->mapped)
		tp_client_set_fullscreen(c, c->toplevel->requested.fullscreen);
	else if (c->toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(c->toplevel->base);
}

static void on_request_maximize(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, request_maximize);
	/* Tiled windows have no maximized state; the protocol still wants an
	 * answer. */
	if (c->toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(c->toplevel->base);
}

static void on_new_toplevel(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct tp_client *c = calloc(1, sizeof(*c));
	if (!c) {
		tam_log_error("new window: out of memory");
		return;
	}
	c->server = s;
	c->toplevel = toplevel;
	c->tree = wlr_scene_xdg_surface_create(s->layer[TP_LAYER_TILE],
										   toplevel->base);
	c->tree->node.data = c;
	toplevel->base->data = c->tree;
	wl_list_init(&c->link);
	wl_list_init(&c->flink);

	struct wlr_surface *surface = toplevel->base->surface;
	c->map.notify = on_map;
	wl_signal_add(&surface->events.map, &c->map);
	c->unmap.notify = on_unmap;
	wl_signal_add(&surface->events.unmap, &c->unmap);
	c->commit.notify = on_commit;
	wl_signal_add(&surface->events.commit, &c->commit);
	c->destroy.notify = on_destroy;
	wl_signal_add(&toplevel->events.destroy, &c->destroy);
	c->request_fullscreen.notify = on_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &c->request_fullscreen);
	c->request_maximize.notify = on_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize, &c->request_maximize);
}

void tp_clients_init(struct tp_server *s) {
	s->new_toplevel.notify = on_new_toplevel;
	wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_toplevel);
	s->new_popup.notify = on_new_popup;
	wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_popup);
}

void tp_clients_finish(struct tp_server *s) {
	if (s->new_toplevel.notify)
		wl_list_remove(&s->new_toplevel.link);
	if (s->new_popup.notify)
		wl_list_remove(&s->new_popup.link);
	s->new_toplevel.notify = s->new_popup.notify = NULL;
}

/* ---- operations ---- */

bool tp_client_visible(const struct tp_client *c) {
	return c->mapped && c->mon && tp_tags_visible(c->tags, &c->mon->view);
}

void tp_client_place(struct tp_client *c, struct tp_rect box) {
	bool resized = box.w != c->geom.w || box.h != c->geom.h;
	c->geom = box;
	wlr_scene_node_set_position(&c->tree->node, box.x, box.y);
	/* Each size request costs the client a configure and a redraw, so
	 * send one only when the size really changes. */
	if (resized)
		wlr_xdg_toplevel_set_size(c->toplevel, box.w, box.h);
}

void tp_client_close(struct tp_client *c) {
	wlr_xdg_toplevel_send_close(c->toplevel);
}

void tp_client_toggle_tags(struct tp_client *c, tp_tags_t mask) {
	tp_client_set_tags(c, c->tags ^ (mask & TP_TAG_ALL));
}

void tp_client_set_tags(struct tp_client *c, tp_tags_t tags) {
	tags &= TP_TAG_ALL;
	if (!tags || tags == c->tags)
		return;
	c->tags = tags;
	if (c->mon) {
		for (unsigned int i = 1; i < TP_TAG_SLOTS; i++)
			if (!(tags & tp_tag_mask(i)) && c->mon->focus[i] == c)
				c->mon->focus[i] = NULL;
		if (!tp_client_visible(c) && c->mon->focus[0] == c)
			c->mon->focus[0] = NULL;
	}
	if (c->mon)
		tp_arrange(c->mon);
	tp_focus_refresh(c->server);
}

void tp_client_set_floating(struct tp_client *c, bool floating) {
	if (c->floating == floating || c->fullscreen)
		return;
	c->floating = floating;
	if (floating)
		c->geom = c->float_geom;
	apply_floating(c);
	if (c->mon)
		tp_arrange(c->mon);
}

void tp_client_set_fullscreen(struct tp_client *c, bool fullscreen) {
	c->fullscreen = fullscreen;
	wlr_xdg_toplevel_set_fullscreen(c->toplevel, fullscreen);
	enum tp_layer layer = fullscreen	? TP_LAYER_FULLSCREEN
						  : c->floating ? TP_LAYER_FLOAT
										: TP_LAYER_TILE;
	wlr_scene_node_reparent(&c->tree->node, c->server->layer[layer]);
	if (c->mon)
		tp_arrange(c->mon);
}

struct tp_client *tp_client_at(struct tp_server *s, double x, double y,
							   struct wlr_surface **surface, double *sx,
							   double *sy) {
	double lx, ly;
	struct wlr_scene_node *node =
		wlr_scene_node_at(&s->scene->tree.node, x, y, &lx, &ly);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	struct wlr_scene_surface *ss =
		wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	if (!ss)
		return NULL;
	if (surface)
		*surface = ss->surface;
	if (sx)
		*sx = lx;
	if (sy)
		*sy = ly;
	/* Walk up to the toplevel's tree, which carries the client. */
	for (struct wlr_scene_tree *t = node->parent; t; t = t->node.parent)
		if (t->node.data)
			return t->node.data;
	return NULL;
}
