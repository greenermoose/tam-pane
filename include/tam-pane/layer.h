/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Layer-shell surfaces: panels, bars, launchers and notifications.
 * Each surface lives on one monitor, reserves exclusive space in
 * tp_monitor.usable, and attaches its popups to TP_LAYER_OVERLAY.
 */
#ifndef TAM_PANE_LAYER_H
#define TAM_PANE_LAYER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct tp_monitor;
struct tp_server;
struct wlr_layer_surface_v1;
struct wlr_scene_layer_surface_v1;
struct wlr_scene_tree;
struct wlr_surface;

struct tp_layer_surface {
	struct wl_list link; /* tp_monitor.layers */
	struct tp_server *server;
	struct tp_monitor *mon;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wlr_scene_tree *popup_tree;
	bool mapped;
	uint32_t layer; /* enum zwlr_layer_shell_v1_layer */
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
};

void tp_layers_init(struct tp_server *s);
void tp_layers_finish(struct tp_server *s);

/* Configure every layer surface on `m` and recompute `m->usable`. */
void tp_layers_arrange(struct tp_monitor *m);

/* Close and detach every layer surface bound to `m` before `m` is freed. */
void tp_layers_monitor_destroy(struct tp_monitor *m);

/* Return the layer surface owning `surface` (or its popup), or NULL. */
struct tp_layer_surface *tp_layer_from_surface(struct wlr_surface *surface);

#endif
