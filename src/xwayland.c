/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Opt-in XWayland integration (0.2.6, Phase P3).
 *
 * Compiled only when built with XWAYLAND=1 (-DTP_XWAYLAND=1). Managed X11
 * windows become ordinary tp_client windows; override-redirect surfaces
 * (menus, tooltips, popups) live on TP_LAYER_OVERLAY outside s->clients.
 */
#include "tam-pane/input.h"
#include "tam-pane/xwayland.h"
#include "tam-pane/capture.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/foreign_toplevel.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/workspace.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/xwayland/xwayland.h>
#include <xcb/xcb_icccm.h>

struct tp_xwayland_unmanaged {
	struct tp_server *server;
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *tree;
	bool associated;
	bool mapped;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener request_configure;
	struct wl_listener set_geometry;
	struct wl_listener set_override_redirect;
};

static void create_managed(struct tp_server *s,
						   struct wlr_xwayland_surface *xsurface);
static void create_unmanaged(struct tp_server *s,
							 struct wlr_xwayland_surface *xsurface);

/* ---- managed XWayland windows (tp_client) ---- */

static bool xwayland_should_float(const struct wlr_xwayland_surface *xs) {
	if (xs->parent != NULL || xs->modal)
		return true;
	static const enum wlr_xwayland_net_wm_window_type float_types[] = {
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,
	};
	for (size_t i = 0; i < sizeof(float_types) / sizeof(float_types[0]); i++) {
		if (wlr_xwayland_surface_has_window_type(xs, float_types[i]))
			return true;
	}
	if (xs->size_hints) {
		const uint32_t both =
			XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
		if ((xs->size_hints->flags & both) == both &&
			xs->size_hints->min_width > 0 &&
			xs->size_hints->min_width == xs->size_hints->max_width &&
			xs->size_hints->min_height == xs->size_hints->max_height)
			return true;
	}
	return false;
}

static void managed_on_map(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, map);
	struct wlr_xwayland_surface *xs = c->xwayland;
	if (!xs || !xs->surface)
		return;

	if (!tp_client_create_scene(c, xs->surface)) {
		tam_log_error("xwayland map: cannot create scene tree");
		return;
	}
	wlr_xwayland_surface_set_minimized(xs, false);

	struct tp_rect init = {
		.x = xs->x,
		.y = xs->y,
		.w = xs->width,
		.h = xs->height,
	};
	tp_client_apply_map(c, init, xwayland_should_float(xs), xs->fullscreen);
}

static void managed_on_unmap(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, unmap);
	tp_client_apply_unmap(c);
	tp_client_destroy_scene(c);
}

static void managed_detach_surface(struct tp_client *c) {
	if (!c->associated)
		return;
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	c->map.notify = NULL;
	c->unmap.notify = NULL;
	c->associated = false;
}

static void managed_on_associate(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, associate);
	struct wlr_xwayland_surface *xs = c->xwayland;
	if (!xs || !xs->surface || c->associated)
		return;
	c->map.notify = managed_on_map;
	wl_signal_add(&xs->surface->events.map, &c->map);
	c->unmap.notify = managed_on_unmap;
	wl_signal_add(&xs->surface->events.unmap, &c->unmap);
	c->associated = true;
}

static void managed_on_dissociate(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, dissociate);
	if (c->mapped) {
		tp_client_apply_unmap(c);
		tp_client_destroy_scene(c);
	}
	managed_detach_surface(c);
}

