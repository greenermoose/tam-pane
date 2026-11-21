/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/layer.h"
#include "tam-pane/focus.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

static enum tp_layer scene_layer_for(enum zwlr_layer_shell_v1_layer l) {
	switch (l) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return TP_LAYER_BACKGROUND;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return TP_LAYER_BOTTOM;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return TP_LAYER_TOP;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return TP_LAYER_OVERLAY;
	}
	return TP_LAYER_TOP;
}

static bool rect_eq(struct tp_rect a, struct tp_rect b) {
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* ---- layer popups ---- */

struct tp_layer_popup {
	struct wlr_xdg_popup *popup;
	struct wlr_box constrain_box;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void popup_commit(struct wl_listener *l, void *data) {
	struct tp_layer_popup *p = wl_container_of(l, p, commit);
	if (p->popup->base->initial_commit) {
		wlr_xdg_popup_unconstrain_from_box(p->popup, &p->constrain_box);
		wlr_xdg_surface_schedule_configure(p->popup->base);
	}
}

static void popup_destroy(struct wl_listener *l, void *data) {
	struct tp_layer_popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void on_new_popup(struct wl_listener *l, void *data) {
	struct tp_layer_surface *ls = wl_container_of(l, ls, new_popup);
	struct wlr_xdg_popup *popup = data;
	if (!ls->mon || !ls->popup_tree)
		return;

	struct tp_layer_popup *p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->popup = popup;
	popup->base->data = wlr_scene_xdg_surface_create(ls->popup_tree, popup->base);
	p->constrain_box = (struct wlr_box){
		.x = ls->mon->area.x - ls->scene->tree->node.x,
		.y = ls->mon->area.y - ls->scene->tree->node.y,
		.width = ls->mon->area.w,
		.height = ls->mon->area.h,
	};

	p->commit.notify = popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
}

/* ---- arrangement ---- */

static void arrange_pass(struct tp_monitor *m, enum zwlr_layer_shell_v1_layer l,
						 bool exclusive, const struct wlr_box *full,
						 struct wlr_box *usable) {
	struct tp_layer_surface *ls;
	wl_list_for_each(ls, &m->layers, link) {
		if (!ls->layer_surface->initialized &&
			!ls->layer_surface->initial_commit)
			continue;
		if (ls->layer != (uint32_t)l)
			continue;
		bool is_excl = ls->layer_surface->current.exclusive_zone > 0;
		if (is_excl != exclusive)
			continue;
		wlr_scene_layer_surface_v1_configure(ls->scene, full, usable);
	}
}

void tp_layers_arrange(struct tp_monitor *m) {
	static const enum zwlr_layer_shell_v1_layer order[4] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
	};
	struct tp_server *s = m->server;
	struct wlr_box full = {m->area.x, m->area.y, m->area.w, m->area.h};
	struct wlr_box usable = full;

	struct tp_layer_surface *ls;
	wl_list_for_each(ls, &m->layers, link) {
		uint32_t cur = ls->layer_surface->current.layer;
		if (ls->layer != cur) {
			ls->layer = cur;
			wlr_scene_node_reparent(&ls->scene->tree->node,
									s->layer[scene_layer_for(cur)]);
		}
	}

	/* Exclusive surfaces reserve space first, top to bottom, so non-exclusive
	 * overlays and wallpapers see the final usable area. */
	for (int i = 0; i < 4; i++)
		arrange_pass(m, order[i], true, &full, &usable);
	for (int i = 0; i < 4; i++)
		arrange_pass(m, order[i], false, &full, &usable);

	wl_list_for_each(ls, &m->layers, link) {
		if (ls->popup_tree)
			wlr_scene_node_set_position(&ls->popup_tree->node,
										ls->scene->tree->node.x,
										ls->scene->tree->node.y);
	}

	m->usable = (struct tp_rect){usable.x, usable.y, usable.width, usable.height};
}

/* ---- surface lifecycle ---- */

static void on_commit(struct wl_listener *l, void *data) {
	struct tp_layer_surface *ls = wl_container_of(l, ls, commit);
	if (!ls->mon)
		return;
	struct wlr_layer_surface_v1 *wlr = ls->layer_surface;
	if (!wlr->initial_commit && wlr->current.committed == 0)
		return;

	struct tp_rect before = ls->mon->usable;
	tp_layers_arrange(ls->mon);
	if (!rect_eq(before, ls->mon->usable))
		tp_arrange(ls->mon);

	if (ls->mapped &&
		(wlr->current.committed &
		 WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY)) {
		if (wlr->current.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
			ls->server->focused_layer == ls) {
			ls->server->focused_layer = NULL;
			tp_focus_refresh(ls->server);
		} else if (wlr->current.keyboard_interactive ==
				   ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
			tp_focus_refresh(ls->server);
		}
	}
}

static void on_map(struct wl_listener *l, void *data) {
	struct tp_layer_surface *ls = wl_container_of(l, ls, map);
	ls->mapped = true;
	if (ls->popup_tree)
		wlr_scene_node_set_enabled(&ls->popup_tree->node, true);
	if (ls->mon) {
		tp_layers_arrange(ls->mon);
		tp_arrange(ls->mon);
	}
	tp_focus_refresh(ls->server);
	tam_log_debug("layer surface mapped: %s (layer %u, excl %d, kb %d)",
				  ls->layer_surface->namespace ? ls->layer_surface->namespace
											   : "(none)",
				  ls->layer, ls->layer_surface->current.exclusive_zone,
				  ls->layer_surface->current.keyboard_interactive);
}

static void on_unmap(struct wl_listener *l, void *data) {
	struct tp_layer_surface *ls = wl_container_of(l, ls, unmap);
	struct tp_server *s = ls->server;
	ls->mapped = false;
	if (ls->popup_tree)
		wlr_scene_node_set_enabled(&ls->popup_tree->node, false);
	if (s->focused_layer == ls)
		s->focused_layer = NULL;
	if (s->stopping)
		return;
	if (ls->mon) {
		tp_layers_arrange(ls->mon);
		tp_arrange(ls->mon);
	}
	tp_focus_refresh(s);
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct tp_layer_surface *ls = wl_container_of(l, ls, destroy);
	struct tp_server *s = ls->server;
	struct tp_monitor *m = ls->mon;

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->new_popup.link);
	wl_list_remove(&ls->link);
	if (s->focused_layer == ls)
		s->focused_layer = NULL;
	if (ls->popup_tree)
		wlr_scene_node_destroy(&ls->popup_tree->node);
	free(ls);

	if (m && !s->stopping) {
		tp_layers_arrange(m);
		tp_arrange(m);
		tp_focus_refresh(s);
	}
}

static void on_new_layer_surface(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	if (!layer_surface->output) {
		if (!s->selmon) {
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
		layer_surface->output = s->selmon->output;
	}
	struct tp_monitor *m = layer_surface->output->data;
	if (!m) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	struct tp_layer_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) {
		tam_log_error("new layer surface: out of memory");
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	ls->server = s;
	ls->mon = m;
	ls->layer_surface = layer_surface;
	ls->layer = layer_surface->pending.layer;
	ls->scene = wlr_scene_layer_surface_v1_create(
		s->layer[scene_layer_for(ls->layer)], layer_surface);
	if (!ls->scene) {
		free(ls);
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	/* Leave ls->scene->tree->node.data NULL so tp_client_at() never mistakes
	 * a layer surface for a toplevel client. */
	ls->popup_tree = wlr_scene_tree_create(s->layer[TP_LAYER_OVERLAY]);
	if (ls->popup_tree)
		wlr_scene_node_set_enabled(&ls->popup_tree->node, false);
	layer_surface->data = ls;
	wl_list_insert(m->layers.prev, &ls->link);

	struct wlr_surface *surface = layer_surface->surface;
	ls->map.notify = on_map;
	wl_signal_add(&surface->events.map, &ls->map);
	ls->unmap.notify = on_unmap;
	wl_signal_add(&surface->events.unmap, &ls->unmap);
	ls->commit.notify = on_commit;
	wl_signal_add(&surface->events.commit, &ls->commit);
	ls->destroy.notify = on_destroy;
	wl_signal_add(&layer_surface->events.destroy, &ls->destroy);
	ls->new_popup.notify = on_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &ls->new_popup);
}

void tp_layers_init(struct tp_server *s) {
	s->new_layer_surface.notify = on_new_layer_surface;
	wl_signal_add(&s->layer_shell->events.new_surface, &s->new_layer_surface);
}

void tp_layers_finish(struct tp_server *s) {
	if (s->new_layer_surface.notify)
		wl_list_remove(&s->new_layer_surface.link);
	s->new_layer_surface.notify = NULL;
	s->focused_layer = NULL;
}

void tp_layers_monitor_destroy(struct tp_monitor *m) {
	struct tp_layer_surface *ls, *tmp;
	wl_list_for_each_safe(ls, tmp, &m->layers, link) {
		ls->mon = NULL;
		wlr_layer_surface_v1_destroy(ls->layer_surface);
	}
}

struct tp_layer_surface *tp_layer_from_surface(struct wlr_surface *surface) {
	while (surface) {
		struct wlr_layer_surface_v1 *wlr =
			wlr_layer_surface_v1_try_from_wlr_surface(surface);
		if (wlr)
			return wlr->data;
		struct wlr_xdg_popup *popup =
			wlr_xdg_popup_try_from_wlr_surface(surface);
		if (!popup)
			break;
		surface = popup->parent;
	}
	return NULL;
}
