/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/client.h"
#include "tam-pane/capture.h"
#include "tam-pane/decoration.h"
#include "tam-pane/focus.h"
#include "tam-pane/foreign_toplevel.h"
#include "tam-pane/idle.h"
#include "tam-pane/input.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/xwayland.h"
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
	if (c->mon->arrange_dirty)
		tp_arrange_flush(c->server);
	struct tp_rect r = c->fullscreen ? c->mon->area : c->mon->usable;
	struct wlr_box g = c->toplevel->base->geometry;
	struct wlr_box box = {
		.x = r.x - (c->content_geom.x - g.x),
		.y = r.y - (c->content_geom.y - g.y),
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
	if (c->tree) {
		wlr_scene_node_reparent(
			&c->tree->node,
			c->server->layer[c->floating ? TP_LAYER_FLOAT : TP_LAYER_TILE]);
	}
	if (c->kind == TP_CLIENT_XDG && c->toplevel) {
		wlr_xdg_toplevel_set_tiled(c->toplevel, c->floating ? 0 : all);
		tp_decoration_update(c);
	}
}

/* Set the fullscreen flag, tell the client, and move the window to the
 * matching layer. The caller arranges. */
static void apply_fullscreen(struct tp_client *c, bool fullscreen) {
	c->fullscreen = fullscreen;
	if (c->kind == TP_CLIENT_XWAYLAND)
		tp_xwayland_set_fullscreen(c, fullscreen);
	else if (c->toplevel)
		wlr_xdg_toplevel_set_fullscreen(c->toplevel, fullscreen);
	tp_foreign_toplevel_set_fullscreen(c, fullscreen);
	enum tp_layer layer = fullscreen	? TP_LAYER_FULLSCREEN
						  : c->floating ? TP_LAYER_FLOAT
										: TP_LAYER_TILE;
	if (c->tree)
		wlr_scene_node_reparent(&c->tree->node, c->server->layer[layer]);
}

void tp_client_apply_map(struct tp_client *c, struct tp_rect initial_geom,
						 bool default_floating, bool request_fullscreen) {
	struct tp_server *s = c->server;
	const char *app_id = tp_client_app_id(c);
	const char *title = tp_client_title(c);

	struct tp_rule_result rule = {0};
	if (s->lua)
		tp_lua_eval_rules(s->lua, app_id, title, &rule);

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

	c->geom = c->float_geom = c->content_geom =
		(struct tp_rect){0, 0, initial_geom.w, initial_geom.h};
	c->floating = default_floating;
	if (rule.has_floating)
		c->floating = rule.floating;
	if (rule.has_scratchpad && rule.scratchpad) {
		c->scratchpad = true;
		c->scratchpad_visible = false;
		c->floating = true;
	}

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
	if (request_fullscreen)
		apply_fullscreen(c, true);

	bool focus_now = tp_client_visible(c) && c->mon == s->selmon;
	wl_list_insert(&s->clients, &c->link);
	if (focus_now)
		wl_list_insert(&s->focus_stack, &c->flink);
	else
		wl_list_insert(s->focus_stack.prev, &c->flink);
	if (c->mon)
		tp_arrange(c->mon);
	tp_foreign_toplevel_map(c);
	tp_events_emit_window(s, TP_EVENT_WINDOW_OPEN, c, false);
	if (focus_now)
		tp_focus(s, c);
	tp_idle_update_inhibit(s);
	tam_log_debug("window mapped: %s (%s), tags 0x%x, %s",
				  app_id ? app_id : "(no app id)",
				  c->kind == TP_CLIENT_XWAYLAND ? "xwayland" : "xdg", c->tags,
				  c->floating ? "floating" : "tiled");
}

void tp_client_apply_unmap(struct tp_client *c) {
	struct tp_server *s = c->server;
	bool had_focus = tp_focused(s) == c;

	c->mapped = false;
	wlr_scene_node_set_enabled(&c->tree->node, false);
	c->fullscreen = false;
	c->scratchpad = false;
	c->scratchpad_visible = false;
	tp_foreign_toplevel_unmap(c);
	if (had_focus)
		s->focused_client = NULL;
	tp_pointer_cancel_grab(s, c);
	tp_client_detach(c);
	tp_monitors_forget_parked(s, c);
	wl_list_remove(&c->link);
	wl_list_remove(&c->flink);
	wl_list_init(&c->link);
	wl_list_init(&c->flink);
	if (c->mon)
		tp_arrange(c->mon);
	tp_idle_update_inhibit(s);
	tp_events_emit_window(s, TP_EVENT_WINDOW_CLOSE, c, false);
	if (had_focus) {
		tp_focus_refresh(s);
		if (!s->focused_client)
			tp_events_emit_window(s, TP_EVENT_WINDOW_FOCUS, NULL, false);
	}
}