static void managed_on_request_configure(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	struct wlr_xwayland_surface *xs = c->xwayland;
	if (!xs)
		return;
	if (!c->mapped || (c->floating && !c->fullscreen)) {
		uint16_t w = ev->width;
		uint16_t h = ev->height;
		if (c->mapped) {
			if (w < TP_FLOAT_MIN_W)
				w = TP_FLOAT_MIN_W;
			if (h < TP_FLOAT_MIN_H)
				h = TP_FLOAT_MIN_H;
			int32_t border = c->content_geom.x - c->geom.x;
			c->float_geom = (struct tp_rect){ev->x - border, ev->y - border,
											w + 2 * border, h + 2 * border};
			tp_client_place(c, c->float_geom);
			return;
		}
		wlr_xwayland_surface_configure(xs, ev->x, ev->y, w, h);
	} else {
		struct tp_rect content = c->content_geom;
		wlr_xwayland_surface_configure(xs, (int16_t)content.x,
									   (int16_t)content.y, (uint16_t)content.w,
									   (uint16_t)content.h);
	}
}

static void managed_on_request_fullscreen(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, request_fullscreen);
	if (c->mapped && c->xwayland)
		tp_client_set_fullscreen(c, c->xwayland->fullscreen);
}

static void managed_on_request_maximize(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, request_maximize);
	if (c->mapped && c->xwayland) {
		wlr_xwayland_surface_configure(c->xwayland, (int16_t)c->content_geom.x,
									   (int16_t)c->content_geom.y, (uint16_t)c->content_geom.w,
									   (uint16_t)c->content_geom.h);
	}
}

static void managed_on_request_activate(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, request_activate);
	struct tp_server *s = c->server;
	if (!c->mapped || s->locked || s->stopping)
		return;
	if (c->mon && !tp_client_visible(c)) {
		unsigned int first = tp_tags_first(c->tags);
		if (first > 0) {
			tp_tags_t prev = tp_tagview_current(&c->mon->view);
			if (tp_tagview_show(&c->mon->view, tp_tag_mask(first))) {
				tp_arrange(c->mon);
				tp_events_emit_tag(s, c->mon, prev,
								   tp_tagview_current(&c->mon->view));
				tp_workspace_sync_monitor(c->mon);
			}
		}
	}
	tp_focus(s, c);
}

static void managed_on_set_info(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, set_title);
	tp_foreign_toplevel_update_info(c);
}

static void managed_on_set_class(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, set_app_id);
	tp_foreign_toplevel_update_info(c);
}

static void managed_destroy_cleanup(struct tp_client *c) {
	if (c->mapped)
		tp_client_apply_unmap(c);
	tp_client_destroy_scene(c);
	managed_detach_surface(c);
	tp_foreign_toplevel_unmap(c);
	if (c->server->focused_client == c)
		c->server->focused_client = NULL;
	wl_list_remove(&c->associate.link);
	wl_list_remove(&c->dissociate.link);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->request_configure.link);
	wl_list_remove(&c->request_fullscreen.link);
	wl_list_remove(&c->request_maximize.link);
	wl_list_remove(&c->request_activate.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->set_app_id.link);
	wl_list_remove(&c->set_override_redirect.link);
	if (c->xwayland && c->xwayland->data == c)
		c->xwayland->data = NULL;
	free(c->home_output);
	free(c);
}

static void managed_on_destroy(struct wl_listener *l, void *data) {
	struct tp_client *c = wl_container_of(l, c, destroy);
	managed_destroy_cleanup(c);
}

static void managed_on_set_override_redirect(struct wl_listener *l,
											 void *data) {
	struct tp_client *c = wl_container_of(l, c, set_override_redirect);
	struct tp_server *s = c->server;
	struct wlr_xwayland_surface *xs = c->xwayland;
	if (!xs || !xs->override_redirect)
		return;
	managed_destroy_cleanup(c);
	create_unmanaged(s, xs);
}

