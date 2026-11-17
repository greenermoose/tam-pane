/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_MONITOR_H
#define TAM_PANE_MONITOR_H

#include "tam-pane/geom.h"
#include "tam-pane/layout/dwindle.h"
#include "tam-pane/tags.h"
#include <wayland-server-core.h>

struct tp_server;
struct tp_client;
struct wlr_output;
struct wlr_scene_output;

struct tp_monitor {
	struct wl_list link; /* tp_server.monitors */
	struct tp_server *server;
	struct wlr_output *output;
	struct wlr_scene_output *scene_output;
	struct tp_rect area;   /* the whole output, layout coordinates */
	struct tp_rect usable; /* area minus panels' exclusive zones */
	struct tp_tagview view;
	struct tp_client *focus[TP_TAG_SLOTS]; /* slot 0: combined views */
	struct tp_dwindle trees[TP_TAG_SLOTS]; /* see tp_tags_slot() */
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

void tp_monitors_init(struct tp_server *s);
void tp_monitors_finish(struct tp_server *s);

/* The monitor under a layout point, or NULL. */
struct tp_monitor *tp_monitor_at(struct tp_server *s, double x, double y);

/* The layout tree for the monitor's current view. */
struct tp_dwindle *tp_monitor_tree(struct tp_monitor *m);

/* Drop a window from every layout tree on the monitor. */
void tp_monitor_forget(struct tp_monitor *m, struct tp_client *c);

#endif
