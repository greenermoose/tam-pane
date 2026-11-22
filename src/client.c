/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/input.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

/* ---- popups: menus and tooltips ride on their parent's scene tree ---- */

struct tp_popup {
	struct wlr_xdg_popup *popup;
	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener destroy;
};

/* The window a popup belongs to: follow parent popups up to the root.
 * NULL when the root is not one of our toplevels (a layer surface). */
static struct tp_client *popup_client(struct wlr_xdg_popup *popup) {
	struct wlr_xdg_surface *xs = NULL;
	while (popup && popup->parent) {
		xs = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
		if (!xs || xs->role != WLR_XDG_SURFACE_ROLE_POPUP)
			break;
		popup = xs->popup;
	}
	if (!xs || xs->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL || !xs->data)
		return NULL;
	struct wlr_scene_tree *tree = xs->data;
	return tree->node.data;
}

/* Keep a new popup on its window's monitor. The box is in the root
 * toplevel's surface coordinates: the scene places the window's
 * geometry at c->geom, so its surface origin is c->geom minus the
 * geometry offset. A fullscreen window covers the bars, so it gets the
 * whole monitor. */
static void popup_unconstrain(struct wlr_xdg_popup *popup) {
	struct tp_client *c = popup_client(popup);
	if (!c || !c->mon)
		return;
	struct tp_rect r = c->fullscreen ? c->mon->area : c->mon->usable;
	struct wlr_box g = c->toplevel->base->geometry;
	struct wlr_box box = {
		.x = r.x - (c->geom.x - g.x),
		.y = r.y - (c->geom.y - g.y),
		.width = r.w,
		.height = r.h,
	};
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
	struct wlr_box got = popup->scheduled.geometry;
	tam_log_debug("popup on %s: box %d,%d %dx%d, placed %d,%d %dx%d",
				  c->mon->output->name, box.x, box.y, box.width, box.height,
				  got.x, got.y, got.width, got.height);
}

static void popup_commit(struct wl_listener *l, void *data) {
	struct tp_popup *p = wl_container_of(l, p, commit);
	if (p->popup->base->initial_commit) {
		popup_unconstrain(p->popup);
		wlr_xdg_surface_schedule_configure(p->popup->base);
	}
}

/* xdg_popup.reposition gives new rules; wlroots configures them on the
 * next idle, so unconstrain them first. GTK 4 repositions a popover once
 * it knows its size (B7). */
static void popup_reposition(struct wl_listener *l, void *data) {
	struct tp_popup *p = wl_container_of(l, p, reposition);
	popup_unconstrain(p->popup);
}

