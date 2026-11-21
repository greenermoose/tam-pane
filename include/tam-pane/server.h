/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The server: one instance holds every piece of compositor state. Modules
 * receive it explicitly; there are no global variables. Each module owns
 * the listeners it sets up here and removes them in its own finish call.
 */
#ifndef TAM_PANE_SERVER_H
#define TAM_PANE_SERVER_H

#include "tam-pane/geom.h"
#include "tam-pane/layout/dwindle.h"
#include "tam-pane/server_events.h"
#include "tam-pane/settings.h"
#include <limits.h>
#include <stdbool.h>
#include <wayland-server-core.h>

struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_cursor;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_scene_tree;
struct wlr_seat;
struct wlr_session;
struct wlr_xcursor_manager;
struct wlr_xdg_shell;
struct wlr_layer_shell_v1;
struct tp_client;
struct tp_layer_surface;
struct tp_lua;
struct tp_monitor;

/* Interactive pointer drag state (explicit in tp_server, no globals). */
enum tp_grab_mode {
	TP_GRAB_NONE,
	TP_GRAB_MOVE,
	TP_GRAB_RESIZE,
};

struct tp_pointer_grab {
	enum tp_grab_mode mode;
	struct tp_client *client;
	uint32_t button;
	double start_x, start_y;
	struct tp_rect start_geom;
	uint32_t edges;
	bool tiled;
	struct tp_dwindle_drag drag;
};

/* Scene layers, bottom to top. */
enum tp_layer {
	TP_LAYER_BACKGROUND,
	TP_LAYER_BOTTOM,
	TP_LAYER_TILE,
	TP_LAYER_FLOAT,
	TP_LAYER_TOP,
	TP_LAYER_FULLSCREEN,
	TP_LAYER_OVERLAY,
	TP_LAYER_COUNT
};

struct tp_server {
	struct tp_settings settings;
	struct tp_lua *lua;
	char config_path[PATH_MAX];
	struct tp_event_queue events;

	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layer[TP_LAYER_COUNT];
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;

	struct wl_list monitors;	/* tp_monitor.link */
	struct tp_monitor *selmon;	/* monitor that receives new windows */
	struct wl_listener new_output;
	struct wl_listener layout_change;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_list clients;		/* tp_client.link, stacking-independent */
	struct wl_list focus_stack; /* tp_client.flink, most recent first */
	struct wl_listener new_toplevel;
	struct wl_listener new_popup;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;
	struct tp_layer_surface *focused_layer;

	struct wlr_seat *seat;
	struct wl_list keyboards; /* tp_keyboard.link */
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct tp_pointer_grab grab;

	struct wl_event_source *sigterm, *sigint, *sighup;
	struct wl_event_source *reload_idle;
	bool stopping; /* set while tearing down: no more layout or focus */
	const char *socket;
};

/* Create every global and module. Returns false (after logging) when the
 * compositor cannot run; tp_server_finish is still safe to call. */
bool tp_server_init(struct tp_server *s);

/* Open the Wayland socket, start the backend and export WAYLAND_DISPLAY. */
bool tp_server_start(struct tp_server *s);

/* Run until tp_server_quit or SIGTERM/SIGINT. */
void tp_server_run(struct tp_server *s);
void tp_server_quit(struct tp_server *s);

/* Set the configuration path used for live reloads (empty = default path). */
void tp_server_set_config_path(struct tp_server *s, const char *path);

/*
 * Reload the Lua configuration in place from s->config_path (or the default
 * path if unset). On success, swaps in the new Lua state and settings,
 * applies live changes, and returns true. On failure, keeps the last good
 * configuration, logs the error, writes a diagnostic to err_buf (if non-NULL),
 * and returns false.
 */
bool tp_server_reload(struct tp_server *s, char *err_buf, size_t err_buf_size);

/* Schedule a coalesced reload on the Wayland idle loop (safe from inside key
 * bindings, Lua callbacks, or event hooks). */
void tp_server_request_reload(struct tp_server *s);

/* Tear down in dependency order. */
void tp_server_finish(struct tp_server *s);

#endif
