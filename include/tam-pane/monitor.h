/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_MONITOR_H
#define TAM_PANE_MONITOR_H

#include "tam-pane/geom.h"
#include "tam-pane/layout/layout.h"
#include "tam-pane/tags.h"
#include <stdbool.h>
#include <stddef.h>
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
struct wl_registry;
struct wl_callback;

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
	struct tp_layout layouts[TP_TAG_SLOTS]; /* see tp_tags_slot() */
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener commit; /* logs each modeset */
	struct wl_listener destroy;
	/* Nested Wayland output: native fractional scale on the host surface. */
	struct wl_registry *host_reg;
	struct wl_callback *host_sync;
	struct wp_viewporter *host_viewporter;
	struct wp_viewport *host_viewport;
	struct wp_fractional_scale_manager_v1 *host_fsm;
	struct wp_fractional_scale_v1 *host_fscale;
	float host_scale;
	int32_t host_w, host_h;
	struct tp_monitor_workspaces *workspaces;
	bool disabled; /* logically disabled (clamshell / tp.monitor disabled) */
	bool arrange_dirty;
	uint64_t arrange_count;
};

/* What a removed monitor was showing, kept by output name so the same
 * output picks up where it left off when it returns, as after a suspend
 * that drops and re-adds a display. The layouts still hold the windows
 * that were tiled there; those windows are on another monitor meanwhile,
 * and tp_monitors_forget_parked drops one that closes or is moved for good. */
struct tp_parked_monitor {
	struct wl_list link; /* tp_server.parked_monitors */
	char *name;
	struct tp_tagview view;
	struct tp_layout layouts[TP_TAG_SLOTS];
};

/* At most this many removed monitors are remembered; the oldest goes. */
#define TP_PARKED_MONITORS_MAX 8

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

/* The layout for the monitor's current view. */
struct tp_layout *tp_monitor_layout(struct tp_monitor *m);

/* The remembered state of the removed output `name`, or NULL. */
struct tp_parked_monitor *tp_monitor_parked_find(struct tp_server *s,
												 const char *name);

/* Drop a window from every remembered monitor's layouts. */
void tp_monitors_forget_parked(struct tp_server *s, struct tp_client *c);

/* Drop a window from every layout slot on the monitor. */
void tp_monitor_forget(struct tp_monitor *m, struct tp_client *c);

/* Propagate live layout settings to every connected monitor's layout slots. */
void tp_monitors_apply_settings(struct tp_server *s);

/* Apply staged tp.monitor rules to all connected monitors (on reload). */
void tp_monitors_apply_config(struct tp_server *s);

/* Change a connected, powered-on monitor's mode (NULL or "focused" =
 * s->selmon). refresh_mhz 0 takes the highest refresh for that size; else
 * the output's own mode within 10 mHz of it. An output with no fixed modes
 * (headless, nested) gets a custom mode. True only when the commit went
 * through, which on DRM is a full modeset; on failure, err says why. */
bool tp_monitor_set_mode(struct tp_server *s, const char *name, int32_t w,
						 int32_t h, int32_t refresh_mhz, char *err,
						 size_t err_size);

/* Change scale on a connected monitor (NULL or "focused" = s->selmon). */
bool tp_monitor_set_scale(struct tp_server *s, const char *name, float scale);

/* Logically enable or disable a connected monitor (clamshell / laptop display).
 * Disabling removes the output from the layout and displaces its windows to an
 * enabled heir; enabling restores it and reclaims displaced windows. */
bool tp_monitor_set_enabled(struct tp_server *s, const char *name, bool enabled,
							char *err, size_t err_size);

#endif
