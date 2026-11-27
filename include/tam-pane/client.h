/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A client is one toplevel window. It holds only what window management
 * needs; visual effects and scripting state will live in their own
 * modules, never as more flags here.
 */
#ifndef TAM_PANE_CLIENT_H
#define TAM_PANE_CLIENT_H

#include "tam-pane/geom.h"
#include "tam-pane/tags.h"
#include <stdbool.h>
#include <wayland-server-core.h>

struct tp_server;
struct tp_monitor;
struct tp_decoration;
struct tp_foreign_toplevel;
struct wlr_ext_image_capture_source_v1;
struct wlr_scene_tree;
struct wlr_surface;
struct wlr_xdg_toplevel;
struct wlr_xwayland_surface;

enum tp_client_kind {
	TP_CLIENT_XDG = 0,
	TP_CLIENT_XWAYLAND,
};

struct tp_client {
	struct wl_list link;  /* tp_server.clients */
	struct wl_list flink; /* tp_server.focus_stack */
	uint64_t id;
	struct tp_server *server;
	struct tp_monitor *mon;
	char *home_output; /* original output name when displaced by hot-plug */
	enum tp_client_kind kind;
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xwayland_surface *xwayland;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *content;
	struct wlr_scene_rect *border[4];
	struct tp_rect content_geom;
	tp_tags_t tags;
	bool mapped;
	bool associated; /* XWayland: surface map/unmap listeners attached */
	bool floating;
	bool fullscreen;
	bool scratchpad;
	bool scratchpad_visible;
	struct tp_rect geom;	   /* current placement */
	struct tp_rect float_geom; /* remembered floating placement */
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_fullscreen;
	struct wl_listener request_maximize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener set_override_redirect;
	struct tp_foreign_toplevel *foreign;
	struct tp_decoration *decoration;
	struct wlr_ext_image_capture_source_v1 *capture_source;
	struct wl_listener capture_source_destroy;
};

void tp_clients_init(struct tp_server *s);
void tp_clients_finish(struct tp_server *s);

/* Client-kind accessors (work for both XDG and XWayland windows). */
struct wlr_surface *tp_client_surface(const struct tp_client *c);
const char *tp_client_app_id(const struct tp_client *c);
const char *tp_client_title(const struct tp_client *c);
int tp_client_pid(const struct tp_client *c);
bool tp_client_is_xwayland(const struct tp_client *c);
void tp_client_set_activated(struct tp_client *c, bool activated);

/* Shared map/unmap pipelines called by XDG and XWayland lifecycle handlers. */
void tp_client_apply_map(struct tp_client *c, struct tp_rect initial_geom,
						 bool default_floating, bool request_fullscreen);
void tp_client_apply_unmap(struct tp_client *c);

/* True when the window is mapped and its tags are in its monitor's view. */
bool tp_client_visible(const struct tp_client *c);

/* Move and resize the window. */
bool tp_client_create_scene(struct tp_client *c, struct wlr_surface *surface);
void tp_client_destroy_scene(struct tp_client *c);
void tp_client_place(struct tp_client *c, struct tp_rect box);

#define TP_FLOAT_MIN_W 64
#define TP_FLOAT_MIN_H 64

void tp_client_close(struct tp_client *c);
void tp_client_toggle_tags(struct tp_client *c, tp_tags_t mask);
void tp_client_set_tags(struct tp_client *c, tp_tags_t tags);
void tp_client_set_floating(struct tp_client *c, bool floating);
void tp_client_set_fullscreen(struct tp_client *c, bool fullscreen);
void tp_client_scratchpad_move(struct tp_client *c);
void tp_scratchpad_toggle(struct tp_server *s);
void tp_client_resize_floating(struct tp_client *c, enum tp_dir dir,
							   int32_t delta);

/* Move a window to another monitor, assigning that monitor's current view. */
void tp_client_set_monitor(struct tp_client *c, struct tp_monitor *m,
						   bool follow);

/*
 * Monitor membership. Every change of c->mon goes through
 * tp_client_attach, so a monitor's focus slots and layout trees never keep
 * a window that left (0.1.10). Callers then arrange and refocus as needed.
 */

/* Drop c from its monitor's focus slots and layout trees; c->mon stays. */
void tp_client_detach(struct tp_client *c);

/* Detach c, then put it on m (NULL: no monitor). tags == 0 keeps c->tags.
 * The floating place is left alone; see tp_client_carry_float. */
void tp_client_attach(struct tp_client *c, struct tp_monitor *m,
					  tp_tags_t tags);

/* Shift a window's floating place from usable area `from` to `to`,
 * keeping its offset and clamping it inside. Tiled windows keep a
 * floating place too, for Meta+V (B9). */
void tp_client_carry_float(struct tp_client *c, struct tp_rect from,
						   struct tp_rect to);

/* Forget the output a hot-plug displaced the window from. */
void tp_client_clear_home(struct tp_client *c);

/* The window and surface under a layout point, with surface-local
 * coordinates. Any output pointer may be NULL. */
struct tp_client *tp_client_at(struct tp_server *s, double x, double y,
							   struct wlr_surface **surface, double *sx,
							   double *sy);

#endif