static void on_commit(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, commit);
	/* The first commit asks for a configure; 0x0 lets the client choose
	 * until the layout places it. */
	if (c->toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(c->toplevel, 0, 0);
		tp_decoration_update(c);
	}
}

static void on_map(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, map);
	struct wlr_box g = c->toplevel->base->geometry;
	/* Dialogs and fixed-size windows float, as in every tiling manager. */
	bool def_float = c->toplevel->parent != NULL ||
					 (c->toplevel->current.min_width > 0 &&
					  c->toplevel->current.min_width ==
						  c->toplevel->current.max_width &&
					  c->toplevel->current.min_height ==
						  c->toplevel->current.max_height);
	tp_client_apply_map(c, (struct tp_rect){0, 0, g.width, g.height}, def_float,
						c->toplevel->requested.fullscreen);
}

static void on_unmap(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, unmap);
	tp_client_apply_unmap(c);
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, destroy);
	tp_client_destroy_scene(c);
	tp_decoration_detach_client(c);
	tp_foreign_toplevel_unmap(c);
	if (c->server->focused_client == c)
		c->server->focused_client = NULL;
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->commit.link);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->request_fullscreen.link);
	wl_list_remove(&c->request_maximize.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->set_app_id.link);
	free(c->home_output);
	free(c);
}

static void on_set_title(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, set_title);
	tp_foreign_toplevel_update_info(c);
}

static void on_set_app_id(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, set_app_id);
	tp_foreign_toplevel_update_info(c);
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

bool tp_client_create_scene(struct tp_client *c, struct wlr_surface *surface) {
	c->tree = wlr_scene_tree_create(c->server->layer[TP_LAYER_TILE]);
	if (!c->tree)
		return false;
	c->content = surface ? wlr_scene_subsurface_tree_create(c->tree, surface)
						 : wlr_scene_xdg_surface_create(c->tree, c->toplevel->base);
	if (!c->content) {
		tp_client_destroy_scene(c);
		return false;
	}
	c->tree->node.data = c;
	c->content->node.data = c;
	wlr_scene_node_set_enabled(&c->tree->node, false);
	return true;
}

void tp_client_destroy_scene(struct tp_client *c) {
	tp_capture_client_finish(c);
	if (c->tree)
		wlr_scene_node_destroy(&c->tree->node);
	c->tree = c->content = NULL;
	for (size_t i = 0; i < 4; i++)
		c->border[i] = NULL;
}

static void on_new_toplevel(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct tp_client *c = calloc(1, sizeof(*c));
	if (!c) {
		tam_log_error("new window: out of memory");
		return;
	}
	c->id = ++s->next_client_id;
	c->server = s;
	c->toplevel = toplevel;
	if (!tp_client_create_scene(c, NULL)) {
		tam_log_error("new window: cannot create scene tree");
		free(c);
		return;
	}
	toplevel->base->data = c->content;
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
	c->set_title.notify = on_set_title;
	wl_signal_add(&toplevel->events.set_title, &c->set_title);
	c->set_app_id.notify = on_set_app_id;
	wl_signal_add(&toplevel->events.set_app_id, &c->set_app_id);
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
	s->focused_client = NULL;
}

/* ---- operations ---- */

struct wlr_surface *tp_client_surface(const struct tp_client *c) {
	if (!c)
		return NULL;
	if (c->kind == TP_CLIENT_XWAYLAND)
		return tp_xwayland_surface(c);
	return c->toplevel ? c->toplevel->base->surface : NULL;
}

const char *tp_client_app_id(const struct tp_client *c) {
	if (!c)
		return NULL;
	if (c->kind == TP_CLIENT_XWAYLAND)
		return tp_xwayland_app_id(c);
	return c->toplevel ? c->toplevel->app_id : NULL;
}