static void create_managed(struct tp_server *s,
						   struct wlr_xwayland_surface *xsurface) {
	struct tp_client *c = calloc(1, sizeof(*c));
	if (!c) {
		tam_log_error("xwayland: out of memory");
		return;
	}
	c->server = s;
	c->kind = TP_CLIENT_XWAYLAND;
	c->xwayland = xsurface;
	xsurface->data = c;
	wl_list_init(&c->link);
	wl_list_init(&c->flink);

	c->associate.notify = managed_on_associate;
	wl_signal_add(&xsurface->events.associate, &c->associate);
	c->dissociate.notify = managed_on_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &c->dissociate);
	c->destroy.notify = managed_on_destroy;
	wl_signal_add(&xsurface->events.destroy, &c->destroy);
	c->request_configure.notify = managed_on_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &c->request_configure);
	c->request_fullscreen.notify = managed_on_request_fullscreen;
	wl_signal_add(&xsurface->events.request_fullscreen, &c->request_fullscreen);
	c->request_maximize.notify = managed_on_request_maximize;
	wl_signal_add(&xsurface->events.request_maximize, &c->request_maximize);
	c->request_activate.notify = managed_on_request_activate;
	wl_signal_add(&xsurface->events.request_activate, &c->request_activate);
	c->set_title.notify = managed_on_set_info;
	wl_signal_add(&xsurface->events.set_title, &c->set_title);
	c->set_app_id.notify = managed_on_set_class;
	wl_signal_add(&xsurface->events.set_class, &c->set_app_id);
	c->set_override_redirect.notify = managed_on_set_override_redirect;
	wl_signal_add(&xsurface->events.set_override_redirect,
				  &c->set_override_redirect);

	if (xsurface->surface) {
		managed_on_associate(&c->associate, NULL);
		if (xsurface->surface->mapped)
			managed_on_map(&c->map, NULL);
	}
}

/* ---- override-redirect (unmanaged) XWayland surfaces ---- */

static void unmanaged_on_map(struct wl_listener *l, void *data) {
	struct tp_xwayland_unmanaged *u = wl_container_of(l, u, map);
	struct tp_server *s = u->server;
	struct wlr_xwayland_surface *xs = u->xsurface;
	if (!xs || !xs->surface)
		return;

	u->tree = wlr_scene_subsurface_tree_create(s->layer[TP_LAYER_OVERLAY],
											   xs->surface);
	if (!u->tree)
		return;
	wlr_scene_node_set_position(&u->tree->node, xs->x, xs->y);
	u->mapped = true;

	if (!s->locked && s->seat &&
		wlr_xwayland_surface_override_redirect_wants_focus(xs)) {
		wlr_xwayland_surface_activate(xs, true);
		tp_keyboard_enter(s, xs->surface);
	}
}

static void unmanaged_on_unmap(struct wl_listener *l, void *data) {
	struct tp_xwayland_unmanaged *u = wl_container_of(l, u, unmap);
	struct tp_server *s = u->server;
	bool had_kb = (s->seat && u->xsurface && u->xsurface->surface &&
				   s->seat->keyboard_state.focused_surface ==
					   u->xsurface->surface);
	u->mapped = false;
	if (u->tree) {
		wlr_scene_node_destroy(&u->tree->node);
		u->tree = NULL;
	}
	if (had_kb && !s->stopping)
		tp_focus_refresh(s);
}

static void unmanaged_detach_surface(struct tp_xwayland_unmanaged *u) {
	if (!u->associated)
		return;
	wl_list_remove(&u->map.link);
	wl_list_remove(&u->unmap.link);
	u->map.notify = NULL;
	u->unmap.notify = NULL;
	u->associated = false;
}

static void unmanaged_on_associate(struct wl_listener *l, void *data) {
	struct tp_xwayland_unmanaged *u = wl_container_of(l, u, associate);
	if (!u->xsurface || !u->xsurface->surface || u->associated)
		return;
	u->map.notify = unmanaged_on_map;
	wl_signal_add(&u->xsurface->surface->events.map, &u->map);
	u->unmap.notify = unmanaged_on_unmap;
	wl_signal_add(&u->xsurface->surface->events.unmap, &u->unmap);
	u->associated = true;
}

static void unmanaged_on_dissociate(struct wl_listener *l, void *data) {
	struct tp_xwayland_unmanaged *u = wl_container_of(l, u, dissociate);
	if (u->mapped)
		unmanaged_on_unmap(&u->unmap, NULL);
	unmanaged_detach_surface(u);
}

