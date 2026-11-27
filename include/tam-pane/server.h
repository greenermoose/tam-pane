/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The server: one instance holds every piece of compositor state. Modules
 * receive it explicitly; there are no global variables. Each module owns
 * the listeners it sets up here and removes them in its own finish call.
 */
#ifndef TAM_PANE_SERVER_H
#define TAM_PANE_SERVER_H

#include "tam-pane/chord.h"
#include "tam-pane/geom.h"
#include "tam-pane/layout/layout.h"
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
struct wlr_security_context_manager_v1;
struct wlr_session;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_ext_workspace_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_output_power_manager_v1;
struct wlr_screencopy_manager_v1;
struct wlr_ext_image_copy_capture_manager_v1;
struct wlr_ext_output_image_capture_source_manager_v1;
struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1;
struct wlr_xdg_output_manager_v1;
struct wlr_output_manager_v1;
struct wlr_gamma_control_manager_v1;
struct wlr_cursor_shape_manager_v1;
struct wlr_text_input_manager_v3;
struct wlr_input_method_manager_v2;
struct wlr_xdg_activation_v1;
struct wlr_primary_selection_v1_device_manager;
struct wlr_data_control_manager_v1;
struct wlr_ext_data_control_manager_v1;
struct wlr_xdg_decoration_manager_v1;
struct wlr_xcursor_manager;
struct wlr_xdg_shell;
struct wlr_layer_shell_v1;
struct wlr_xwayland;
struct tp_chord_overlay;
struct tp_client;
struct tp_ipc_server;
struct tp_layer_surface;
struct tp_lua;
struct tp_monitor;
struct tp_text_input_relay;

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
	struct tp_layout_drag drag;
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
	TP_LAYER_LOCK,
	TP_LAYER_COUNT
};

struct tp_server {
	struct tp_settings settings;
	struct tp_lua *lua;
	struct tp_chord_state chord;
	struct tp_chord_overlay *chord_overlay;
	char config_path[PATH_MAX];
	struct tp_event_queue events;

	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wl_listener renderer_lost;
	uint32_t renderer_recoveries;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layer[TP_LAYER_COUNT];
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;

	struct wl_list monitors;	/* tp_monitor.link */
	struct wl_list parked_monitors; /* tp_parked_monitor.link, newest first */
	struct tp_monitor *selmon;	/* monitor that receives new windows */
	struct wl_listener new_output;
	struct wl_listener layout_change;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_list clients;		/* tp_client.link, stacking-independent */
	struct wl_list focus_stack; /* tp_client.flink, most recent first */
	uint64_t next_client_id;
	struct tp_client *focused_client; /* explicitly focused window, or NULL */
	struct wl_listener new_toplevel;
	struct wl_listener new_popup;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;
	struct tp_layer_surface *focused_layer;

	struct wlr_security_context_manager_v1 *security_context_mgr;
	struct wl_client *shell_client; /* allowed exclusive OVERLAY focus */
	struct wl_listener shell_client_destroy;
	struct wl_client *locker_client; /* granted ext_session_lock_manager_v1 */
	struct wl_listener locker_client_destroy;
	struct wl_list capture_clients; /* tp_capture_client.link */

	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;
	struct wlr_ext_foreign_toplevel_list_v1 *ext_foreign_toplevel_list;

	struct wlr_ext_workspace_manager_v1 *workspace_mgr;
	struct wl_listener workspace_commit;

	struct wlr_session_lock_manager_v1 *session_lock_mgr;
	struct wl_listener new_session_lock;
	struct wlr_session_lock_v1 *active_lock;
	struct wl_listener lock_new_surface;
	struct wl_listener lock_unlock;
	struct wl_listener lock_destroy;
	struct wl_list lock_surfaces; /* tp_lock_surface.link */
	struct wlr_scene_tree *lock_bg_tree;
	bool locked;

	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wl_listener new_idle_inhibitor;
	struct wl_list idle_inhibitors; /* tp_idle_inhibitor.link */

	struct wlr_output_power_manager_v1 *output_power_mgr;
	struct wl_listener output_power_set_mode;

	struct wlr_screencopy_manager_v1 *screencopy_mgr;
	struct wlr_ext_image_copy_capture_manager_v1 *image_copy_capture_mgr;
	struct wlr_ext_output_image_capture_source_manager_v1 *output_capture_source_mgr;
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1
		*toplevel_capture_source_mgr;
	struct wl_listener new_toplevel_capture_request;

	struct wlr_xdg_output_manager_v1 *xdg_output_mgr;
	struct wlr_output_manager_v1 *output_mgr;
	struct wl_listener output_mgr_apply;
	struct wl_listener output_mgr_test;
	bool output_mgmt_applying;
	struct wlr_gamma_control_manager_v1 *gamma_mgr;

	struct wlr_text_input_manager_v3 *text_input_mgr;
	struct wlr_input_method_manager_v2 *input_method_mgr;
	struct tp_text_input_relay *im_relay;

	struct wlr_xdg_activation_v1 *xdg_activation;
	struct wl_listener request_activate;

	struct wlr_primary_selection_v1_device_manager *primary_selection_mgr;
	struct wlr_data_control_manager_v1 *data_control_mgr;
	struct wlr_ext_data_control_manager_v1 *ext_data_control_mgr;

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
	struct wl_listener new_xdg_decoration;

	struct wlr_xwayland *xwayland;
	const char *xwayland_display;
	struct wl_listener xwayland_ready;
	struct wl_listener xwayland_new_surface;

	struct wlr_seat *seat;
	struct wl_list keyboards; /* tp_keyboard.link */
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct wl_listener request_cursor_shape;
	const char *cursor_shape_name;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct tp_pointer_grab grab;

	struct wl_event_source *sigterm, *sigint, *sighup;
	struct wl_event_source *reload_idle;
	struct wl_event_source *arrange_idle;
	struct wl_event_source *renderer_lost_idle;
	/* Switch to VT n (1..12) for Ctrl+Alt+F-keys. Set only when tam-pane
	 * owns a session (DRM); NULL nested or headless. Tests may set their
	 * own. */
	bool (*change_vt)(struct tp_server *s, unsigned int vt);
	bool quit_requested; /* set by tp_server_quit */
	bool stopping; /* set while tearing down: no more layout or focus */
	const char *socket;
	struct tp_ipc_server *ipc;
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