const char *tp_client_title(const struct tp_client *c) {
	if (!c)
		return NULL;
	if (c->kind == TP_CLIENT_XWAYLAND)
		return tp_xwayland_title(c);
	return c->toplevel ? c->toplevel->title : NULL;
}

int tp_client_pid(const struct tp_client *c) {
	if (!c)
		return 0;
	struct wlr_surface *surf = tp_client_surface(c);
	if (!surf || !surf->resource)
		return 0;
	struct wl_client *cl = wl_resource_get_client(surf->resource);
	if (!cl)
		return 0;
	pid_t pid = 0;
	wl_client_get_credentials(cl, &pid, NULL, NULL);
	return (int)pid;
}

bool tp_client_is_xwayland(const struct tp_client *c) {
	return c && c->kind == TP_CLIENT_XWAYLAND;
}

static int32_t client_border_width(const struct tp_client *c) {
	int32_t width = c->server->settings.border_width;
	if (c->fullscreen || c->geom.w <= 2 * width || c->geom.h <= 2 * width)
		return 0;
	return width;
}

static void update_border(struct tp_client *c, bool active) {
	const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
	int32_t b = client_border_width(c);
	bool show = active && c->tree && b > 0 && tp_client_visible(c);
	struct tp_rect edges[4] = {
		{0, 0, c->geom.w, b}, {0, c->geom.h - b, c->geom.w, b},
		{0, b, b, c->geom.h - 2 * b},
		{c->geom.w - b, b, b, c->geom.h - 2 * b},
	};
	for (size_t i = 0; i < 4; i++) {
		struct tp_rect edge = edges[i];
		if (show && !c->border[i]) {
			c->border[i] = wlr_scene_rect_create(c->tree, edge.w, edge.h, green);
			if (c->border[i])
				wlr_scene_node_lower_to_bottom(&c->border[i]->node);
		}
		if (!c->border[i])
			continue;
		if (show) {
			wlr_scene_rect_set_size(c->border[i], edge.w, edge.h);
			wlr_scene_node_set_position(&c->border[i]->node, edge.x, edge.y);
		}
		wlr_scene_node_set_enabled(&c->border[i]->node, show);
	}
}

void tp_client_set_activated(struct tp_client *c, bool activated) {
	if (!c)
		return;
	update_border(c, activated);
	if (c->kind == TP_CLIENT_XWAYLAND)
		tp_xwayland_set_activated(c, activated);
	else if (c->toplevel)
		wlr_xdg_toplevel_set_activated(c->toplevel, activated);
}

bool tp_client_visible(const struct tp_client *c) {
	if (!c || !c->mapped || !c->mon)
		return false;
	if (c->scratchpad)
		return c->scratchpad_visible;
	return tp_tags_visible(c->tags, &c->mon->view);
}

void tp_client_place(struct tp_client *c, struct tp_rect box) {
	c->geom = box;
	int32_t border = client_border_width(c);
	struct tp_rect content = tp_rect_inset(box, border);
	bool resized = content.w != c->content_geom.w ||
				   content.h != c->content_geom.h;
	c->content_geom = content;
	if (c->tree) {
		wlr_scene_node_set_position(&c->tree->node, box.x, box.y);
		wlr_scene_node_set_position(&c->content->node, border, border);
		update_border(c, c->server->focused_client == c && !c->server->locked);
	}
	if (c->kind == TP_CLIENT_XWAYLAND) {
		tp_xwayland_place(c, content);
	} else if (resized && c->toplevel) {
		/* Each size request costs the client a configure and a redraw, so
		 * send one only when the size really changes. */
		wlr_xdg_toplevel_set_size(c->toplevel, content.w, content.h);
	}
}

void tp_client_close(struct tp_client *c) {
	if (!c)
		return;
	if (c->kind == TP_CLIENT_XWAYLAND)
		tp_xwayland_close(c);
	else if (c->toplevel)
		wlr_xdg_toplevel_send_close(c->toplevel);
}

void tp_client_toggle_tags(struct tp_client *c, tp_tags_t mask) {
	tp_client_set_tags(c, c->tags ^ (mask & TP_TAG_ALL));
}