static void unmanaged_on_request_configure(struct wl_listener *l, void *data) {
	struct tp_xwayland_unmanaged *u = wl_container_of(l, u, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	wlr_xwayland_surface_configure(u->xsurface, ev->x, ev->y, ev->width,
								   ev->height);
	if (u->tree)
		wlr_scene_node_set_position(&u->tree->node, ev->x, ev->y);
}

static void unmanaged_on_set_geometry(struct wl_listener *l, void *data) {
	struct tp_xwayland_unmanaged *u = wl_container_of(l, u, set_geometry);
	if (u->tree)
		wlr_scene_node_set_position(&u->tree->node, u->xsurface->x,
									u->xsurface->y);
}

static void unmanaged_destroy_cleanup(struct tp_xwayland_unmanaged *u) {
	if (u->mapped)
		unmanaged_on_unmap(&u->unmap, NULL);
	unmanaged_detach_surface(u);
	wl_list_remove(&u->associate.link);
	wl_list_remove(&u->dissociate.link);
	wl_list_remove(&u->destroy.link);
	wl_list_remove(&u->request_configure.link);
	wl_list_remove(&u->set_geometry.link);
	wl_list_remove(&u->set_override_redirect.link);
	free(u);
}

static void unmanaged_on_destroy(struct wl_listener *l, void *data) {
	struct tp_xwayland_unmanaged *u = wl_container_of(l, u, destroy);
	unmanaged_destroy_cleanup(u);
}

static void unmanaged_on_set_override_redirect(struct wl_listener *l,
											   void *data) {
	struct tp_xwayland_unmanaged *u =
		wl_container_of(l, u, set_override_redirect);
	struct tp_server *s = u->server;
	struct wlr_xwayland_surface *xs = u->xsurface;
	if (!xs || xs->override_redirect)
		return;
	unmanaged_destroy_cleanup(u);
	create_managed(s, xs);
}

static void create_unmanaged(struct tp_server *s,
							 struct wlr_xwayland_surface *xsurface) {
	struct tp_xwayland_unmanaged *u = calloc(1, sizeof(*u));
	if (!u)
		return;
	u->server = s;
	u->xsurface = xsurface;
	xsurface->data = NULL;

	u->associate.notify = unmanaged_on_associate;
	wl_signal_add(&xsurface->events.associate, &u->associate);
	u->dissociate.notify = unmanaged_on_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &u->dissociate);
	u->destroy.notify = unmanaged_on_destroy;
	wl_signal_add(&xsurface->events.destroy, &u->destroy);
	u->request_configure.notify = unmanaged_on_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &u->request_configure);
	u->set_geometry.notify = unmanaged_on_set_geometry;
	wl_signal_add(&xsurface->events.set_geometry, &u->set_geometry);
	u->set_override_redirect.notify = unmanaged_on_set_override_redirect;
	wl_signal_add(&xsurface->events.set_override_redirect,
				  &u->set_override_redirect);

	if (xsurface->surface) {
		unmanaged_on_associate(&u->associate, NULL);
		if (xsurface->surface->mapped)
			unmanaged_on_map(&u->map, NULL);
	}
}

/* ---- server-level XWayland lifecycle ---- */

static void on_xwayland_ready(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, xwayland_ready);
	if (!s->xwayland)
		return;
	if (s->seat)
		wlr_xwayland_set_seat(s->xwayland, s->seat);
	tam_log_info("XWayland ready on DISPLAY=%s",
				 s->xwayland->display_name ? s->xwayland->display_name : "");
}

static void on_xwayland_new_surface(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, xwayland_new_surface);
	struct wlr_xwayland_surface *xsurface = data;
	if (!xsurface)
		return;
	if (xsurface->override_redirect)
		create_unmanaged(s, xsurface);
	else
		create_managed(s, xsurface);
}