static void popup_destroy(struct wl_listener *l, void *data) {
	struct tp_popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->reposition.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void on_new_popup(struct wl_listener *l, void *data) {
	struct wlr_xdg_popup *popup = data;
	if (!popup->parent)
		return; /* layer-shell attaches its own popups in src/layer.c */
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (!parent || !parent->data)
		return;

	struct tp_popup *p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->popup = popup;
	popup->base->data = wlr_scene_xdg_surface_create(parent->data, popup->base);
	p->commit.notify = popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->reposition.notify = popup_reposition;
	wl_signal_add(&popup->events.reposition, &p->reposition);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
}

/* ---- toplevels ---- */

/* Give a floating window with no usable size half its monitor, at least
 * the minimum. Returns whether it changed the size. */
static void half_monitor_size(struct tp_client *c) {
	int32_t w = c->mon->usable.w / 2;
	int32_t h = c->mon->usable.h / 2;
	c->float_geom.w = w < TP_FLOAT_MIN_W ? TP_FLOAT_MIN_W : w;
	c->float_geom.h = h < TP_FLOAT_MIN_H ? TP_FLOAT_MIN_H : h;
}

static bool default_float_size(struct tp_client *c) {
	if (c->float_geom.w >= TP_FLOAT_MIN_W && c->float_geom.h >= TP_FLOAT_MIN_H)
		return false;
	half_monitor_size(c);
	return true;
}

static bool float_on_monitor(const struct tp_client *c) {
	struct tp_rect r = c->float_geom, u = c->mon->usable;
	return r.x < u.x + u.w && r.x + r.w > u.x && r.y < u.y + u.h &&
		   r.y + r.h > u.y;
}

/* Centre the floating place on the window's monitor. */
static void center_float(struct tp_client *c) {
	c->float_geom.x = c->mon->usable.x + (c->mon->usable.w - c->float_geom.w) / 2;
	c->float_geom.y = c->mon->usable.y + (c->mon->usable.h - c->float_geom.h) / 2;
}

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

/* Set the fullscreen flag, tell the client, and move the window to the
 * matching layer. The caller arranges. */
static void apply_fullscreen(struct tp_client *c, bool fullscreen) {
	c->fullscreen = fullscreen;
	wlr_xdg_toplevel_set_fullscreen(c->toplevel, fullscreen);
	enum tp_layer layer = fullscreen	? TP_LAYER_FULLSCREEN
						  : c->floating ? TP_LAYER_FLOAT
										: TP_LAYER_TILE;
	wlr_scene_node_reparent(&c->tree->node, c->server->layer[layer]);
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

	struct tp_rule_result rule = {0};
	if (s->lua) {
		tp_lua_eval_rules(s->lua, c->toplevel->app_id, c->toplevel->title,
						  &rule);
	}

	c->mapped = true;
	struct tp_monitor *mon = s->selmon;
	if (rule.has_monitor) {
		struct tp_monitor *target = tp_monitor_by_name(s, rule.monitor);
		if (target)
			mon = target;
		else
			tam_log_warn(
				"window rule: monitor '%s' not found, using active monitor",
				rule.monitor);
	}
	tp_client_attach(c, mon, 0);

	if (rule.has_tags)
		c->tags = rule.tags;
	else
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
	if (rule.has_floating)
		c->floating = rule.floating;

	if (rule.has_size) {
		c->float_geom.w = rule.width;
		c->float_geom.h = rule.height;
	} else if (c->floating && c->mon) {
		default_float_size(c);
	} else if (c->mon) {
		/* Where Meta+V first floats a window that opened tiled: half its
		 * monitor, centred (B9, Fred's choice). */
		half_monitor_size(c);
	}

	if (c->mon) {
		if (rule.pos_mode == TP_RULE_POS_COORDS) {
			c->float_geom.x = c->mon->usable.x + rule.x;
			c->float_geom.y = c->mon->usable.y + rule.y;
		} else {
			center_float(c);
		}
	}
	apply_floating(c);
	/* A program may ask for fullscreen before it maps; on_unmap cleared
	 * the flag, so a remap follows the client's current request too. */
	if (c->toplevel->requested.fullscreen)
		apply_fullscreen(c, true);

	bool focus_now = tp_client_visible(c) && c->mon == s->selmon;
	wl_list_insert(&s->clients, &c->link);
	if (focus_now)
		wl_list_insert(&s->focus_stack, &c->flink);
	else
		wl_list_insert(s->focus_stack.prev, &c->flink);
	if (c->mon)
		tp_arrange(c->mon);
	tp_events_emit_window(s, TP_EVENT_WINDOW_OPEN, c, false);
	if (focus_now)
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
	c->fullscreen = false;
	tp_pointer_cancel_grab(s, c);
	tp_client_detach(c);
	wl_list_remove(&c->link);
	wl_list_remove(&c->flink);
	if (c->mon)
		tp_arrange(c->mon);
	tp_events_emit_window(s, TP_EVENT_WINDOW_CLOSE, c, false);
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
	free(c->home_output);
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
	tp_pointer_cancel_grab(c->server, c);
	c->floating = floating;
	if (floating && c->mon) {
		if (!float_on_monitor(c)) {
			/* A window that mapped with no monitor has no floating place
			 * yet: treat it as a first float (B9). */
			half_monitor_size(c);
			center_float(c);
		} else if (default_float_size(c)) {
			center_float(c);
		}
	}
	apply_floating(c);
	if (c->mon)
		tp_arrange(c->mon);
}

void tp_client_set_fullscreen(struct tp_client *c, bool fullscreen) {
	tp_pointer_cancel_grab(c->server, c);
	apply_fullscreen(c, fullscreen);
	if (c->mon)
		tp_arrange(c->mon);
}

void tp_client_resize_floating(struct tp_client *c, enum tp_dir dir,
							   int32_t delta) {
	if (!c || !c->floating || c->fullscreen || !delta)
		return;
	struct tp_rect r = c->float_geom;
	switch (dir) {
	case TP_DIR_RIGHT:
		r.w += delta;
		break;
	case TP_DIR_LEFT:
		r.w -= delta;
		break;
	case TP_DIR_DOWN:
		r.h += delta;
		break;
	case TP_DIR_UP:
		r.h -= delta;
		break;
	}
	if (r.w < TP_FLOAT_MIN_W)
		r.w = TP_FLOAT_MIN_W;
	if (r.h < TP_FLOAT_MIN_H)
		r.h = TP_FLOAT_MIN_H;
	c->float_geom = r;
	tp_client_place(c, c->float_geom);
}

void tp_client_set_monitor(struct tp_client *c, struct tp_monitor *m,
						   bool follow) {
	if (!c || !m || c->mon == m)
		return;
	struct tp_server *s = c->server;
	struct tp_monitor *old = c->mon;
	tp_pointer_cancel_grab(s, c);

	if (old)
		tp_client_carry_float(c, old->usable, m->usable);
	tp_client_attach(c, m, tp_tagview_current(&m->view));
	tp_client_clear_home(c);

	if (old)
		tp_arrange(old);
	tp_arrange(m);
	if (follow) {
		tp_monitor_focus(s, m);
		tp_focus(s, c);
	} else {
		tp_focus_refresh(s);
	}
}

void tp_client_detach(struct tp_client *c) {
	struct tp_monitor *m = c->mon;
	if (!m)
		return;
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++)
		if (m->focus[i] == c)
			m->focus[i] = NULL;
	tp_monitor_forget(m, c);
}

void tp_client_attach(struct tp_client *c, struct tp_monitor *m,
					  tp_tags_t tags) {
	tp_client_detach(c);
	c->mon = m;
	if (tags & TP_TAG_ALL)
		c->tags = tags & TP_TAG_ALL;
}

void tp_client_carry_float(struct tp_client *c, struct tp_rect from,
						   struct tp_rect to) {
	/* Tiled windows too: their floating place is where Meta+V puts them,
	 * and it must stay on their monitor (B9). */
	if (to.w <= 0 || to.h <= 0)
		return;
	struct tp_rect r = c->float_geom;
	if (r.w > to.w)
		r.w = to.w < TP_FLOAT_MIN_W ? TP_FLOAT_MIN_W : to.w;
	if (r.h > to.h)
		r.h = to.h < TP_FLOAT_MIN_H ? TP_FLOAT_MIN_H : to.h;
	r.x = to.x + (r.x - from.x);
	r.y = to.y + (r.y - from.y);
	if (r.x < to.x)
		r.x = to.x;
	if (r.y < to.y)
		r.y = to.y;
	if (r.x + r.w > to.x + to.w)
		r.x = to.x + to.w - r.w;
	if (r.y + r.h > to.y + to.h)
		r.y = to.y + to.h - r.h;
	c->float_geom = r;
}

void tp_client_clear_home(struct tp_client *c) {
	free(c->home_output);
	c->home_output = NULL;
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