void tp_client_set_tags(struct tp_client *c, tp_tags_t tags) {
	tags &= TP_TAG_ALL;
	if (!tags)
		return;
	if (!c->scratchpad && tags == c->tags)
		return;
	c->scratchpad = false;
	c->scratchpad_visible = false;
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
	tp_events_emit_window(c->server, TP_EVENT_WINDOW_CHANGE, c,
						  tp_focused(c->server) == c);
}

void tp_client_set_floating(struct tp_client *c, bool floating) {
	if (c->floating == floating || c->fullscreen)
		return;
	tp_pointer_cancel_grab(c->server, c);
	c->floating = floating;
	if (!floating && c->scratchpad) {
		c->scratchpad = false;
		c->scratchpad_visible = false;
		if (c->mon)
			c->tags = tp_tagview_current(&c->mon->view);
	}
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

void tp_client_scratchpad_move(struct tp_client *c) {
	if (!c || !c->mapped)
		return;
	struct tp_server *s = c->server;
	tp_pointer_cancel_grab(s, c);

	if (c->scratchpad) {
		c->scratchpad = false;
		c->scratchpad_visible = false;
		if (c->mon) {
			c->tags = tp_tagview_current(&c->mon->view);
			tp_arrange(c->mon);
		}
		tp_focus(s, c);
		return;
	}

	if (c->fullscreen)
		apply_fullscreen(c, false);
	if (!c->floating) {
		c->floating = true;
		if (c->mon) {
			if (!float_on_monitor(c)) {
				half_monitor_size(c);
				center_float(c);
			} else if (default_float_size(c)) {
				center_float(c);
			}
		}
		apply_floating(c);
	}
	c->scratchpad = true;
	c->scratchpad_visible = false;
	if (c->mon) {
		tp_client_detach(c);
		tp_arrange(c->mon);
	}
	tp_focus_refresh(s);
}

void tp_scratchpad_toggle(struct tp_server *s) {
	if (!s || !s->selmon)
		return;
	struct tp_monitor *m = s->selmon;

	bool hid_any = false;
	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) {
		if (c->mapped && c->scratchpad && c->scratchpad_visible && c->mon == m) {
			tp_pointer_cancel_grab(s, c);
			c->scratchpad_visible = false;
			hid_any = true;
		}
	}
	if (hid_any) {
		tp_arrange(m);
		tp_focus_refresh(s);
		return;
	}

	/* Find the most recently focused scratchpad window. */
	struct tp_client *pick = NULL;
	wl_list_for_each(c, &s->focus_stack, flink) {
		if (c->mapped && c->scratchpad) {
			pick = c;
			break;
		}
	}
	if (!pick)
		return;

	struct tp_monitor *old = pick->mon;
	if (old != m) {
		if (old)
			tp_client_carry_float(pick, old->usable, m->usable);
		tp_client_attach(pick, m, 0);
		tp_client_clear_home(pick);
		if (old)
			tp_arrange(old);
	}
	if (!float_on_monitor(pick)) {
		half_monitor_size(pick);
		center_float(pick);
	} else if (default_float_size(pick)) {
		center_float(pick);
	}
	pick->scratchpad_visible = true;
	tp_arrange(m);
	tp_focus(s, pick);
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
	struct tp_monitor *old = c->mon;
	tp_tags_t previous_tags = c->tags;
	tp_client_detach(c);
	c->mon = m;
	if (tags & TP_TAG_ALL)
		c->tags = tags & TP_TAG_ALL;
	tp_foreign_toplevel_set_output(c, m ? m->output : NULL);
	if (c->mapped && (old != m || previous_tags != c->tags))
		tp_events_emit_window(c->server, TP_EVENT_WINDOW_CHANGE, c,
							  tp_focused(c->server) == c);
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
	if (c->home_output)
		tp_monitors_forget_parked(c->server, c);
	free(c->home_output);
	c->home_output = NULL;
}

struct tp_client *tp_client_at(struct tp_server *s, double x, double y,
							   struct wlr_surface **surface, double *sx,
							   double *sy) {
	double lx, ly;
	struct wlr_scene_node *node =
		wlr_scene_node_at(&s->scene->tree.node, x, y, &lx, &ly);
	if (!node)
		return NULL;
	if (node->type == WLR_SCENE_NODE_RECT)
		return node->parent->node.data;
	if (node->type != WLR_SCENE_NODE_BUFFER)
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