void tp_xwayland_init(struct tp_server *s) {
	if (!s || !s->display || !s->compositor)
		return;
	s->xwayland = wlr_xwayland_create(s->display, s->compositor, true);
	if (!s->xwayland) {
		tam_log_warn("XWayland unavailable; continuing Wayland-only");
		return;
	}
	s->xwayland_display = s->xwayland->display_name;
	s->xwayland_ready.notify = on_xwayland_ready;
	wl_signal_add(&s->xwayland->events.ready, &s->xwayland_ready);
	s->xwayland_new_surface.notify = on_xwayland_new_surface;
	wl_signal_add(&s->xwayland->events.new_surface, &s->xwayland_new_surface);
}

void tp_xwayland_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->xwayland_ready.notify) {
		wl_list_remove(&s->xwayland_ready.link);
		s->xwayland_ready.notify = NULL;
	}
	if (s->xwayland_new_surface.notify) {
		wl_list_remove(&s->xwayland_new_surface.link);
		s->xwayland_new_surface.notify = NULL;
	}
	if (s->xwayland) {
		wlr_xwayland_destroy(s->xwayland);
		s->xwayland = NULL;
	}
	s->xwayland_display = NULL;
}

/* ---- client-kind dispatch helpers ---- */

struct wlr_surface *tp_xwayland_surface(const struct tp_client *c) {
	return (c && c->xwayland) ? c->xwayland->surface : NULL;
}

const char *tp_xwayland_app_id(const struct tp_client *c) {
	if (!c || !c->xwayland)
		return NULL;
	return c->xwayland->class ? c->xwayland->class : c->xwayland->instance;
}

const char *tp_xwayland_title(const struct tp_client *c) {
	return (c && c->xwayland) ? c->xwayland->title : NULL;
}

void tp_xwayland_place(struct tp_client *c, struct tp_rect box) {
	if (!c || !c->xwayland)
		return;
	int16_t x = (int16_t)box.x;
	int16_t y = (int16_t)box.y;
	uint16_t w = box.w > 0 ? (uint16_t)box.w : 1;
	uint16_t h = box.h > 0 ? (uint16_t)box.h : 1;
	if (c->xwayland->x != x || c->xwayland->y != y || c->xwayland->width != w ||
		c->xwayland->height != h)
		wlr_xwayland_surface_configure(c->xwayland, x, y, w, h);
}

void tp_xwayland_close(struct tp_client *c) {
	if (c && c->xwayland)
		wlr_xwayland_surface_close(c->xwayland);
}

void tp_xwayland_set_activated(struct tp_client *c, bool activated) {
	if (!c || !c->xwayland)
		return;
	if (activated) {
		if (wlr_xwayland_surface_icccm_input_model(c->xwayland) ==
			WLR_ICCCM_INPUT_MODEL_GLOBAL)
			wlr_xwayland_surface_offer_focus(c->xwayland);
		wlr_xwayland_surface_activate(c->xwayland, true);
		wlr_xwayland_surface_restack(c->xwayland, NULL, XCB_STACK_MODE_ABOVE);
	} else {
		wlr_xwayland_surface_activate(c->xwayland, false);
	}
}

void tp_xwayland_set_fullscreen(struct tp_client *c, bool fullscreen) {
	if (c && c->xwayland)
		wlr_xwayland_surface_set_fullscreen(c->xwayland, fullscreen);
}

bool tp_xwayland_accepts_keyboard(const struct tp_client *c) {
	if (!c || !c->xwayland)
		return false;
	return wlr_xwayland_surface_icccm_input_model(c->xwayland) !=
		   WLR_ICCCM_INPUT_MODEL_NONE;
}

struct tp_client *tp_xwayland_client_from_surface(struct wlr_surface *surface) {
	if (!surface)
		return NULL;
	struct wlr_xwayland_surface *xs =
		wlr_xwayland_surface_try_from_wlr_surface(surface);
	if (!xs || xs->override_redirect || !xs->data)
		return NULL;
	return xs->data;
}
