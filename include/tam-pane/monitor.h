/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_MONITOR_H
#define TAM_PANE_MONITOR_H

#include "tam-pane/geom.h"
#include "tam-pane/layout/dwindle.h"
#include "tam-pane/tags.h"
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct tp_server;
struct tp_client;
struct tp_monitor_workspaces;
struct wlr_output;
struct wlr_scene_output;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;

struct tp_monitor {
	struct wl_list link; /* tp_server.monitors */
	struct tp_server *server;
	struct wlr_output *output;
	struct wlr_scene_output *scene_output;
	struct tp_rect area;   /* the whole output, layout coordinates */
	struct tp_rect usable; /* area minus panels' exclusive zones */
	struct wl_list layers; /* tp_layer_surface.link */
	struct tp_tagview view;
	struct tp_client *focus[TP_TAG_SLOTS]; /* slot 0: combined views */
	struct tp_dwindle trees[TP_TAG_SLOTS]; /* see tp_tags_slot() */
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
	/* Nested Wayland output: native fractional scale on the host surface. */
	struct wp_viewporter *host_viewporter;
	struct wp_viewport *host_viewport;
	struct wp_fractional_scale_manager_v1 *host_fsm;
	struct wp_fractional_scale_v1 *host_fscale;
	float host_scale;
	int32_t host_w, host_h;
	struct tp_monitor_workspaces *workspaces;
	bool arrange_dirty;
	uint64_t arrange_count;
};

void tp_monitors_init(struct tp_server *s);
void tp_monitors_finish(struct tp_server *s);

/* The monitor under a layout point, or NULL. */
struct tp_monitor *tp_monitor_at(struct tp_server *s, double x, double y);

/* The connected monitor with output->name matching `name`, or NULL. */
struct tp_monitor *tp_monitor_by_name(struct tp_server *s, const char *name);

/* The next (dir > 0) or previous monitor from s->selmon, or NULL if < 2. */
struct tp_monitor *tp_monitor_cycle(struct tp_server *s, int dir);

/* The adjacent monitor in `dir` from `m`, or NULL. */
struct tp_monitor *tp_monitor_in_dir(struct tp_server *s, struct tp_monitor *m,
									 enum tp_dir dir);

/* Select `m`, move the pointer onto it if needed, and refresh focus. */
void tp_monitor_focus(struct tp_server *s, struct tp_monitor *m);

/* The layout tree for the monitor's current view. */
struct tp_dwindle *tp_monitor_tree(struct tp_monitor *m);

/* Drop a window from every layout tree on the monitor. */
void tp_monitor_forget(struct tp_monitor *m, struct tp_client *c);

/* Propagate live dwindle settings to every connected monitor's layout trees. */
void tp_monitors_apply_settings(struct tp_server *s);

#endif
