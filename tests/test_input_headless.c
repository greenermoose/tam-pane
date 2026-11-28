/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise input delivery to real Wayland clients on a private compositor. */
#define _GNU_SOURCE
#include "check.h"
#include "tam-pane/client.h"
#include "tam-pane/input.h"
#include "tam-pane/ipc.h"
#include "tam-pane/focus.h"
#include "tam-pane/layer.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/output_power.h"
#include "tam-pane/security.h"
#include "tam-pane/session_lock.h"
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_scene.h>
#include "tam-pane/server.h"
#include "tam-pane/shortcuts_inhibit.h"
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "security-context-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static const struct wlr_keyboard_impl keyboard_impl = { .name = "test" };
static const struct wlr_pointer_impl pointer_impl = { .name = "test" };

struct test_window {
	struct test_client *client;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer; /* NULL: the client's shared 2x2 buffer */
	int32_t width, height;
	bool configured, closed;
};

struct test_ft_wlr {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char title[64];
	char app_id[64];
	bool activated, fullscreen, output_entered, closed;
};

struct test_ft_ext {
	struct ext_foreign_toplevel_handle_v1 *handle;
	char title[64];
	char app_id[64];
	bool closed;
};

struct test_ws_item {
	struct ext_workspace_handle_v1 *handle;
	char id[64];
	char name[32];
	uint32_t state;
	bool removed;
};

struct test_client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_output *output;
	struct wl_output *outputs[8];
	size_t output_count;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct xdg_wm_base *wm_base;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_security_context_manager_v1 *security_context_mgr;
	struct zwlr_foreign_toplevel_manager_v1 *wlr_ft_mgr;
	struct ext_foreign_toplevel_list_v1 *ext_ft_list;
	struct ext_workspace_manager_v1 *ws_mgr;
	struct ext_session_lock_manager_v1 *lock_mgr;
	struct ext_idle_notifier_v1 *idle_notifier;
	struct zwp_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct zwp_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit_mgr;
	struct zwlr_output_power_manager_v1 *output_power_mgr;
	struct zwlr_screencopy_manager_v1 *screencopy_mgr;
	struct ext_image_copy_capture_manager_v1 *image_copy_mgr;
	struct ext_output_image_capture_source_manager_v1 *output_src_mgr;
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_src_mgr;
	struct zwlr_output_manager_v1 *output_mgr;
	struct zwlr_gamma_control_manager_v1 *gamma_mgr;
	struct wp_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct zxdg_output_manager_v1 *xdg_output_mgr;
	struct zwp_text_input_manager_v3 *text_input_mgr;
	struct zwp_input_method_manager_v2 *input_method_mgr;
	struct xdg_activation_v1 *xdg_activation;
	struct zwp_primary_selection_device_manager_v1 *primary_selection_mgr;
	struct zwlr_data_control_manager_v1 *wlr_data_control_mgr;
	struct ext_data_control_manager_v1 *ext_data_control_mgr;
	struct zxdg_decoration_manager_v1 *xdg_decoration_mgr;
	uint32_t layer_shell_name;
	uint32_t lock_mgr_name;
	uint32_t screencopy_mgr_name;
	uint32_t output_mgr_name;
	uint32_t pointer_enter_serial;
	unsigned int unclassified_globals;
	bool has_viewporter, has_fractional_scale, has_presentation;
	bool has_wlr_foreign_toplevel, has_ext_foreign_toplevel;
	bool has_ext_workspace, has_ext_session_lock;
	bool has_idle_notify, has_idle_inhibit, has_shortcuts_inhibit, has_output_power;
	bool has_screencopy, has_image_copy, has_output_capture_src, has_toplevel_capture_src;
	bool has_output_mgmt, has_gamma_control, has_cursor_shape, has_xdg_output;
	bool has_text_input, has_input_method, has_xdg_activation;
	bool has_primary_selection, has_wlr_data_control, has_ext_data_control;
	bool has_xdg_decoration;
	struct test_ft_wlr ft_wlr[8];
	size_t ft_wlr_count;
	struct test_ft_ext ft_ext[8];
	size_t ft_ext_count;
	struct test_ws_item ws_items[32];
	size_t ws_count;
	size_t ws_group_count;
	struct test_window windows[3];
	struct wl_surface *keyboard_focus;
	unsigned int key_events, pointer_enters, pointer_motions, pointer_buttons;
	uint32_t keys[8], key_states[8];
	bool slash_on_enter, caps_on_enter;
	uint32_t seen_locked;
	uint32_t depressed, key_mods[8];
	int shm_fd;
	void *shm_data;
	struct wl_buffer *buffer;
};

static void client_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = client_ping };

static void window_configure(void *data, struct xdg_surface *surface,
		uint32_t serial) {
	struct test_window *window = data;
	xdg_surface_ack_configure(surface, serial);
	if (!window->configured) {
		window->configured = true;
		wl_surface_attach(window->surface,
			window->buffer ? window->buffer : window->client->buffer, 0, 0);
		wl_surface_damage(window->surface, 0, 0, 2, 2);
		wl_surface_commit(window->surface);
	}
}

static void window_toplevel_configure(void *data, struct xdg_toplevel *top,
		int32_t width, int32_t height, struct wl_array *states) {
	struct test_window *window = data;
	if (width > 0) window->width = width;
	if (height > 0) window->height = height;
}

static void window_close(void *data, struct xdg_toplevel *top) {
	((struct test_window *)data)->closed = true;
}

static const struct xdg_surface_listener surface_listener = {
	.configure = window_configure,
};

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = window_toplevel_configure,
	.close = window_close,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	struct test_client *client = data;
	client->keyboard_focus = surface;
	client->slash_on_enter = false;
	client->caps_on_enter = false;
	uint32_t *key;
	wl_array_for_each(key, keys) {
		if (*key == KEY_SLASH)
			client->slash_on_enter = true;
		if (*key == KEY_CAPSLOCK)
			client->caps_on_enter = true;
	}
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface) {
	struct test_client *client = data;
	if (client->keyboard_focus == surface) client->keyboard_focus = NULL;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	struct test_client *client = data;
	if (client->key_events < 8) {
		client->keys[client->key_events] = key;
		client->key_states[client->key_events] = state;
		client->key_mods[client->key_events] = client->depressed;
	}
	client->key_events++;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t depressed, uint32_t latched,
		uint32_t locked, uint32_t group) {
	struct test_client *client = data;
	client->depressed = depressed;
	client->seen_locked |= locked;
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard,
		int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface, wl_fixed_t sx,
		wl_fixed_t sy) {
	struct test_client *client = data;
	client->pointer_enters++;
	client->pointer_enter_serial = serial;
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface) {}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
	((struct test_client *)data)->pointer_motions++;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	((struct test_client *)data)->pointer_buttons++;
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {}

static void pointer_frame(void *data, struct wl_pointer *pointer) {}
static void pointer_axis_source(void *data, struct wl_pointer *pointer,
		uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		uint32_t axis, int32_t discrete) {}
static void pointer_axis_value120(void *data, struct wl_pointer *pointer,
		uint32_t axis, int32_t value120) {}
static void pointer_axis_relative_direction(void *data,
		struct wl_pointer *pointer, uint32_t axis, uint32_t direction) {}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities) {
	struct test_client *client = data;
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !client->keyboard) {
		client->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
	}
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !client->pointer) {
		client->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(client->pointer, &pointer_listener, client);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- 0.2.0 protocol client listeners ---- */

static void ft_wlr_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
		const char *title) {
	struct test_ft_wlr *ft = data;
	snprintf(ft->title, sizeof(ft->title), "%s", title ? title : "");
}
static void ft_wlr_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
		const char *app_id) {
	struct test_ft_wlr *ft = data;
	snprintf(ft->app_id, sizeof(ft->app_id), "%s", app_id ? app_id : "");
}
static void ft_wlr_output_enter(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
	((struct test_ft_wlr *)data)->output_entered = true;
}
static void ft_wlr_output_leave(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
	((struct test_ft_wlr *)data)->output_entered = false;
}
static void ft_wlr_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
		struct wl_array *state) {
	struct test_ft_wlr *ft = data;
	ft->activated = false;
	ft->fullscreen = false;
	uint32_t *p;
	wl_array_for_each(p, state) {
		if (*p == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
			ft->activated = true;
		if (*p == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN)
			ft->fullscreen = true;
	}
}
static void ft_wlr_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {}
static void ft_wlr_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
	((struct test_ft_wlr *)data)->closed = true;
}
static void ft_wlr_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
		struct zwlr_foreign_toplevel_handle_v1 *parent) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener ft_wlr_handle_listener = {
	.title = ft_wlr_title,
	.app_id = ft_wlr_app_id,
	.output_enter = ft_wlr_output_enter,
	.output_leave = ft_wlr_output_leave,
	.state = ft_wlr_state,
	.done = ft_wlr_done,
	.closed = ft_wlr_closed,
	.parent = ft_wlr_parent,
};

static void ft_wlr_mgr_toplevel(void *data,
		struct zwlr_foreign_toplevel_manager_v1 *mgr,
		struct zwlr_foreign_toplevel_handle_v1 *handle) {
	struct test_client *c = data;
	if (c->ft_wlr_count < 8) {
		struct test_ft_wlr *ft = &c->ft_wlr[c->ft_wlr_count++];
		memset(ft, 0, sizeof(*ft));
		ft->handle = handle;
		zwlr_foreign_toplevel_handle_v1_add_listener(
			handle, &ft_wlr_handle_listener, ft);
	}
}
static void ft_wlr_mgr_finished(void *data,
		struct zwlr_foreign_toplevel_manager_v1 *mgr) {}
static const struct zwlr_foreign_toplevel_manager_v1_listener ft_wlr_mgr_listener = {
	.toplevel = ft_wlr_mgr_toplevel,
	.finished = ft_wlr_mgr_finished,
};

static void ft_ext_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
	((struct test_ft_ext *)data)->closed = true;
}
static void ft_ext_done(void *data, struct ext_foreign_toplevel_handle_v1 *h) {}
static void ft_ext_title(void *data, struct ext_foreign_toplevel_handle_v1 *h,
		const char *title) {
	struct test_ft_ext *ft = data;
	snprintf(ft->title, sizeof(ft->title), "%s", title ? title : "");
}
static void ft_ext_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h,
		const char *app_id) {
	struct test_ft_ext *ft = data;
	snprintf(ft->app_id, sizeof(ft->app_id), "%s", app_id ? app_id : "");
}
static void ft_ext_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *h,
		const char *id) {}
static const struct ext_foreign_toplevel_handle_v1_listener ft_ext_handle_listener = {
	.closed = ft_ext_closed,
	.done = ft_ext_done,
	.title = ft_ext_title,
	.app_id = ft_ext_app_id,
	.identifier = ft_ext_identifier,
};

static void ft_ext_list_toplevel(void *data,
		struct ext_foreign_toplevel_list_v1 *list,
		struct ext_foreign_toplevel_handle_v1 *handle) {
	struct test_client *c = data;
	if (c->ft_ext_count < 8) {
		struct test_ft_ext *ft = &c->ft_ext[c->ft_ext_count++];
		memset(ft, 0, sizeof(*ft));
		ft->handle = handle;
		ext_foreign_toplevel_handle_v1_add_listener(
			handle, &ft_ext_handle_listener, ft);
	}
}
static void ft_ext_list_finished(void *data,
		struct ext_foreign_toplevel_list_v1 *list) {}
static const struct ext_foreign_toplevel_list_v1_listener ft_ext_list_listener = {
	.toplevel = ft_ext_list_toplevel,
	.finished = ft_ext_list_finished,
};

static void ws_id(void *data, struct ext_workspace_handle_v1 *h, const char *id) {
	struct test_ws_item *w = data;
	snprintf(w->id, sizeof(w->id), "%s", id ? id : "");
}
static void ws_name(void *data, struct ext_workspace_handle_v1 *h, const char *name) {
	struct test_ws_item *w = data;
	snprintf(w->name, sizeof(w->name), "%s", name ? name : "");
}
static void ws_coords(void *data, struct ext_workspace_handle_v1 *h,
		struct wl_array *coords) {}
static void ws_state(void *data, struct ext_workspace_handle_v1 *h, uint32_t state) {
	((struct test_ws_item *)data)->state = state;
}
static void ws_caps(void *data, struct ext_workspace_handle_v1 *h, uint32_t caps) {}
static void ws_removed(void *data, struct ext_workspace_handle_v1 *h) {
	((struct test_ws_item *)data)->removed = true;
}
static const struct ext_workspace_handle_v1_listener ws_handle_listener = {
	.id = ws_id,
	.name = ws_name,
	.coordinates = ws_coords,
	.state = ws_state,
	.capabilities = ws_caps,
	.removed = ws_removed,
};

static void ws_grp_caps(void *data, struct ext_workspace_group_handle_v1 *g,
		uint32_t caps) {}
static void ws_grp_out_enter(void *data, struct ext_workspace_group_handle_v1 *g,
		struct wl_output *o) {}
static void ws_grp_out_leave(void *data, struct ext_workspace_group_handle_v1 *g,
		struct wl_output *o) {}
static void ws_grp_ws_enter(void *data, struct ext_workspace_group_handle_v1 *g,
		struct ext_workspace_handle_v1 *w) {}
static void ws_grp_ws_leave(void *data, struct ext_workspace_group_handle_v1 *g,
		struct ext_workspace_handle_v1 *w) {}
static void ws_grp_removed(void *data, struct ext_workspace_group_handle_v1 *g) {}
static const struct ext_workspace_group_handle_v1_listener ws_group_listener = {
	.capabilities = ws_grp_caps,
	.output_enter = ws_grp_out_enter,
	.output_leave = ws_grp_out_leave,
	.workspace_enter = ws_grp_ws_enter,
	.workspace_leave = ws_grp_ws_leave,
	.removed = ws_grp_removed,
};

static void ws_mgr_group(void *data, struct ext_workspace_manager_v1 *mgr,
		struct ext_workspace_group_handle_v1 *grp) {
	struct test_client *c = data;
	c->ws_group_count++;
	ext_workspace_group_handle_v1_add_listener(grp, &ws_group_listener, c);
}
static void ws_mgr_workspace(void *data, struct ext_workspace_manager_v1 *mgr,
		struct ext_workspace_handle_v1 *ws) {
	struct test_client *c = data;
	if (c->ws_count < 32) {
		struct test_ws_item *item = &c->ws_items[c->ws_count++];
		memset(item, 0, sizeof(*item));
		item->handle = ws;
		ext_workspace_handle_v1_add_listener(ws, &ws_handle_listener, item);
	}
}
static void ws_mgr_done(void *data, struct ext_workspace_manager_v1 *mgr) {}
static void ws_mgr_finished(void *data, struct ext_workspace_manager_v1 *mgr) {}
static const struct ext_workspace_manager_v1_listener ws_mgr_listener = {
	.workspace_group = ws_mgr_group,
	.workspace = ws_mgr_workspace,
	.done = ws_mgr_done,
	.finished = ws_mgr_finished,
};

struct test_lock_state {
	bool locked;
	bool finished;
};
static void lock_locked(void *data, struct ext_session_lock_v1 *lock) {
	((struct test_lock_state *)data)->locked = true;
}
static void lock_finished(void *data, struct ext_session_lock_v1 *lock) {
	((struct test_lock_state *)data)->finished = true;
}
static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

struct test_lock_surf_state {
	struct test_client *client;
	struct wl_surface *surface;
	bool configured;
	uint32_t width, height;
};
static struct wl_buffer *create_sized_buffer(struct test_client *client,
		int32_t w, int32_t h);

static void lock_surf_configure(void *data,
		struct ext_session_lock_surface_v1 *ls, uint32_t serial,
		uint32_t width, uint32_t height) {
	struct test_lock_surf_state *st = data;
	st->width = width;
	st->height = height;
	ext_session_lock_surface_v1_ack_configure(ls, serial);
	if (!st->configured && st->client->shm && st->width > 0 && st->height > 0) {
		st->configured = true;
		struct wl_buffer *buf = create_sized_buffer(
			st->client, (int32_t)st->width, (int32_t)st->height);
		wl_surface_attach(st->surface, buf, 0, 0);
		wl_surface_damage(st->surface, 0, 0, (int32_t)st->width,
						  (int32_t)st->height);
		wl_surface_commit(st->surface);
	}
}
static const struct ext_session_lock_surface_v1_listener lock_surf_listener = {
	.configure = lock_surf_configure,
};

struct test_idle_state {
	bool idled;
	bool resumed;
};
static void idle_idled(void *data, struct ext_idle_notification_v1 *n) {
	((struct test_idle_state *)data)->idled = true;
}
static void idle_resumed(void *data, struct ext_idle_notification_v1 *n) {
	((struct test_idle_state *)data)->resumed = true;
}
static const struct ext_idle_notification_v1_listener idle_notif_listener = {
	.idled = idle_idled,
	.resumed = idle_resumed,
};

struct test_power_state {
	bool got_mode;
	uint32_t mode;
	bool failed;
};
static void power_mode(void *data, struct zwlr_output_power_v1 *p,
		uint32_t mode) {
	struct test_power_state *st = data;
	st->got_mode = true;
	st->mode = mode;
}
static void power_failed(void *data, struct zwlr_output_power_v1 *p) {
	((struct test_power_state *)data)->failed = true;
}
static const struct zwlr_output_power_v1_listener power_listener = {
	.mode = power_mode,
	.failed = power_failed,
};

struct test_screencopy_state {
	bool got_buffer, buffer_done, ready, failed;
	uint32_t format, width, height, stride;
};
static void sc_buffer(void *data, struct zwlr_screencopy_frame_v1 *f,
		uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
	struct test_screencopy_state *st = data;
	st->got_buffer = true;
	st->format = format;
	st->width = w;
	st->height = h;
	st->stride = stride;
}
static void sc_flags(void *data, struct zwlr_screencopy_frame_v1 *f,
		uint32_t flags) { (void)data; (void)f; (void)flags; }
static void sc_ready(void *data, struct zwlr_screencopy_frame_v1 *f,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
	(void)f; (void)sec_hi; (void)sec_lo; (void)nsec;
	((struct test_screencopy_state *)data)->ready = true;
}
static void sc_failed(void *data, struct zwlr_screencopy_frame_v1 *f) {
	(void)f;
	((struct test_screencopy_state *)data)->failed = true;
}
static void sc_damage(void *data, struct zwlr_screencopy_frame_v1 *f,
		uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	(void)data; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void sc_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f,
		uint32_t fmt, uint32_t w, uint32_t h) {
	(void)data; (void)f; (void)fmt; (void)w; (void)h;
}
static void sc_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f) {
	(void)f;
	((struct test_screencopy_state *)data)->buffer_done = true;
}
static const struct zwlr_screencopy_frame_v1_listener sc_frame_listener = {
	.buffer = sc_buffer,
	.flags = sc_flags,
	.ready = sc_ready,
	.failed = sc_failed,
	.damage = sc_damage,
	.linux_dmabuf = sc_linux_dmabuf,
	.buffer_done = sc_buffer_done,
};

struct test_icc_session_state {
	uint32_t width, height;
	uint32_t shm_format;
	bool has_shm_format, done, stopped;
};
static void icc_buffer_size(void *data,
		struct ext_image_copy_capture_session_v1 *s, uint32_t w, uint32_t h) {
	(void)s;
	struct test_icc_session_state *st = data;
	st->width = w;
	st->height = h;
}
static void icc_shm_format(void *data,
		struct ext_image_copy_capture_session_v1 *s, uint32_t fmt) {
	(void)s;
	struct test_icc_session_state *st = data;
	if (!st->has_shm_format || fmt == WL_SHM_FORMAT_ARGB8888 ||
		fmt == WL_SHM_FORMAT_XRGB8888) {
		st->shm_format = fmt;
		st->has_shm_format = true;
	}
}
static void icc_dmabuf_device(void *data,
		struct ext_image_copy_capture_session_v1 *s, struct wl_array *dev) {
	(void)data; (void)s; (void)dev;
}
static void icc_dmabuf_format(void *data,
		struct ext_image_copy_capture_session_v1 *s, uint32_t fmt,
		struct wl_array *mods) {
	(void)data; (void)s; (void)fmt; (void)mods;
}
static void icc_done(void *data, struct ext_image_copy_capture_session_v1 *s) {
	(void)s;
	((struct test_icc_session_state *)data)->done = true;
}
static void icc_stopped(void *data,
		struct ext_image_copy_capture_session_v1 *s) {
	(void)s;
	((struct test_icc_session_state *)data)->stopped = true;
}
static const struct ext_image_copy_capture_session_v1_listener
	icc_session_listener = {
	.buffer_size = icc_buffer_size,
	.shm_format = icc_shm_format,
	.dmabuf_device = icc_dmabuf_device,
	.dmabuf_format = icc_dmabuf_format,
	.done = icc_done,
	.stopped = icc_stopped,
};

struct test_icc_frame_state {
	bool ready, failed;
};
static void iccf_transform(void *data,
		struct ext_image_copy_capture_frame_v1 *f, uint32_t transform) {
	(void)data; (void)f; (void)transform;
}
static void iccf_damage(void *data, struct ext_image_copy_capture_frame_v1 *f,
		int32_t x, int32_t y, int32_t w, int32_t h) {
	(void)data; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void iccf_presentation_time(void *data,
		struct ext_image_copy_capture_frame_v1 *f, uint32_t hi, uint32_t lo,
		uint32_t ns) {
	(void)data; (void)f; (void)hi; (void)lo; (void)ns;
}
static void iccf_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
	(void)f;
	((struct test_icc_frame_state *)data)->ready = true;
}
static void iccf_failed(void *data, struct ext_image_copy_capture_frame_v1 *f,
		uint32_t reason) {
	(void)f; (void)reason;
	((struct test_icc_frame_state *)data)->failed = true;
}
static const struct ext_image_copy_capture_frame_v1_listener
	icc_frame_listener = {
	.transform = iccf_transform,
	.damage = iccf_damage,
	.presentation_time = iccf_presentation_time,
	.ready = iccf_ready,
	.failed = iccf_failed,
};

struct test_om_head {
	struct zwlr_output_head_v1 *head;
	char name[64];
	bool enabled, finished;
	int32_t x, y;
	double scale;
};
struct test_om_state {
	struct test_om_head heads[8];
	size_t head_count;
	uint32_t serial;
	bool got_done;
};
static void om_head_name(void *data, struct zwlr_output_head_v1 *h,
		const char *name) {
	(void)h;
	struct test_om_head *th = data;
	snprintf(th->name, sizeof(th->name), "%s", name ? name : "");
}
static void om_head_desc(void *data, struct zwlr_output_head_v1 *h,
		const char *d) { (void)data; (void)h; (void)d; }
static void om_head_phys(void *data, struct zwlr_output_head_v1 *h,
		int32_t w, int32_t hh) { (void)data; (void)h; (void)w; (void)hh; }
static void om_head_mode(void *data, struct zwlr_output_head_v1 *h,
		struct zwlr_output_mode_v1 *m) { (void)data; (void)h; (void)m; }
static void om_head_enabled(void *data, struct zwlr_output_head_v1 *h,
		int32_t en) {
	(void)h;
	((struct test_om_head *)data)->enabled = en != 0;
}
static void om_head_current_mode(void *data, struct zwlr_output_head_v1 *h,
		struct zwlr_output_mode_v1 *m) { (void)data; (void)h; (void)m; }
static void om_head_position(void *data, struct zwlr_output_head_v1 *h,
		int32_t x, int32_t y) {
	(void)h;
	struct test_om_head *th = data;
	th->x = x;
	th->y = y;
}
static void om_head_transform(void *data, struct zwlr_output_head_v1 *h,
		int32_t t) { (void)data; (void)h; (void)t; }
static void om_head_scale(void *data, struct zwlr_output_head_v1 *h,
		wl_fixed_t s) {
	(void)h;
	((struct test_om_head *)data)->scale = wl_fixed_to_double(s);
}
static void om_head_finished(void *data, struct zwlr_output_head_v1 *h) {
	(void)h;
	((struct test_om_head *)data)->finished = true;
}
static void om_head_make(void *data, struct zwlr_output_head_v1 *h,
		const char *s) { (void)data; (void)h; (void)s; }
static void om_head_model(void *data, struct zwlr_output_head_v1 *h,
		const char *s) { (void)data; (void)h; (void)s; }
static void om_head_serial_number(void *data, struct zwlr_output_head_v1 *h,
		const char *s) { (void)data; (void)h; (void)s; }
static void om_head_adaptive_sync(void *data, struct zwlr_output_head_v1 *h,
		uint32_t st) { (void)data; (void)h; (void)st; }
static const struct zwlr_output_head_v1_listener om_head_listener = {
	.name = om_head_name,
	.description = om_head_desc,
	.physical_size = om_head_phys,
	.mode = om_head_mode,
	.enabled = om_head_enabled,
	.current_mode = om_head_current_mode,
	.position = om_head_position,
	.transform = om_head_transform,
	.scale = om_head_scale,
	.finished = om_head_finished,
	.make = om_head_make,
	.model = om_head_model,
	.serial_number = om_head_serial_number,
	.adaptive_sync = om_head_adaptive_sync,
};
static void om_mgr_head(void *data, struct zwlr_output_manager_v1 *m,
		struct zwlr_output_head_v1 *h) {
	(void)m;
	struct test_om_state *st = data;
	if (st->head_count >= 8)
		return;
	struct test_om_head *th = &st->heads[st->head_count++];
	memset(th, 0, sizeof(*th));
	th->head = h;
	zwlr_output_head_v1_add_listener(h, &om_head_listener, th);
}
static void om_mgr_done(void *data, struct zwlr_output_manager_v1 *m,
		uint32_t serial) {
	(void)m;
	struct test_om_state *st = data;
	st->serial = serial;
	st->got_done = true;
}
static void om_mgr_finished(void *data, struct zwlr_output_manager_v1 *m) {
	(void)data; (void)m;
}
static const struct zwlr_output_manager_v1_listener om_mgr_listener = {
	.head = om_mgr_head,
	.done = om_mgr_done,
	.finished = om_mgr_finished,
};

struct test_om_cfg_state {
	bool succeeded, failed, cancelled;
};
static void om_cfg_succeeded(void *data,
		struct zwlr_output_configuration_v1 *c) {
	(void)c;
	((struct test_om_cfg_state *)data)->succeeded = true;
}
static void om_cfg_failed(void *data, struct zwlr_output_configuration_v1 *c) {
	(void)c;
	((struct test_om_cfg_state *)data)->failed = true;
}
static void om_cfg_cancelled(void *data,
		struct zwlr_output_configuration_v1 *c) {
	(void)c;
	((struct test_om_cfg_state *)data)->cancelled = true;
}
static const struct zwlr_output_configuration_v1_listener om_cfg_listener = {
	.succeeded = om_cfg_succeeded,
	.failed = om_cfg_failed,
	.cancelled = om_cfg_cancelled,
};

struct test_gamma_state {
	bool got_size, failed;
	uint32_t size;
};
static void gamma_size_cb(void *data, struct zwlr_gamma_control_v1 *g,
		uint32_t size) {
	(void)g;
	struct test_gamma_state *st = data;
	st->got_size = true;
	st->size = size;
}
static void gamma_failed_cb(void *data, struct zwlr_gamma_control_v1 *g) {
	(void)g;
	((struct test_gamma_state *)data)->failed = true;
}
static const struct zwlr_gamma_control_v1_listener gamma_listener = {
	.gamma_size = gamma_size_cb,
	.failed = gamma_failed_cb,
};

struct test_xdg_out_state {
	int32_t x, y, w, h;
	char name[64];
	bool done;
};
static void xo_pos(void *data, struct zxdg_output_v1 *xo, int32_t x, int32_t y) {
	(void)xo;
	struct test_xdg_out_state *st = data;
	st->x = x;
	st->y = y;
}
static void xo_size(void *data, struct zxdg_output_v1 *xo, int32_t w, int32_t h) {
	(void)xo;
	struct test_xdg_out_state *st = data;
	st->w = w;
	st->h = h;
}
static void xo_done(void *data, struct zxdg_output_v1 *xo) {
	(void)xo;
	((struct test_xdg_out_state *)data)->done = true;
}
static void xo_name(void *data, struct zxdg_output_v1 *xo, const char *name) {
	(void)xo;
	struct test_xdg_out_state *st = data;
	snprintf(st->name, sizeof(st->name), "%s", name ? name : "");
}
static void xo_desc(void *data, struct zxdg_output_v1 *xo, const char *d) {
	(void)data; (void)xo; (void)d;
}
static const struct zxdg_output_v1_listener xdg_out_listener = {
	.logical_position = xo_pos,
	.logical_size = xo_size,
	.done = xo_done,
	.name = xo_name,
	.description = xo_desc,
};

/* ---- 0.2.2 protocol client listeners ---- */

struct test_decor_state {
	uint32_t mode;
	unsigned int configures;
};
static void decor_configure(void *data,
		struct zxdg_toplevel_decoration_v1 *d, uint32_t mode) {
	(void)d;
	struct test_decor_state *st = data;
	st->mode = mode;
	st->configures++;
}
static const struct zxdg_toplevel_decoration_v1_listener decor_listener = {
	.configure = decor_configure,
};

struct test_act_token_state {
	char token[128];
	bool done;
};
static void act_token_done(void *data, struct xdg_activation_token_v1 *t,
		const char *token) {
	(void)t;
	struct test_act_token_state *st = data;
	snprintf(st->token, sizeof(st->token), "%s", token ? token : "");
	st->done = true;
}
static const struct xdg_activation_token_v1_listener act_token_listener = {
	.done = act_token_done,
};

static void wlr_dc_src_send(void *data, struct zwlr_data_control_source_v1 *s,
		const char *mime, int32_t fd) {
	(void)data; (void)s; (void)mime;
	const char msg[] = "clip-ok";
	ssize_t n = write(fd, msg, sizeof(msg) - 1);
	(void)n;
	close(fd);
}
static void wlr_dc_src_cancelled(void *data,
		struct zwlr_data_control_source_v1 *s) {
	(void)data; (void)s;
}
static const struct zwlr_data_control_source_v1_listener wlr_dc_src_listener = {
	.send = wlr_dc_src_send,
	.cancelled = wlr_dc_src_cancelled,
};

struct test_ext_dc_state {
	struct ext_data_control_offer_v1 *sel_offer;
	struct ext_data_control_offer_v1 *prim_offer;
	char last_mime[64];
	bool got_sel, got_prim;
};
static void ext_dc_offer_mime(void *data, struct ext_data_control_offer_v1 *o,
		const char *mime) {
	(void)o;
	struct test_ext_dc_state *st = data;
	snprintf(st->last_mime, sizeof(st->last_mime), "%s", mime ? mime : "");
}
static const struct ext_data_control_offer_v1_listener ext_dc_offer_listener = {
	.offer = ext_dc_offer_mime,
};
static void ext_dc_data_offer(void *data, struct ext_data_control_device_v1 *d,
		struct ext_data_control_offer_v1 *offer) {
	(void)d;
	ext_data_control_offer_v1_add_listener(offer, &ext_dc_offer_listener, data);
}
static void ext_dc_selection(void *data, struct ext_data_control_device_v1 *d,
		struct ext_data_control_offer_v1 *offer) {
	(void)d;
	struct test_ext_dc_state *st = data;
	if (st->sel_offer)
		ext_data_control_offer_v1_destroy(st->sel_offer);
	st->sel_offer = offer;
	if (offer)
		st->got_sel = true;
}
static void ext_dc_finished(void *data, struct ext_data_control_device_v1 *d) {
	(void)data; (void)d;
}
static void ext_dc_primary_selection(void *data,
		struct ext_data_control_device_v1 *d,
		struct ext_data_control_offer_v1 *offer) {
	(void)d;
	struct test_ext_dc_state *st = data;
	if (st->prim_offer)
		ext_data_control_offer_v1_destroy(st->prim_offer);
	st->prim_offer = offer;
	if (offer)
		st->got_prim = true;
}
static const struct ext_data_control_device_v1_listener ext_dc_dev_listener = {
	.data_offer = ext_dc_data_offer,
	.selection = ext_dc_selection,
	.finished = ext_dc_finished,
	.primary_selection = ext_dc_primary_selection,
};

struct test_ti_state {
	struct wl_surface *entered;
	unsigned int enters, leaves, dones;
	char preedit[64];
	char commit[64];
};
static void ti_enter(void *data, struct zwp_text_input_v3 *ti,
		struct wl_surface *surface) {
	(void)ti;
	struct test_ti_state *st = data;
	st->entered = surface;
	st->enters++;
}
static void ti_leave(void *data, struct zwp_text_input_v3 *ti,
		struct wl_surface *surface) {
	(void)ti; (void)surface;
	struct test_ti_state *st = data;
	st->entered = NULL;
	st->leaves++;
}
static void ti_preedit(void *data, struct zwp_text_input_v3 *ti,
		const char *text, int32_t begin, int32_t end) {
	(void)ti; (void)begin; (void)end;
	struct test_ti_state *st = data;
	snprintf(st->preedit, sizeof(st->preedit), "%s", text ? text : "");
}
static void ti_commit(void *data, struct zwp_text_input_v3 *ti,
		const char *text) {
	(void)ti;
	struct test_ti_state *st = data;
	snprintf(st->commit, sizeof(st->commit), "%s", text ? text : "");
}
static void ti_delete(void *data, struct zwp_text_input_v3 *ti,
		uint32_t before, uint32_t after) {
	(void)data; (void)ti; (void)before; (void)after;
}
static void ti_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial) {
	(void)ti; (void)serial;
	((struct test_ti_state *)data)->dones++;
}
static const struct zwp_text_input_v3_listener ti_listener = {
	.enter = ti_enter,
	.leave = ti_leave,
	.preedit_string = ti_preedit,
	.commit_string = ti_commit,
	.delete_surrounding_text = ti_delete,
	.done = ti_done,
};

struct test_im_state {
	bool pending_active, active, unavailable;
	unsigned int dones;
	char surrounding[64];
};
static void im_activate(void *data, struct zwp_input_method_v2 *im) {
	(void)im;
	((struct test_im_state *)data)->pending_active = true;
}
static void im_deactivate(void *data, struct zwp_input_method_v2 *im) {
	(void)im;
	((struct test_im_state *)data)->pending_active = false;
}
static void im_surrounding(void *data, struct zwp_input_method_v2 *im,
		const char *text, uint32_t cursor, uint32_t anchor) {
	(void)im; (void)cursor; (void)anchor;
	struct test_im_state *st = data;
	snprintf(st->surrounding, sizeof(st->surrounding), "%s", text ? text : "");
}
static void im_change_cause(void *data, struct zwp_input_method_v2 *im,
		uint32_t cause) {
	(void)data; (void)im; (void)cause;
}
static void im_content_type(void *data, struct zwp_input_method_v2 *im,
		uint32_t hint, uint32_t purpose) {
	(void)data; (void)im; (void)hint; (void)purpose;
}
static void im_done(void *data, struct zwp_input_method_v2 *im) {
	(void)im;
	struct test_im_state *st = data;
	st->active = st->pending_active;
	st->dones++;
}
static void im_unavailable(void *data, struct zwp_input_method_v2 *im) {
	(void)im;
	((struct test_im_state *)data)->unavailable = true;
}
static const struct zwp_input_method_v2_listener im_listener = {
	.activate = im_activate,
	.deactivate = im_deactivate,
	.surrounding_text = im_surrounding,
	.text_change_cause = im_change_cause,
	.content_type = im_content_type,
	.done = im_done,
	.unavailable = im_unavailable,
};

struct test_im_grab_state {
	unsigned int key_events;
	uint32_t last_key;
};
static void im_grab_keymap(void *data,
		struct zwp_input_method_keyboard_grab_v2 *g, uint32_t format,
		int32_t fd, uint32_t size) {
	(void)data; (void)g; (void)format; (void)size;
	close(fd);
}
static void im_grab_key(void *data,
		struct zwp_input_method_keyboard_grab_v2 *g, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	(void)g; (void)serial; (void)time; (void)state;
	struct test_im_grab_state *st = data;
	st->key_events++;
	st->last_key = key;
}
static void im_grab_modifiers(void *data,
		struct zwp_input_method_keyboard_grab_v2 *g, uint32_t serial,
		uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
	(void)data; (void)g; (void)serial; (void)dep; (void)lat; (void)lock; (void)grp;
}
static void im_grab_repeat(void *data,
		struct zwp_input_method_keyboard_grab_v2 *g, int32_t rate,
		int32_t delay) {
	(void)data; (void)g; (void)rate; (void)delay;
}
static const struct zwp_input_method_keyboard_grab_v2_listener im_grab_listener = {
	.keymap = im_grab_keymap,
	.key = im_grab_key,
	.modifiers = im_grab_modifiers,
	.repeat_info = im_grab_repeat,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct test_client *client = data;
	if (tp_security_classify_global(interface) == TP_GLOBAL_UNKNOWN) {
		fprintf(stderr, "unclassified global advertised: %s\n", interface);
		client->unclassified_globals++;
	}
	if (!strcmp(interface, wl_compositor_interface.name))
		client->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, version < 4 ? version : 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, wl_output_interface.name)) {
		struct wl_output *out = wl_registry_bind(registry, name,
			&wl_output_interface, version < 3 ? version : 3);
		if (!client->output)
			client->output = out;
		if (client->output_count < 8)
			client->outputs[client->output_count++] = out;
	}
	else if (!strcmp(interface, wl_seat_interface.name)) {
		client->seat = wl_registry_bind(registry, name, &wl_seat_interface,
			version < 5 ? version : 5);
		wl_seat_add_listener(client->seat, &seat_listener, client);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		/* Version 3 for xdg_popup.reposition. Version 4 would add
		 * toplevel events these listeners do not handle. */
		client->wm_base = wl_registry_bind(registry, name,
			&xdg_wm_base_interface, version < 3 ? version : 3);
		xdg_wm_base_add_listener(client->wm_base, &wm_listener, client);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		client->layer_shell_name = name;
		client->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
	} else if (!strcmp(interface, wp_security_context_manager_v1_interface.name)) {
		client->security_context_mgr = wl_registry_bind(registry, name,
			&wp_security_context_manager_v1_interface, 1);
	} else if (!strcmp(interface, "wp_viewporter")) {
		client->has_viewporter = true;
	} else if (!strcmp(interface, "wp_fractional_scale_manager_v1")) {
		client->has_fractional_scale = true;
	} else if (!strcmp(interface, "wp_presentation")) {
		client->has_presentation = true;
	} else if (!strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		client->has_wlr_foreign_toplevel = true;
		client->wlr_ft_mgr = wl_registry_bind(registry, name,
			&zwlr_foreign_toplevel_manager_v1_interface,
			version < 3 ? version : 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(
			client->wlr_ft_mgr, &ft_wlr_mgr_listener, client);
	} else if (!strcmp(interface, ext_foreign_toplevel_list_v1_interface.name)) {
		client->has_ext_foreign_toplevel = true;
		client->ext_ft_list = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(
			client->ext_ft_list, &ft_ext_list_listener, client);
	} else if (!strcmp(interface, ext_workspace_manager_v1_interface.name)) {
		client->has_ext_workspace = true;
		client->ws_mgr = wl_registry_bind(registry, name,
			&ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(
			client->ws_mgr, &ws_mgr_listener, client);
	} else if (!strcmp(interface, ext_session_lock_manager_v1_interface.name)) {
		client->has_ext_session_lock = true;
		client->lock_mgr_name = name;
		client->lock_mgr = wl_registry_bind(registry, name,
			&ext_session_lock_manager_v1_interface, 1);
	} else if (!strcmp(interface, ext_idle_notifier_v1_interface.name)) {
		client->has_idle_notify = true;
		client->idle_notifier = wl_registry_bind(registry, name,
			&ext_idle_notifier_v1_interface, 1);
	} else if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name)) {
		client->has_idle_inhibit = true;
		client->idle_inhibit_mgr = wl_registry_bind(registry, name,
			&zwp_idle_inhibit_manager_v1_interface, 1);
	} else if (!strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) {
		client->has_shortcuts_inhibit = true;
		client->shortcuts_inhibit_mgr = wl_registry_bind(registry, name,
			&zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
	} else if (!strcmp(interface, zwlr_output_power_manager_v1_interface.name)) {
		client->has_output_power = true;
		client->output_power_mgr = wl_registry_bind(registry, name,
			&zwlr_output_power_manager_v1_interface, 1);
	} else if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
		client->has_screencopy = true;
		client->screencopy_mgr_name = name;
		client->screencopy_mgr = wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface,
			version < 3 ? version : 3);
	} else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
		client->has_image_copy = true;
		client->image_copy_mgr = wl_registry_bind(registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
	} else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
		client->has_output_capture_src = true;
		client->output_src_mgr = wl_registry_bind(registry, name,
			&ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (!strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name)) {
		client->has_toplevel_capture_src = true;
		client->toplevel_src_mgr = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (!strcmp(interface, zwlr_output_manager_v1_interface.name)) {
		client->has_output_mgmt = true;
		client->output_mgr_name = name;
		client->output_mgr = wl_registry_bind(registry, name,
			&zwlr_output_manager_v1_interface, version < 4 ? version : 4);
	} else if (!strcmp(interface, zwlr_gamma_control_manager_v1_interface.name)) {
		client->has_gamma_control = true;
		client->gamma_mgr = wl_registry_bind(registry, name,
			&zwlr_gamma_control_manager_v1_interface, 1);
	} else if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name)) {
		client->has_cursor_shape = true;
		client->cursor_shape_mgr = wl_registry_bind(registry, name,
			&wp_cursor_shape_manager_v1_interface, 1);
	} else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
		client->has_xdg_output = true;
		client->xdg_output_mgr = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface, version < 3 ? version : 3);
	} else if (!strcmp(interface, zwp_text_input_manager_v3_interface.name)) {
		client->has_text_input = true;
		client->text_input_mgr = wl_registry_bind(registry, name,
			&zwp_text_input_manager_v3_interface, 1);
	} else if (!strcmp(interface, zwp_input_method_manager_v2_interface.name)) {
		client->has_input_method = true;
		client->input_method_mgr = wl_registry_bind(registry, name,
			&zwp_input_method_manager_v2_interface, 1);
	} else if (!strcmp(interface, xdg_activation_v1_interface.name)) {
		client->has_xdg_activation = true;
		client->xdg_activation = wl_registry_bind(registry, name,
			&xdg_activation_v1_interface, 1);
	} else if (!strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name)) {
		client->has_primary_selection = true;
		client->primary_selection_mgr = wl_registry_bind(registry, name,
			&zwp_primary_selection_device_manager_v1_interface, 1);
	} else if (!strcmp(interface, zwlr_data_control_manager_v1_interface.name)) {
		client->has_wlr_data_control = true;
		client->wlr_data_control_mgr = wl_registry_bind(registry, name,
			&zwlr_data_control_manager_v1_interface, version < 2 ? version : 2);
	} else if (!strcmp(interface, ext_data_control_manager_v1_interface.name)) {
		client->has_ext_data_control = true;
		client->ext_data_control_mgr = wl_registry_bind(registry, name,
			&ext_data_control_manager_v1_interface, 1);
	} else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
		client->has_xdg_decoration = true;
		client->xdg_decoration_mgr = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void registry_remove(void *data, struct wl_registry *registry,
		uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static bool pump(struct test_client *client, struct tp_server *server) {
	if (wl_display_flush(client->display) < 0) return false;
	if (wl_event_loop_dispatch(server->loop, 0) < 0) return false;
	wl_display_flush_clients(server->display);
	int fd = wl_display_get_fd(client->display);
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int ready = poll(&pfd, 1, 2);
	if (ready < 0) return false;
	if (ready && (pfd.revents & POLLIN)) {
		if (wl_display_dispatch(client->display) < 0) return false;
	} else if (wl_display_dispatch_pending(client->display) < 0) {
		return false;
	}
	return true;
}

static int ipc_test_connect(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 &&
		errno != EINPROGRESS) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool ipc_test_exchange(struct test_client *tc, struct tp_server *s,
							  const char *path, const char *req, char *out,
							  size_t out_size) {
	out[0] = '\0';
	int fd = ipc_test_connect(path);
	if (fd < 0)
		return false;
	size_t rlen = strlen(req);
	size_t woff = 0, roff = 0;
	for (unsigned int i = 0; i < 60; i++) {
		while (woff < rlen) {
			ssize_t n = write(fd, req + woff, rlen - woff);
			if (n > 0)
				woff += (size_t)n;
			else
				break;
		}
		if (!pump(tc, s)) {
			close(fd);
			return false;
		}
		for (;;) {
			if (roff + 1 >= out_size)
				break;
			ssize_t n = read(fd, out + roff, out_size - 1 - roff);
			if (n > 0) {
				roff += (size_t)n;
				out[roff] = '\0';
			} else {
				break;
			}
		}
		if (strncmp(out, "ERR ", 4) == 0 && strchr(out, '\n') != NULL) {
			close(fd);
			return true;
		}
		if (strstr(out, "\nEND\n") != NULL || strcmp(out, "OK\nEND\n") == 0) {
			close(fd);
			return true;
		}
	}
	close(fd);
	return roff > 0;
}

static bool globals_ready(struct test_client *client) {
	return client->compositor && client->shm && client->seat && client->wm_base &&
		client->layer_shell && client->keyboard && client->pointer &&
		client->has_viewporter && client->has_fractional_scale;
}

struct test_layer {
	struct test_client *client;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	bool configured, closed;
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct test_layer *l = data;
	l->width = width;
	l->height = height;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (!l->configured) {
		l->configured = true;
		wl_surface_attach(l->surface, l->client->buffer, 0, 0);
		wl_surface_damage(l->surface, 0, 0, 2, 2);
		wl_surface_commit(l->surface);
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
	((struct test_layer *)data)->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void create_layer(struct test_client *client, struct test_layer *l,
		uint32_t layer, uint32_t anchor, uint32_t w, uint32_t h,
		int32_t excl, uint32_t kb) {
	memset(l, 0, sizeof(*l));
	l->client = client;
	l->surface = wl_compositor_create_surface(client->compositor);
	l->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		client->layer_shell, l->surface, NULL, layer, "test-panel");
	zwlr_layer_surface_v1_add_listener(l->layer_surface, &layer_listener, l);
	zwlr_layer_surface_v1_set_anchor(l->layer_surface, anchor);
	zwlr_layer_surface_v1_set_size(l->layer_surface, w, h);
	zwlr_layer_surface_v1_set_exclusive_zone(l->layer_surface, excl);
	zwlr_layer_surface_v1_set_keyboard_interactivity(l->layer_surface, kb);
	wl_surface_commit(l->surface);
}

static void destroy_layer(struct test_layer *l) {
	if (l->layer_surface)
		zwlr_layer_surface_v1_destroy(l->layer_surface);
	if (l->surface)
		wl_surface_destroy(l->surface);
	memset(l, 0, sizeof(*l));
}

struct test_popup {
	struct test_client *client;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_popup *popup;
	bool configured;
	int32_t x, y, w, h; /* the last xdg_popup.configure */
	uint32_t repositioned; /* token of the last xdg_popup.repositioned */
};

static void popup_xdg_configure(void *data, struct xdg_surface *surface,
		uint32_t serial) {
	struct test_popup *p = data;
	xdg_surface_ack_configure(surface, serial);
	if (!p->configured) {
		p->configured = true;
		wl_surface_attach(p->surface, p->client->buffer, 0, 0);
		wl_surface_damage(p->surface, 0, 0, 2, 2);
		wl_surface_commit(p->surface);
	}
}

static const struct xdg_surface_listener popup_surface_listener = {
	.configure = popup_xdg_configure,
};

static void popup_configure(void *data, struct xdg_popup *popup,
		int32_t x, int32_t y, int32_t w, int32_t h) {
	struct test_popup *p = data;
	p->x = x;
	p->y = y;
	p->w = w;
	p->h = h;
}
static void popup_done(void *data, struct xdg_popup *popup) {}

static void popup_repositioned(void *data, struct xdg_popup *popup,
		uint32_t token) {
	((struct test_popup *)data)->repositioned = token;
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
	.repositioned = popup_repositioned,
};

/* A blank w x h buffer, for a window with a real size (the shared buffer
 * is 2x2, below TP_FLOAT_MIN_W/H). */
static struct wl_buffer *create_sized_buffer_fmt(struct test_client *client,
		int32_t w, int32_t h, uint32_t format) {
	static unsigned int seq = 0;
	char name[64];
	snprintf(name, sizeof(name), "/tam-pane-test-%ld-%u-%dx%d", (long)getpid(),
		++seq, w, h);
	int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0) return NULL;
	shm_unlink(name);
	int32_t size = w * h * 4;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
		format);
	wl_shm_pool_destroy(pool);
	close(fd); /* libwayland sent a duplicate */
	return b;
}

static struct wl_buffer *create_sized_buffer(struct test_client *client,
		int32_t w, int32_t h) {
	return create_sized_buffer_fmt(client, w, h, WL_SHM_FORMAT_ARGB8888);
}

static int create_shm_buffer(struct test_client *client) {
	char name[64];
	snprintf(name, sizeof(name), "/tam-pane-test-%ld", (long)getpid());
	client->shm_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (client->shm_fd < 0) return -1;
	shm_unlink(name);
	if (ftruncate(client->shm_fd, 16) < 0) return -1;
	client->shm_data = mmap(NULL, 16, PROT_READ | PROT_WRITE, MAP_SHARED,
		client->shm_fd, 0);
	if (client->shm_data == MAP_FAILED) return -1;
	memset(client->shm_data, 0xff, 16);
	struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, client->shm_fd, 16);
	client->buffer = wl_shm_pool_create_buffer(pool, 0, 2, 2, 8,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	return client->buffer ? 0 : -1;
}

/* Create a toplevel and make its first commit. With fullscreen set, it
 * asks for fullscreen before that commit, as programs that start
 * fullscreen do. */
static void configure_window_full(struct test_client *client,
		struct test_window *window, const char *app_id, const char *title,
		bool fullscreen) {
	*window = (struct test_window){ .client = client };
	window->surface = wl_compositor_create_surface(client->compositor);
	window->xdg_surface = xdg_wm_base_get_xdg_surface(client->wm_base,
		window->surface);
	xdg_surface_add_listener(window->xdg_surface, &surface_listener, window);
	window->toplevel = xdg_surface_get_toplevel(window->xdg_surface);
	xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
	xdg_toplevel_set_title(window->toplevel, title);
	xdg_toplevel_set_app_id(window->toplevel, app_id);
	if (fullscreen) xdg_toplevel_set_fullscreen(window->toplevel, NULL);
	wl_surface_commit(window->surface);
}

static void configure_window_app(struct test_client *client,
		struct test_window *window, const char *app_id, const char *title) {
	configure_window_full(client, window, app_id, title, false);
}

static void configure_window(struct test_client *client,
		struct test_window *window, const char *title) {
	configure_window_app(client, window, "tam-pane-test", title);
}

static void destroy_window(struct test_window *window) {
	if (window->toplevel) xdg_toplevel_destroy(window->toplevel);
	if (window->xdg_surface) xdg_surface_destroy(window->xdg_surface);
	if (window->surface) wl_surface_destroy(window->surface);
	window->toplevel = NULL;
	window->xdg_surface = NULL;
	window->surface = NULL;
}

static void send_key(struct wlr_keyboard *keyboard, uint32_t code,
		uint32_t state, uint32_t time) {
	struct wlr_keyboard_key_event event = {
		.time_msec = time,
		.keycode = code,
		.update_state = true,
		.state = state,
	};
	wlr_keyboard_notify_key(keyboard, &event);
}

/* Send actual modifier/key events through the compositor keyboard path. */
static void chord_mod(struct wlr_keyboard *kb, unsigned int code, bool shift,
		bool ctrl, bool alt) {
	send_key(kb, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 50);
	if (shift) send_key(kb, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_PRESSED, 51);
	if (ctrl) send_key(kb, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED, 52);
	if (alt) send_key(kb, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 53);
	send_key(kb, code, WL_KEYBOARD_KEY_STATE_PRESSED, 54);
	send_key(kb, code, WL_KEYBOARD_KEY_STATE_RELEASED, 55);
	if (alt) send_key(kb, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 56);
	if (ctrl) send_key(kb, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED, 57);
	if (shift) send_key(kb, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_RELEASED, 58);
	send_key(kb, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 59);
}

static void chord(struct wlr_keyboard *kb, unsigned int code, bool shift,
		bool ctrl) {
	chord_mod(kb, code, shift, ctrl, false);
}

static void emit_motion(struct tp_server *s, struct wlr_pointer *pointer,
		double dx, double dy, uint32_t time) {
	struct wlr_pointer_motion_event motion = {
		.pointer = pointer,
		.time_msec = time,
		.delta_x = dx,
		.delta_y = dy,
		.unaccel_dx = dx,
		.unaccel_dy = dy,
	};
	wl_signal_emit_mutable(&s->cursor->events.motion, &motion);
}

static void emit_button(struct tp_server *s, struct wlr_pointer *pointer,
		uint32_t btn, enum wl_pointer_button_state state, uint32_t time) {
	struct wlr_pointer_button_event button = {
		.pointer = pointer,
		.time_msec = time,
		.button = btn,
		.state = state,
	};
	wl_signal_emit_mutable(&s->cursor->events.button, &button);
}

static char hook_events[32][128];
static int hook_event_count;

static void record_hook_spawn(const char *cmd) {
	if (hook_event_count < 32)
		snprintf(hook_events[hook_event_count++], sizeof(hook_events[0]), "%s",
				 cmd);
}

static bool has_hook_event(const char *prefix) {
	size_t n = strlen(prefix);
	for (int i = 0; i < hook_event_count; i++) {
		if (strncmp(hook_events[i], prefix, n) == 0)
			return true;
	}
	return false;
}

static int find_hook_event(const char *prefix) {
	size_t n = strlen(prefix);
	for (int i = 0; i < hook_event_count; i++) {
		if (strncmp(hook_events[i], prefix, n) == 0)
			return i;
	}
	return -1;
}

static unsigned int last_vt;

static bool mock_change_vt(struct tp_server *s, unsigned int vt) {
	(void)s;
	last_vt = vt;
	return true;
}

static bool key_consumed(struct tp_server *s, uint32_t code) {
	return tp_keyboard_key_consumed(s, code);
}

/* A hook feedback loop used to keep the event loop spinning forever; turn
 * that hang into a test failure. */
static void on_hang_alarm(int sig) {
	(void)sig;
	static const char msg[] = "test_input_headless: hung (event loop never "
							  "returned)\n";
	if (write(2, msg, sizeof(msg) - 1) < 0) {
	}
	_exit(1);
}

struct test_shortcut_inhibit_state {
	unsigned int active_count;
	unsigned int inactive_count;
	bool active;
};

static void test_si_active(void *data,
		struct zwp_keyboard_shortcuts_inhibitor_v1 *inh) {
	(void)inh;
	struct test_shortcut_inhibit_state *st = data;
	st->active = true;
	st->active_count++;
}

static void test_si_inactive(void *data,
		struct zwp_keyboard_shortcuts_inhibitor_v1 *inh) {
	(void)inh;
	struct test_shortcut_inhibit_state *st = data;
	st->active = false;
	st->inactive_count++;
}

static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener test_si_listener = {
	.active = test_si_active,
	.inactive = test_si_inactive,
};

int main(void) {
	struct tp_server server = {0};
	struct wlr_keyboard keyboard;
	struct wlr_pointer pointer;
	struct test_client client = { .shm_fd = -1 };
	tp_settings_defaults(&server.settings);
	if (!tp_server_init(&server) || !tp_server_start(&server)) {
		fprintf(stderr, "headless input: compositor setup failed\n");
		tp_server_finish(&server);
		return 1;
	}
	wlr_keyboard_init(&keyboard, &keyboard_impl, "test keyboard");
	tp_keyboard_add(&server, &keyboard.base);
	wlr_pointer_init(&pointer, &pointer_impl, "test pointer");
	tp_pointer_add(&server, &pointer.base);
	tp_input_update_caps(&server);
	client.display = wl_display_connect(NULL);
	if (!client.display) {
		fprintf(stderr, "headless input: cannot connect test client\n");
		goto fail;
	}
	client.registry = wl_display_get_registry(client.display);
	wl_registry_add_listener(client.registry, &registry_listener, &client);
	for (unsigned int i = 0; i < 1000 && !globals_ready(&client); i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(globals_ready(&client));
	if (!globals_ready(&client) || create_shm_buffer(&client) < 0) goto fail;
	/* presentation-time is offered (B6). Without it GTK 4 times frames
	 * from the output's refresh, and a nested output's refresh is 0:
	 * GTK divides by it and crashes. Before 0.1.14 it was missing. */
	CHECK(client.has_presentation);
	configure_window(&client, &client.windows[0], "input-one");
	for (unsigned int i = 0; i < 1000 && !client.windows[0].configured; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.windows[0].configured);
	if (!client.windows[0].configured) goto fail;
	for (unsigned int i = 0; i < 1000 && wl_list_empty(&server.clients); i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(!wl_list_empty(&server.clients));
	CHECK(client.windows[0].width > 0 && client.windows[0].height > 0);
	struct tp_client *split_target = tp_focused(&server);
	if (!split_target) goto fail;
	tp_arrange_flush(&server);
	struct tp_rect target_box = split_target->geom;
	CHECK(target_box.w > target_box.h);
	CHECK(wlr_cursor_warp(server.cursor, &pointer.base,
		target_box.x + target_box.w / 2.0 - 8.0,
		target_box.y + target_box.h / 2.0));

	/* The test xdg-surface configure handler attaches its first buffer. */
	client.windows[1].surface = wl_compositor_create_surface(client.compositor);
	client.windows[1].client = &client;
	client.windows[1].xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base,
		client.windows[1].surface);
	xdg_surface_add_listener(client.windows[1].xdg_surface,
		&surface_listener, &client.windows[1]);
	client.windows[1].toplevel = xdg_surface_get_toplevel(client.windows[1].xdg_surface);
	xdg_toplevel_add_listener(client.windows[1].toplevel, &toplevel_listener,
		&client.windows[1]);
	xdg_toplevel_set_title(client.windows[1].toplevel, "input-two");
	wl_surface_commit(client.windows[1].surface);
	for (unsigned int i = 0; i < 1000 && !client.windows[1].configured; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.windows[1].configured);
	for (unsigned int i = 0; i < 1000 && wl_list_length(&server.clients) < 2; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);
	CHECK(client.windows[1].width > 0 && client.windows[1].height > 0);

	/* The two tiles now stand tall. Split the new right-hand tile from
	 * just above its midpoint, where the next window should land below. */
	tp_arrange_flush(&server);
	struct tp_client *right_tile = tp_focused(&server);
	if (!right_tile || right_tile == split_target) goto fail;
	CHECK(right_tile->geom.x > split_target->geom.x);
	target_box = right_tile->geom;
	CHECK(target_box.h > target_box.w);
	CHECK(wlr_cursor_warp(server.cursor, &pointer.base,
		target_box.x + target_box.w / 2.0,
		target_box.y + target_box.h / 2.0 - 8.0));
	configure_window(&client, &client.windows[2], "split-below-center");
	for (unsigned int i = 0; i < 1000 && wl_list_length(&server.clients) < 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 3);
	tp_arrange_flush(&server);
	struct tp_client *bottom_tile = tp_focused(&server);
	if (!bottom_tile || bottom_tile == right_tile) goto fail;
	CHECK(bottom_tile->geom.x == right_tile->geom.x &&
		bottom_tile->geom.y > right_tile->geom.y);
	destroy_window(&client.windows[2]);
	for (unsigned int i = 0; i < 100 && wl_list_length(&server.clients) == 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);
	tp_focus(&server, right_tile);

	/* Meta+j cycles focus. The binding's J press and release must both stay
	 * inside the compositor, while the ordinary X pair reaches the client. */
	send_key(&keyboard, 125, WL_KEYBOARD_KEY_STATE_PRESSED, 10);
	send_key(&keyboard, 36, WL_KEYBOARD_KEY_STATE_PRESSED, 11);
	send_key(&keyboard, 36, WL_KEYBOARD_KEY_STATE_RELEASED, 12);
	send_key(&keyboard, 125, WL_KEYBOARD_KEY_STATE_RELEASED, 13);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.keyboard_focus == client.windows[0].surface);
	CHECK(client.key_events == 2);
	CHECK(client.keys[0] == 125 &&
		client.key_states[0] == WL_KEYBOARD_KEY_STATE_PRESSED);
	CHECK(client.keys[1] == 125 &&
		client.key_states[1] == WL_KEYBOARD_KEY_STATE_RELEASED);
	send_key(&keyboard, 45, WL_KEYBOARD_KEY_STATE_PRESSED, 20);
	send_key(&keyboard, 45, WL_KEYBOARD_KEY_STATE_RELEASED, 21);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events == 4);
	CHECK(client.keys[2] == 45 &&
		client.key_states[2] == WL_KEYBOARD_KEY_STATE_PRESSED);
	CHECK(client.keys[3] == 45 &&
		client.key_states[3] == WL_KEYBOARD_KEY_STATE_RELEASED);

	/* CapsLock pairs disappear from client input, including across focus. */
	client.key_events = 0;
	client.seen_locked = 0;
	send_key(&keyboard, KEY_CAPSLOCK, WL_KEYBOARD_KEY_STATE_PRESSED, 30);
	tp_focus(&server, right_tile);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.keyboard_focus == client.windows[1].surface);
	CHECK(!client.caps_on_enter && !(client.seen_locked & WLR_MODIFIER_CAPS));
	send_key(&keyboard, KEY_CAPSLOCK, WL_KEYBOARD_KEY_STATE_RELEASED, 31);
	send_key(&keyboard, KEY_CAPSLOCK, WL_KEYBOARD_KEY_STATE_PRESSED, 32);
	send_key(&keyboard, KEY_CAPSLOCK, WL_KEYBOARD_KEY_STATE_RELEASED, 33);
	send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED, 34);
	send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED, 35);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events == 2 && client.keys[0] == KEY_A);
	CHECK(client.key_mods[0] == 0 && !(client.seen_locked & WLR_MODIFIER_CAPS));

	client.key_events = 0;
	send_key(&keyboard, KEY_CAPSLOCK, WL_KEYBOARD_KEY_STATE_PRESSED, 36);
	send_key(&keyboard, KEY_CAPSLOCK, WL_KEYBOARD_KEY_STATE_RELEASED, 37);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 38);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 39);
	send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED, 40);
	send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED, 41);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events == 0);
	send_key(&keyboard, KEY_CAPSLOCK, WL_KEYBOARD_KEY_STATE_PRESSED, 42);
	send_key(&keyboard, KEY_CAPSLOCK, WL_KEYBOARD_KEY_STATE_RELEASED, 43);
	send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED, 44);
	send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED, 45);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events == 6);
	CHECK(client.keys[0] == KEY_LEFTMETA && client.keys[3] == KEY_LEFTMETA);
	CHECK(client.keys[1] == KEY_A && client.keys[2] == KEY_A);
	CHECK(client.key_mods[1] == WLR_MODIFIER_LOGO);
	CHECK(client.keys[4] == KEY_A && client.key_mods[4] == 0);
	CHECK(!(client.seen_locked & WLR_MODIFIER_CAPS));
	tp_focus(&server, split_target);

	/* Dispatch a terminal paste while the physical macro modifiers are
	 * still held. The synthetic key and real releases must stay distinct. */
	client.key_events = 0;
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 22);
	send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 23);
	CHECK(tp_keyboard_send_key(&server, TP_MOD_CTRL | TP_MOD_SHIFT, XKB_KEY_v));
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events == 4);
	CHECK(client.keys[2] == KEY_V &&
		client.key_states[2] == WL_KEYBOARD_KEY_STATE_PRESSED);
	CHECK(client.keys[3] == KEY_V &&
		client.key_states[3] == WL_KEYBOARD_KEY_STATE_RELEASED);
	CHECK(client.key_mods[2] == (WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT));
	CHECK(client.key_mods[3] == client.key_mods[2]);
	CHECK(client.depressed == (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT));
	send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 24);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 25);
	send_key(&keyboard, KEY_X, WL_KEYBOARD_KEY_STATE_PRESSED, 26);
	send_key(&keyboard, KEY_X, WL_KEYBOARD_KEY_STATE_RELEASED, 27);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events == 8 && client.depressed == 0);
	CHECK(client.keys[6] == KEY_X && client.key_mods[6] == 0);

	/* Macro-style release bindings keep the whole physical key pair and
	 * send exactly one paste pair, regardless of modifier release order. */
	const char macro_binds[] =
		"tp.bind('Meta+Alt+slash', tp.dsp.send_key('Ctrl+Shift+v'), { release = true })\n"
		"tp.bind('F9', function() tp.spawn('dictation-start') end)\n"
		"tp.bind('F9', function() tp.spawn('dictation-stop') end, { release = true })";
	CHECK(tp_lua_exec_string(server.lua, macro_binds, sizeof(macro_binds) - 1,
							 "macro-binds", 100000, 100, &server.settings,
							 &server.settings, NULL, 0));
	for (int modifiers_first = 0; modifiers_first < 2; modifiers_first++) {
		client.key_events = 0;
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 60);
		send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 61);
		send_key(&keyboard, KEY_SLASH, WL_KEYBOARD_KEY_STATE_PRESSED, 62);
		CHECK(key_consumed(&server, KEY_SLASH));
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(client.key_events == 2);
		if (modifiers_first) {
			tp_focus(&server, right_tile);
			for (unsigned int i = 0; i < 10; i++)
				if (!pump(&client, &server)) goto fail;
			CHECK(client.keyboard_focus == client.windows[1].surface);
			CHECK(!client.slash_on_enter);
		} else {
			send_key(&keyboard, KEY_SLASH, WL_KEYBOARD_KEY_STATE_RELEASED, 63);
		}
		send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 64);
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 65);
		if (modifiers_first)
			send_key(&keyboard, KEY_SLASH, WL_KEYBOARD_KEY_STATE_RELEASED, 66);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		int paste = modifiers_first ? 4 : 2;
		CHECK(client.key_events == 6 && client.depressed == 0);
		CHECK(client.keys[paste] == KEY_V &&
			  client.key_states[paste] == WL_KEYBOARD_KEY_STATE_PRESSED);
		CHECK(client.keys[paste + 1] == KEY_V &&
			  client.key_states[paste + 1] == WL_KEYBOARD_KEY_STATE_RELEASED);
		CHECK(client.key_mods[paste] == (WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT));
		CHECK(!key_consumed(&server, KEY_SLASH));
	}
	tp_focus(&server, split_target);

	/* Terminal-aware Meta+v using tp.focused_window(): sends Ctrl+Shift+v
	 * when focused on tam-last and Ctrl+v when focused on chromium. */
	const char clip_binds[] =
		"tp.bind('Meta+v', function()\n"
		"  local w = tp.focused_window()\n"
		"  if w and w.app_id == 'tam-last' then\n"
		"    tp.dsp.send_key('Ctrl+Shift+v')()\n"
		"  else\n"
		"    tp.dsp.send_key('Ctrl+v')()\n"
		"  end\n"
		"end)\n";
	CHECK(tp_lua_exec_string(server.lua, clip_binds, sizeof(clip_binds) - 1,
							 "clip-binds", 100000, 100, &server.settings,
							 &server.settings, NULL, 0));
	xdg_toplevel_set_app_id(client.windows[0].toplevel, "tam-last");
	xdg_toplevel_set_app_id(client.windows[1].toplevel, "chromium");
	wl_display_flush(client.display);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;

	/* Focus tam-last -> Meta+v synthesizes Ctrl+Shift+v. */
	tp_focus(&server, split_target);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	client.key_events = 0;
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 100);
	send_key(&keyboard, KEY_V, WL_KEYBOARD_KEY_STATE_PRESSED, 101);
	send_key(&keyboard, KEY_V, WL_KEYBOARD_KEY_STATE_RELEASED, 102);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 103);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events >= 3);
	CHECK(client.keys[1] == KEY_V &&
		  client.key_states[1] == WL_KEYBOARD_KEY_STATE_PRESSED &&
		  client.key_mods[1] == (WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT));

	/* Focus chromium -> Meta+v synthesizes Ctrl+v. */
	tp_focus(&server, right_tile);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	client.key_events = 0;
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 104);
	send_key(&keyboard, KEY_V, WL_KEYBOARD_KEY_STATE_PRESSED, 105);
	send_key(&keyboard, KEY_V, WL_KEYBOARD_KEY_STATE_RELEASED, 106);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 107);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events >= 3);
	CHECK(client.keys[1] == KEY_V &&
		  client.key_states[1] == WL_KEYBOARD_KEY_STATE_PRESSED &&
		  client.key_mods[1] == WLR_MODIFIER_CTRL);

	xdg_toplevel_set_app_id(client.windows[0].toplevel, "tam-pane-test");
	xdg_toplevel_set_app_id(client.windows[1].toplevel, "tam-pane-test");
	wl_display_flush(client.display);
	const char restore_meta_v[] = "tp.bind('Meta+v', tp.dsp.toggle_float())\n";
	CHECK(tp_lua_exec_string(server.lua, restore_meta_v,
							 sizeof(restore_meta_v) - 1, "restore-meta-v",
							 100000, 100, &server.settings, &server.settings,
							 NULL, 0));
	tp_focus(&server, split_target);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;

	/* Both plain slash and slash pressed before the modifiers pass through. */
	for (int late_modifiers = 0; late_modifiers < 2; late_modifiers++) {
		client.key_events = 0;
		send_key(&keyboard, KEY_SLASH, WL_KEYBOARD_KEY_STATE_PRESSED, 67);
		if (late_modifiers) {
			send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 68);
			send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 69);
		}
		send_key(&keyboard, KEY_SLASH, WL_KEYBOARD_KEY_STATE_RELEASED, 70);
		if (late_modifiers) {
			send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 71);
			send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 72);
		}
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		int release = late_modifiers ? 3 : 1;
		CHECK(client.key_events == (late_modifiers ? 6u : 2u));
		CHECK(client.keys[0] == KEY_SLASH &&
			  client.key_states[0] == WL_KEYBOARD_KEY_STATE_PRESSED);
		CHECK(client.keys[release] == KEY_SLASH &&
			  client.key_states[release] == WL_KEYBOARD_KEY_STATE_RELEASED);
	}

	tp_lua_set_spawn_fn(server.lua, record_hook_spawn);
	hook_event_count = 0;
	client.key_events = 0;
	send_key(&keyboard, KEY_F9, WL_KEYBOARD_KEY_STATE_PRESSED, 73);
	CHECK(hook_event_count == 1 && has_hook_event("dictation-start"));
	send_key(&keyboard, KEY_F9, WL_KEYBOARD_KEY_STATE_RELEASED, 74);
	CHECK(hook_event_count == 2 && has_hook_event("dictation-stop"));
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.key_events == 0);

	/* The pointer starts over the headless output. Motion and a button must
	 * be delivered to the surface underneath it. */
	struct tp_client *first = wl_container_of(server.clients.next, first, link);
	double target_x = first->geom.x + 0.25, target_y = first->geom.y + 0.25;
	wlr_cursor_warp(server.cursor, &pointer.base, target_x, target_y);
	struct wlr_pointer_motion_event motion = {
		.pointer = &pointer,
		.time_msec = 30,
		.delta_x = 0,
		.delta_y = 0,
		.unaccel_dx = 0,
		.unaccel_dy = 0,
	};
	wl_signal_emit_mutable(&server.cursor->events.motion, &motion);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	motion.time_msec++;
	motion.delta_x = motion.delta_y = 0.25;
	motion.unaccel_dx = motion.unaccel_dy = 0.25;
	wl_signal_emit_mutable(&server.cursor->events.motion, &motion);
	struct wlr_pointer_button_event button = {
		.pointer = &pointer,
		.time_msec = 31,
		.button = BTN_LEFT,
		.state = WL_POINTER_BUTTON_STATE_PRESSED,
	};
	wl_signal_emit_mutable(&server.cursor->events.button, &button);
	button.time_msec++;
	button.state = WL_POINTER_BUTTON_STATE_RELEASED;
	wl_signal_emit_mutable(&server.cursor->events.button, &button);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.pointer_enters > 0);
	CHECK(client.pointer_motions > 0);
	CHECK(client.pointer_buttons == 2);

	/* Exercise all tag bindings, including shifted digit symbol handling. */
	struct tp_client *a = tp_focused(&server);
	struct tp_client *b;
	wl_list_for_each(b, &server.clients, link) if (b != a) break;
	struct tp_monitor *m = a->mon;
	for (unsigned int n = 1; n <= TP_TAG_COUNT; n++) {
		chord(&keyboard, KEY_1 + n - 1, true, false);
		CHECK(a->tags == tp_tag_mask(n));
		chord(&keyboard, KEY_1 + n - 1, false, false);
		CHECK(tp_tagview_current(&m->view) == tp_tag_mask(n));
		tp_focus(&server, a);
		chord(&keyboard, KEY_1 + n - 1, true, true);
		CHECK(a->tags == tp_tag_mask(n)); /* cannot remove last tag */
		unsigned int other = n == TP_TAG_COUNT ? 1 : n + 1;
		chord(&keyboard, KEY_1 + other - 1, true, true);
		CHECK(a->tags == (tp_tag_mask(n) | tp_tag_mask(other)));
		chord(&keyboard, KEY_1 + other - 1, false, true);
		CHECK(tp_tags_count(tp_tagview_current(&m->view)) == 2);
		chord(&keyboard, KEY_1 + other - 1, false, true);
		CHECK(tp_tagview_current(&m->view) == tp_tag_mask(n));
		chord(&keyboard, KEY_1 + n - 1, false, true);
		CHECK(tp_tagview_current(&m->view) == tp_tag_mask(n));
		chord(&keyboard, KEY_1 + other - 1, true, true);
		CHECK(a->tags == tp_tag_mask(n));
	}
	/* The same shared window must not overwrite another tag's memory. */
	tp_client_set_tags(a, tp_tag_mask(1) | tp_tag_mask(2));
	tp_client_set_tags(b, tp_tag_mask(1) | tp_tag_mask(2));
	chord(&keyboard, KEY_1, false, false);
	tp_focus(&server, a);
	chord(&keyboard, KEY_2, false, false);
	tp_focus(&server, b);
	chord(&keyboard, KEY_1, false, false);
	CHECK(tp_focused(&server) == a);
	chord(&keyboard, KEY_TAB, false, false);
	CHECK(tp_focused(&server) == b);
	chord(&keyboard, KEY_1, false, true);
	tp_focus(&server, a); /* combined slot */
	chord(&keyboard, KEY_2, false, false);
	tp_focus(&server, b);
	chord(&keyboard, KEY_1, false, true);
	CHECK(tp_focused(&server) == a);
	tp_client_set_tags(a, tp_tag_mask(9));
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(tp_focused(&server) == b);
	CHECK(m->focus[1] != a && m->focus[2] != a && m->focus[0] != a);
	CHECK(!a->tree->node.enabled);
	chord(&keyboard, KEY_8, false, false);
	CHECK(tp_focused(&server) == NULL);
	chord(&keyboard, KEY_1, false, false);
	CHECK(tp_focused(&server) == b);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.keyboard_focus == (b == first ? client.windows[1].surface
		: client.windows[0].surface));

	/* The close binding must send xdg_toplevel.close to the focused client. */
	send_key(&keyboard, 125, WL_KEYBOARD_KEY_STATE_PRESSED, 40);
	send_key(&keyboard, 17, WL_KEYBOARD_KEY_STATE_PRESSED, 41);
	send_key(&keyboard, 17, WL_KEYBOARD_KEY_STATE_RELEASED, 42);
	send_key(&keyboard, 125, WL_KEYBOARD_KEY_STATE_RELEASED, 43);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	struct test_window *focused = client.keyboard_focus == client.windows[0].surface
		? &client.windows[0]
		: client.keyboard_focus == client.windows[1].surface
			? &client.windows[1]
			: NULL;
	CHECK(focused && focused->closed);
	if (focused) destroy_window(focused);
	for (unsigned int i = 0; i < 100 && wl_list_length(&server.clients) == 2; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 1);
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++)
		CHECK(m->focus[i] != b);
	CHECK(tp_focused(&server) == NULL);
	chord(&keyboard, KEY_9, false, false);
	CHECK(tp_focused(&server) == a);

	/* 0.0.4: tiling resize, directional swap/move, floating move/resize, and
	 * grab cleanup on unmap. Put 3 windows on tag 1 as [c0 | [c1 / c2]]. */
	chord(&keyboard, KEY_1, true, false);
	chord(&keyboard, KEY_1, false, false);
	wlr_cursor_warp(server.cursor, &pointer.base, 1270.0, 710.0);
	struct test_window *slot1 = client.windows[0].surface
		? &client.windows[1] : &client.windows[0];
	configure_window(&client, slot1, "tile-two");
	for (unsigned int i = 0; i < 1000 && wl_list_length(&server.clients) < 2; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);
	struct tp_client *c1 = tp_focused(&server);
	configure_window(&client, &client.windows[2], "tile-three");
	for (unsigned int i = 0; i < 1000 && wl_list_length(&server.clients) < 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 3);
	struct tp_client *c2 = tp_focused(&server);
	struct tp_client *c0 = a;
	CHECK(c0 != c1 && c1 != c2 && c0 != c2);
	CHECK(c0->geom.x < c1->geom.x && c1->geom.y < c2->geom.y);

	/* Keyboard split resize: Meta+Alt+Right grows c0; Meta+Alt+Down grows c1. */
	int32_t w0_before = c0->geom.w;
	tp_focus(&server, c0);
	chord_mod(&keyboard, KEY_RIGHT, false, false, true);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c0->geom.w > w0_before);
	chord_mod(&keyboard, KEY_LEFT, false, false, true);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c0->geom.w == w0_before);

	int32_t h1_before = c1->geom.h;
	tp_focus(&server, c1);
	chord_mod(&keyboard, KEY_DOWN, false, false, true);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c1->geom.h > h1_before);
	chord_mod(&keyboard, KEY_UP, false, false, true);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c1->geom.h == h1_before);

	/* Directional swap: Meta+Shift+Down swaps c1 and c2. */
	struct tp_rect g1 = c1->geom, g2 = c2->geom;
	tp_focus(&server, c1);
	chord_mod(&keyboard, KEY_DOWN, true, false, false);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c1->geom.y == g2.y && c2->geom.y == g1.y);
	CHECK(tp_focused(&server) == c1);
	chord_mod(&keyboard, KEY_UP, true, false, false);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c1->geom.y == g1.y && c2->geom.y == g2.y);

	/* Directional move: Meta+Ctrl+Right moves c0 into c1's slot -> [[c0|c1]/c2]. */
	tp_focus(&server, c0);
	chord_mod(&keyboard, KEY_RIGHT, false, true, false);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c0->geom.y == c1->geom.y && c0->geom.x < c1->geom.x);
	CHECK(c2->geom.y > c0->geom.y && c2->geom.w > c0->geom.w);

	/* Tiled pointer resize: Meta+Right-drag on c0 widens c0. */
	w0_before = c0->geom.w;
	wlr_cursor_warp(server.cursor, &pointer.base,
		c0->geom.x + 1.0, c0->geom.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 100);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 101);
	emit_button(&server, &pointer, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED, 102);
	CHECK(server.grab.mode == TP_GRAB_RESIZE && server.grab.tiled);
	emit_motion(&server, &pointer, 80.0, 0.0, 103);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c0->geom.w > w0_before);
	emit_button(&server, &pointer, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED, 104);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 105);
	CHECK(server.grab.mode == TP_GRAB_NONE);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;

	/* W52 tiled drop: Meta+Left-drag c0 and release it on itself, which
	 * changes nothing, then over the right part of c2, which splits c2's
	 * tile and leaves c1 alone across the top. */
	struct tp_rect d0 = c0->geom, d1 = c1->geom, d2 = c2->geom;
	wlr_cursor_warp(server.cursor, &pointer.base, d0.x + 1.0, d0.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 106);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 107);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 108);
	CHECK(server.grab.mode == TP_GRAB_MOVE && server.grab.tiled &&
		server.grab.client == c0);
	emit_motion(&server, &pointer, 5.0, 5.0, 109);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 110);
	CHECK(server.grab.mode == TP_GRAB_NONE);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c0->geom.x == d0.x && c0->geom.w == d0.w && c1->geom.x == d1.x &&
		c2->geom.y == d2.y && c2->geom.w == d2.w);

	wlr_cursor_warp(server.cursor, &pointer.base, d0.x + 1.0, d0.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 111);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 111);
	CHECK(server.grab.mode == TP_GRAB_MOVE && server.grab.client == c0);
	wlr_cursor_warp(server.cursor, &pointer.base,
		d2.x + d2.w * 0.75, d2.y + d2.h / 2.0);
	emit_motion(&server, &pointer, 0, 0, 112);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 113);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 114);
	CHECK(server.grab.mode == TP_GRAB_NONE);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c1->geom.y == d1.y && c1->geom.x == d0.x);
	CHECK(c1->geom.w > d1.w);
	CHECK(c0->geom.y == d2.y && c2->geom.y == d2.y);
	CHECK(c2->geom.x == d2.x && c0->geom.x > c2->geom.x);
	CHECK(c0->geom.w < d2.w && c2->geom.w < d2.w);
	CHECK(tp_focused(&server) == c0);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;

	/* Drop c0 back on c1's left part: [[c0|c1]/c2] again, evenly split. */
	struct tp_rect e1 = c1->geom;
	wlr_cursor_warp(server.cursor, &pointer.base,
		c0->geom.x + 1.0, c0->geom.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 115);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 116);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 117);
	wlr_cursor_warp(server.cursor, &pointer.base,
		e1.x + e1.w * 0.25, e1.y + e1.h / 2.0);
	emit_motion(&server, &pointer, 0, 0, 118);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 119);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 120);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c0->geom.y == d0.y && c1->geom.y == d0.y && c0->geom.x == d0.x);
	CHECK(c1->geom.x > c0->geom.x && c2->geom.y == d2.y && c2->geom.w == d2.w);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;

	/* Floating toggle, keyboard resize, pointer move, and pointer resize. */
	tp_focus(&server, c2);
	chord(&keyboard, KEY_V, false, false);
	CHECK(c2->floating);
	CHECK(c2->float_geom.w >= TP_FLOAT_MIN_W && c2->float_geom.h >= TP_FLOAT_MIN_H);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;

	/* Keyboard resize on floating window. */
	int32_t fw = c2->float_geom.w, fh = c2->float_geom.h;
	chord_mod(&keyboard, KEY_RIGHT, false, false, true);
	chord_mod(&keyboard, KEY_DOWN, false, false, true);
	CHECK(c2->float_geom.w == fw + server.settings.resize_step);
	CHECK(c2->float_geom.h == fh + server.settings.resize_step);

	/* Pointer move on floating window (Meta + BTN_LEFT). */
	unsigned int buttons_before = client.pointer_buttons;
	int32_t fx = c2->float_geom.x, fy = c2->float_geom.y;
	wlr_cursor_warp(server.cursor, &pointer.base, fx + 1.0, fy + 1.0);
	emit_motion(&server, &pointer, 0, 0, 110);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 111);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 112);
	CHECK(server.grab.mode == TP_GRAB_MOVE && server.grab.client == c2);
	emit_motion(&server, &pointer, 50.0, 30.0, 113);
	CHECK(c2->float_geom.x == fx + 50 && c2->float_geom.y == fy + 30);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 114);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 115);
	CHECK(server.grab.mode == TP_GRAB_NONE);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.pointer_buttons == buttons_before); /* grab swallowed buttons */

	/* Pointer resize on floating window (Meta + BTN_RIGHT from top-left
	 * quadrant clamps at TP_FLOAT_MIN_W/H while anchoring bottom-right). */
	int32_t right_edge = c2->float_geom.x + c2->float_geom.w;
	int32_t bottom_edge = c2->float_geom.y + c2->float_geom.h;
	wlr_cursor_warp(server.cursor, &pointer.base,
		c2->float_geom.x + 1.0, c2->float_geom.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 120);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 121);
	emit_button(&server, &pointer, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED, 122);
	CHECK(server.grab.mode == TP_GRAB_RESIZE && !server.grab.tiled);
	emit_motion(&server, &pointer, 2000.0, 2000.0, 123);
	CHECK(c2->float_geom.w == TP_FLOAT_MIN_W && c2->float_geom.h == TP_FLOAT_MIN_H);
	CHECK(c2->float_geom.x + c2->float_geom.w == right_edge);
	CHECK(c2->float_geom.y + c2->float_geom.h == bottom_edge);

	/* Unmapping a grabbed window mid-drag cancels the grab cleanly. */
	destroy_window(&client.windows[2]);
	for (unsigned int i = 0; i < 100 && wl_list_length(&server.clients) == 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);
	CHECK(server.grab.mode == TP_GRAB_NONE);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 124);

	/* A tiled drag survives another window leaving the tree. Open a window,
	 * Meta+Right-drag the window it split, then close the new window
	 * mid-drag: the split the drag was holding is freed, so the next motion
	 * must not touch it, and the drag carries on against the tree that is
	 * left. */
	configure_window(&client, &client.windows[2], "tile-extra");
	for (unsigned int i = 0; i < 1000 && wl_list_length(&server.clients) < 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 3);
	struct tp_client *cx = tp_focused(&server);
	CHECK(cx && cx != c0 && cx != c1 && !cx->floating && cx->mon);
	if (!cx || !cx->mon) goto fail;
	struct tp_dwindle_node *xn =
		tp_dwindle_find(&tp_monitor_layout(cx->mon)->dwindle, cx);
	CHECK(xn && xn->parent);
	if (!xn || !xn->parent) goto fail;
	struct tp_dwindle_node *sib = xn->parent->child[xn->parent->child[0] == xn];
	CHECK(sib->item == c0 || sib->item == c1);
	struct tp_client *cs = sib->item;
	tp_focus(&server, cs);
	wlr_cursor_warp(server.cursor, &pointer.base,
		cs->geom.x + 1.0, cs->geom.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 140);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 141);
	emit_button(&server, &pointer, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED, 142);
	CHECK(server.grab.mode == TP_GRAB_RESIZE && server.grab.tiled &&
		server.grab.client == cs);
	CHECK(server.grab.drag.u.dwindle.h_node == xn->parent ||
		server.grab.drag.u.dwindle.v_node == xn->parent);
	destroy_window(&client.windows[2]);
	for (unsigned int i = 0; i < 100 && wl_list_length(&server.clients) == 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);
	w0_before = c0->geom.w;
	emit_motion(&server, &pointer, 60.0, 0.0, 143);
	emit_motion(&server, &pointer, 60.0, 0.0, 144);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(server.grab.mode == TP_GRAB_RESIZE && server.grab.tiled);
	CHECK(c0->geom.w != w0_before);
	emit_button(&server, &pointer, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED, 145);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 146);
	CHECK(server.grab.mode == TP_GRAB_NONE);

	/* A new window splits the focused window, not the last leaf (R1
	 * finding 12). In [c0 | c1], focus c0 and open a window: its sibling
	 * must be c0. Before 0.1.11 it split c1. */
	tp_focus(&server, c0);
	CHECK(tp_focused(&server) == c0);
	configure_window(&client, &client.windows[2], "split-focused");
	for (unsigned int i = 0; i < 1000 && wl_list_length(&server.clients) < 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 3);
	struct tp_client *cn = tp_focused(&server);
	CHECK(cn && cn != c0 && cn != c1 && !cn->floating && cn->mon);
	if (!cn || !cn->mon) goto fail;
	struct tp_dwindle_node *nn =
		tp_dwindle_find(&tp_monitor_layout(cn->mon)->dwindle, cn);
	CHECK(nn && nn->parent);
	if (!nn || !nn->parent) goto fail;
	CHECK(nn->parent->child[nn->parent->child[0] == nn]->item == c0);
	destroy_window(&client.windows[2]);
	for (unsigned int i = 0; i < 100 && wl_list_length(&server.clients) == 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);

	/* Fullscreen across map and remap (R1 finding 4). A window that asks
	 * for fullscreen before its first commit maps fullscreen. Unmapped
	 * (null buffer) and mapped again without the request, it is tiled,
	 * not left marked fullscreen. Before 0.1.12 it mapped tiled, and the
	 * remapped window kept the stale fullscreen flag. */
	configure_window_full(&client, &client.windows[2], "tam-pane-test",
		"starts-fullscreen", true);
	for (unsigned int i = 0; i < 1000 && wl_list_length(&server.clients) < 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 3);
	struct tp_client *cf = tp_focused(&server);
	CHECK(cf && cf != c0 && cf != c1 && cf->mon);
	if (!cf || !cf->mon) goto fail;
	CHECK(cf->fullscreen);
	CHECK(cf->tree->node.parent == server.layer[TP_LAYER_FULLSCREEN]);
	CHECK(cf->geom.x == cf->mon->area.x && cf->geom.y == cf->mon->area.y &&
		cf->geom.w == cf->mon->area.w && cf->geom.h == cf->mon->area.h);
	CHECK(!tp_dwindle_find(&tp_monitor_layout(cf->mon)->dwindle, cf));

	/* Ask again while mapped (a no-op once the first half works), so the
	 * remap below starts from a fullscreen window either way. */
	xdg_toplevel_set_fullscreen(client.windows[2].toplevel, NULL);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(cf->fullscreen);

	wl_surface_attach(client.windows[2].surface, NULL, 0, 0);
	wl_surface_commit(client.windows[2].surface);
	for (unsigned int i = 0; i < 100 && wl_list_length(&server.clients) == 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);
	CHECK(!cf->fullscreen);
	client.windows[2].configured = false;
	xdg_toplevel_unset_fullscreen(client.windows[2].toplevel);
	wl_surface_commit(client.windows[2].surface);
	for (unsigned int i = 0; i < 1000 && wl_list_length(&server.clients) < 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 3);
	CHECK(cf->mapped && !cf->fullscreen && !cf->floating);
	CHECK(cf->tree->node.parent == server.layer[TP_LAYER_TILE]);
	CHECK(tp_dwindle_find(&tp_monitor_layout(cf->mon)->dwindle, cf) != NULL);
	destroy_window(&client.windows[2]);
	for (unsigned int i = 0; i < 100 && wl_list_length(&server.clients) == 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);

	/* A menu near the screen edge stays on the monitor (R1 finding 5). On
	 * the right-hand window c1, open a popup that the positioner would put
	 * past the monitor's right edge, with slide and flip allowed as GTK
	 * and Qt do. Before 0.1.13 the compositor never unconstrained it. */
	struct test_window *w1 = NULL;
	for (unsigned int i = 0; i < 3; i++) {
		struct test_window *w = &client.windows[i];
		if (w->surface && wl_proxy_get_id((struct wl_proxy *)w->surface) ==
				wl_resource_get_id(c1->toplevel->base->surface->resource))
			w1 = w;
	}
	CHECK(w1 != NULL && c1->mon && c1->geom.x > c0->geom.x);
	if (!w1 || !c1->mon) goto fail;
	struct tp_rect u1 = c1->mon->usable;
	/* The popup's surface-local origin in layout coordinates. */
	int32_t ox = c1->geom.x - c1->toplevel->base->geometry.x;
	int32_t oy = c1->geom.y - c1->toplevel->base->geometry.y;
	int32_t pw = u1.x + u1.w - ox + 100;
	CHECK(pw < u1.w);
	struct test_popup edge = { .client = &client };
	struct xdg_positioner *epos = xdg_wm_base_create_positioner(client.wm_base);
	xdg_positioner_set_size(epos, pw, 60);
	xdg_positioner_set_anchor_rect(epos, 0, 0, 1, 1);
	xdg_positioner_set_anchor(epos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
	xdg_positioner_set_gravity(epos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(epos,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
	edge.surface = wl_compositor_create_surface(client.compositor);
	edge.xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base, edge.surface);
	xdg_surface_add_listener(edge.xdg_surface, &popup_surface_listener, &edge);
	edge.popup = xdg_surface_get_popup(edge.xdg_surface, w1->xdg_surface, epos);
	xdg_popup_add_listener(edge.popup, &popup_listener, &edge);
	xdg_positioner_destroy(epos);
	wl_surface_commit(edge.surface);
	for (unsigned int i = 0; i < 200 && !edge.configured; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(edge.configured);
	CHECK(edge.w == pw && edge.h == 60);
	CHECK(ox + edge.x >= u1.x && ox + edge.x + edge.w <= u1.x + u1.w);
	CHECK(oy + edge.y >= u1.y && oy + edge.y + edge.h <= u1.y + u1.h);

	/* A popup moved after it is mapped stays on the monitor too (B7).
	 * GTK 4 repositions popovers once it knows their size. Shrink the
	 * popup to one that fits, then reposition it to the too-wide one.
	 * Before 0.1.15 the reposition was configured unconstrained. */
	epos = xdg_wm_base_create_positioner(client.wm_base);
	xdg_positioner_set_size(epos, 40, 60);
	xdg_positioner_set_anchor_rect(epos, 0, 0, 1, 1);
	xdg_positioner_set_anchor(epos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
	xdg_positioner_set_gravity(epos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_popup_reposition(edge.popup, epos, 1);
	xdg_positioner_destroy(epos);
	for (unsigned int i = 0; i < 200 && edge.repositioned != 1; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(edge.repositioned == 1 && edge.w == 40);
	epos = xdg_wm_base_create_positioner(client.wm_base);
	xdg_positioner_set_size(epos, pw, 60);
	xdg_positioner_set_anchor_rect(epos, 0, 0, 1, 1);
	xdg_positioner_set_anchor(epos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
	xdg_positioner_set_gravity(epos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(epos,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
	xdg_popup_reposition(edge.popup, epos, 2);
	xdg_positioner_destroy(epos);
	for (unsigned int i = 0; i < 200 && edge.repositioned != 2; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(edge.repositioned == 2 && edge.w == pw);
	CHECK(ox + edge.x >= u1.x && ox + edge.x + edge.w <= u1.x + u1.w);
	xdg_popup_destroy(edge.popup);
	xdg_surface_destroy(edge.xdg_surface);
	wl_surface_destroy(edge.surface);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;

	/* ---- 0.0.5: layer-shell exclusive zone, popups, keyboard focus ---- */
	struct tp_monitor *m1 = server.selmon;
	CHECK(m1 != NULL);
	struct tp_rect area1 = m1->area;

	struct test_layer bar;
	create_layer(&client, &bar, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
		0, 32, 32, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	for (unsigned int i = 0; i < 200 && !bar.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(bar.configured);
	CHECK(bar.width == (uint32_t)area1.w && bar.height == 32);
	CHECK(m1->usable.y == area1.y + 32);
	CHECK(m1->usable.h == area1.h - 32);
	CHECK(c0->geom.y >= m1->usable.y);
	CHECK(c1->geom.y >= m1->usable.y);

	/* Clicking a NONE-interactivity bar delivers pointer button without
	 * stealing keyboard focus from the active toplevel. */
	tp_focus(&server, c0);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	buttons_before = client.pointer_buttons;
	wlr_cursor_warp(server.cursor, &pointer.base, area1.x + 1.0, area1.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 130);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 131);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 132);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.pointer_buttons > buttons_before);
	CHECK(tp_focused(&server) == c0);
	CHECK(server.focused_layer == NULL);

	/* Layer popup attaches to TP_LAYER_OVERLAY, configures, and receives clicks. */
	struct test_popup pop = { .client = &client };
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(client.wm_base);
	xdg_positioner_set_size(pos, 100, 80);
	xdg_positioner_set_anchor_rect(pos, 0, 0, 20, 32);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	pop.surface = wl_compositor_create_surface(client.compositor);
	pop.xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base, pop.surface);
	xdg_surface_add_listener(pop.xdg_surface, &popup_surface_listener, &pop);
	pop.popup = xdg_surface_get_popup(pop.xdg_surface, NULL, pos);
	xdg_popup_add_listener(pop.popup, &popup_listener, &pop);
	zwlr_layer_surface_v1_get_popup(bar.layer_surface, pop.popup);
	xdg_positioner_destroy(pos);
	wl_surface_commit(pop.surface);
	for (unsigned int i = 0; i < 200 && !pop.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(pop.configured);
	xdg_popup_destroy(pop.popup);
	xdg_surface_destroy(pop.xdg_surface);
	wl_surface_destroy(pop.surface);

	/* Destroying the bar restores m1->usable and expands tiled windows. */
	destroy_layer(&bar);
	for (unsigned int i = 0; i < 50 && m1->usable.y != area1.y; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(m1->usable.y == area1.y && m1->usable.h == area1.h);
	CHECK(c0->geom.y < area1.y + 32);

	/* Ordinary TOP EXCLUSIVE layer surface grabs keyboard focus on map,
	 * restores on destroy. */
	struct test_layer launcher;
	create_layer(&client, &launcher, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
		200, 100, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	for (unsigned int i = 0; i < 200 && !launcher.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.focused_layer != NULL);
	CHECK(client.keyboard_focus == launcher.surface);
	CHECK(tp_focused(&server) == NULL);
	destroy_layer(&launcher);
	for (unsigned int i = 0; i < 50 && server.focused_layer != NULL; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.focused_layer == NULL);
	CHECK(tp_focused(&server) == c0);

	/* Exclusive OVERLAY keyboard focus is allowed for direct same-UID,
	 * non-sandboxed clients (matching TOP + EXCLUSIVE and zwlr_layer_shell_v1). */
	CHECK(tp_security_allow_exclusive_overlay(&server,
		wl_resource_get_client(c0->toplevel->resource)));
	struct test_layer overlay_excl;
	create_layer(&client, &overlay_excl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
		200, 100, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	for (unsigned int i = 0; i < 200 && !overlay_excl.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.focused_layer != NULL);
	CHECK(client.keyboard_focus == overlay_excl.surface);
	CHECK(tp_focused(&server) == NULL);
	destroy_layer(&overlay_excl);
	for (unsigned int i = 0; i < 50 && server.focused_layer != NULL; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.focused_layer == NULL);
	CHECK(tp_focused(&server) == c0);

	/* ON_DEMAND layer surface takes focus only when clicked, yields on window click. */
	struct test_layer ondemand;
	create_layer(&client, &ondemand, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
		200, 100, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
	for (unsigned int i = 0; i < 200 && !ondemand.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.focused_layer == NULL);
	CHECK(tp_focused(&server) == c0);
	wlr_cursor_warp(server.cursor, &pointer.base, area1.x + 1.0, area1.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 140);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 141);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 142);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.focused_layer != NULL);
	CHECK(client.keyboard_focus == ondemand.surface);
	destroy_layer(&ondemand);
	for (unsigned int i = 0; i < 50 && server.focused_layer != NULL; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(tp_focused(&server) == c0);

	/* ---- 0.0.5: multi-monitor focus, window move, hot-plug recovery ---- */
	struct wlr_backend *headless = m1->output->backend;
	struct wlr_output *o2 = wlr_headless_add_output(headless, 800, 600);
	CHECK(o2 != NULL);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.monitors) == 2);
	struct tp_monitor *m2 = o2->data;
	CHECK(m2 != NULL && m2 != m1);
	CHECK(tp_monitor_in_dir(&server, m1, TP_DIR_RIGHT) == m2);
	CHECK(tp_monitor_in_dir(&server, m2, TP_DIR_LEFT) == m1);

	/* Meta+Period / Meta+Comma switch selected monitor. */
	CHECK(server.selmon == m1);
	chord(&keyboard, KEY_DOT, false, false);
	CHECK(server.selmon == m2);
	chord(&keyboard, KEY_COMMA, false, false);
	CHECK(server.selmon == m1);

	/* Meta+Shift+Period moves focused window c1 from m1 to m2. */
	tp_focus(&server, c1);
	chord(&keyboard, KEY_DOT, true, false);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c1->mon == m2 && c0->mon == m1);
	CHECK(c1->geom.x >= m2->usable.x);
	chord(&keyboard, KEY_DOT, false, false);
	CHECK(server.selmon == m2 && tp_focused(&server) == c1);

	/* A stationary pointer does not undo keyboard focus, but motion does. */
	server.settings.focus_follows_mouse = true;
	wlr_cursor_warp(server.cursor, &pointer.base, c0->geom.x + 1.0,
					c0->geom.y + 1.0);
	tp_pointer_refocus_now(&server);
	CHECK(tp_focused(&server) == c1);
	emit_motion(&server, &pointer, 0, 0, 155);
	CHECK(tp_focused(&server) == c0 && server.selmon == m1);
	server.settings.focus_follows_mouse = false;

	/* Tiled movement is committed only on drop and preserves tiling. */
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 156);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 157);
	CHECK(server.grab.mode == TP_GRAB_MOVE && server.grab.tiled);
	CHECK(server.grab.move_outline != NULL);
	emit_motion(&server, &pointer, m2->area.x + 40.0 - server.cursor->x, 0, 158);
	CHECK(c0->mon == m1);
	tp_pointer_cancel_grab(&server, c0);
	CHECK(server.grab.mode == TP_GRAB_NONE && c0->mon == m1);
	wlr_cursor_warp(server.cursor, &pointer.base, c0->geom.x + 1.0,
					c0->geom.y + 1.0);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 159);
	emit_motion(&server, &pointer, m2->area.x + 40.0 - server.cursor->x, 0, 160);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 161);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 162);
	CHECK(c0->mon == m2 && !c0->floating && tp_focused(&server) == c0);
	CHECK(c0->tags == tp_tagview_current(&m2->view));
	tp_client_set_monitor(c0, m1, true);
	tp_arrange_flush(&server);

	/* Dragging a floating window onto another monitor moves it there in
	 * full: monitor, tags, focus slots and layout trees (R1 findings 6 and
	 * 13). Then Meta+Shift+Comma brings it back and it tiles again. */
	tp_focus(&server, c0);
	chord(&keyboard, KEY_V, false, false);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c0->floating && c0->mon == m1);
	/* The test clients draw a 2x2 buffer: point inside it. */
	wlr_cursor_warp(server.cursor, &pointer.base, c0->float_geom.x + 1.0,
		c0->float_geom.y + 1.0);
	emit_motion(&server, &pointer, 0, 0, 160);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 161);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 162);
	CHECK(server.grab.mode == TP_GRAB_MOVE && server.grab.client == c0);
	emit_motion(&server, &pointer, m2->area.x + 40.0 - server.cursor->x, 0, 163);
	CHECK(c0->mon == m2);
	CHECK(c0->tags == tp_tagview_current(&m2->view));
	CHECK(c0->home_output == NULL);
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++) {
		CHECK(m1->focus[i] != c0);
		CHECK(!tp_dwindle_find(&m1->layouts[i].dwindle, c0));
	}
	CHECK(m2->focus[tp_tags_slot(tp_tagview_current(&m2->view))] == c0);
	emit_button(&server, &pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 164);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 165);
	CHECK(server.grab.mode == TP_GRAB_NONE && c0->mon == m2);
	CHECK(tp_focused(&server) == c0);
	chord(&keyboard, KEY_COMMA, true, false);
	CHECK(c0->mon == m1 && c0->floating);
	CHECK(c0->float_geom.x >= m1->usable.x &&
		c0->float_geom.x + c0->float_geom.w <= m1->usable.x + m1->usable.w);
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++)
		CHECK(m2->focus[i] != c0);
	tp_focus(&server, c0);
	chord(&keyboard, KEY_V, false, false);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(!c0->floating && c0->mon == m1);
	CHECK(tp_dwindle_find(&tp_monitor_layout(m1)->dwindle, c0) != NULL);
	tp_monitor_focus(&server, m2);
	tp_focus(&server, c1);
	CHECK(server.selmon == m2 && tp_focused(&server) == c1);

	/* B9: Meta+V on a window that mapped tiled floats it at half its own
	 * monitor, centred. A later float returns to its last floating place,
	 * and that place follows the window to another monitor. Before 0.1.17
	 * it floated at its first size at the layout's (0, 0), which is on
	 * m1, and a tiled window's floating place stayed behind on a move. */
	{
		struct wl_buffer *big = create_sized_buffer(&client, 200, 150);
		CHECK(big != NULL);
		if (!big) goto fail;
		int before = wl_list_length(&server.clients);
		configure_window(&client, &client.windows[2], "float-later");
		client.windows[2].buffer = big;
		for (unsigned int i = 0;
				i < 1000 && wl_list_length(&server.clients) == before; i++)
			if (!pump(&client, &server)) goto fail;
		struct tp_client *cb = tp_focused(&server);
		CHECK(cb && cb != c0 && cb != c1);
		if (!cb || cb == c0 || cb == c1) goto fail;
		CHECK(cb->mon == m2 && !cb->floating);
		CHECK(cb->toplevel->base->geometry.width == 200);

		chord(&keyboard, KEY_V, false, false);
		wl_event_loop_dispatch(server.loop, 0);
		struct tp_rect u2 = m2->usable;
		int32_t hw = u2.w / 2, hh = u2.h / 2;
		CHECK(cb->floating && cb->mon == m2);
		CHECK(cb->float_geom.w == hw && cb->float_geom.h == hh);
		CHECK(cb->float_geom.x == u2.x + (u2.w - hw) / 2 &&
			cb->float_geom.y == u2.y + (u2.h - hh) / 2);
		CHECK(cb->geom.x == cb->float_geom.x && cb->geom.y == cb->float_geom.y);

		/* As if Fred had moved it: tile it, float it, it comes back. */
		cb->float_geom.x += 30;
		struct tp_rect placed = cb->float_geom;
		chord(&keyboard, KEY_V, false, false);
		CHECK(!cb->floating);
		chord(&keyboard, KEY_V, false, false);
		CHECK(cb->floating && cb->float_geom.x == placed.x &&
			cb->float_geom.y == placed.y && cb->float_geom.w == placed.w &&
			cb->float_geom.h == placed.h);

		/* Tile it, send it to m1 while tiled, float it: it floats on m1. */
		chord(&keyboard, KEY_V, false, false);
		chord(&keyboard, KEY_COMMA, true, false);
		CHECK(cb->mon == m1 && !cb->floating);
		tp_focus(&server, cb);
		chord(&keyboard, KEY_V, false, false);
		struct tp_rect u1m = m1->usable;
		CHECK(cb->floating && cb->mon == m1);
		CHECK(cb->float_geom.x >= u1m.x && cb->float_geom.y >= u1m.y &&
			cb->float_geom.x + cb->float_geom.w <= u1m.x + u1m.w &&
			cb->float_geom.y + cb->float_geom.h <= u1m.y + u1m.h);

		/* A floating place off its monitor (as for a window that mapped
		 * with no monitor) is treated as a first float. */
		chord(&keyboard, KEY_V, false, false);
		CHECK(!cb->floating);
		cb->float_geom = (struct tp_rect){-5000, -5000, 200, 150};
		chord(&keyboard, KEY_V, false, false);
		CHECK(cb->floating && cb->float_geom.w == u1m.w / 2 &&
			cb->float_geom.h == u1m.h / 2 &&
			cb->float_geom.x == u1m.x + (u1m.w - u1m.w / 2) / 2 &&
			cb->float_geom.y == u1m.y + (u1m.h - u1m.h / 2) / 2);

		destroy_window(&client.windows[2]);
		for (unsigned int i = 0;
				i < 100 && wl_list_length(&server.clients) > before; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(wl_list_length(&server.clients) == before);
		wl_buffer_destroy(big);
		tp_monitor_focus(&server, m2);
		tp_focus(&server, c1);
		CHECK(server.selmon == m2 && tp_focused(&server) == c1);
	}

	/* Layer surface on m2 is cleanly closed when m2 is unplugged; c1 moves
	 * to m1 keeping its tags and remembering m2's output name. */
	struct test_layer bar2;
	create_layer(&client, &bar2, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
		0, 24, 24, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	for (unsigned int i = 0; i < 200 && !bar2.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(m2->usable.y == m2->area.y + 24);

	/* A monitor that is removed and returns, as when a suspend drops and
	 * re-adds a display, comes back on the tags it showed, with its tiled
	 * layout and its windows' floating places. */
	tp_tagview_show(&m2->view, tp_tag_mask(3));
	tp_tagview_show(&m2->view, c1->tags);
	struct tp_tagview saved_view = m2->view;
	c1->float_geom = (struct tp_rect){m2->area.x + 100, m2->area.y + 80,
		300, 200};
	tp_arrange(m2);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(tp_layout_contains(tp_monitor_layout(m2), c1));

	tp_tags_t saved_tags = c1->tags;
	char saved_name[64];
	snprintf(saved_name, sizeof(saved_name), "%s", o2->name);
	wlr_output_destroy(o2);
	for (unsigned int i = 0; i < 50 && !bar2.closed; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(bar2.closed);
	destroy_layer(&bar2);
	CHECK(wl_list_length(&server.monitors) == 1);
	CHECK(c1->mon == m1);
	CHECK(c1->tags == saved_tags);
	CHECK(c1->home_output != NULL && strcmp(c1->home_output, saved_name) == 0);
	struct tp_parked_monitor *parked =
		tp_monitor_parked_find(&server, saved_name);
	unsigned int saved_slot = tp_tags_slot(tp_tagview_current(&saved_view));
	const void *saved_root = NULL;
	CHECK(parked != NULL);
	if (parked) {
		saved_root = parked->layouts[saved_slot].dwindle.root;
		CHECK(saved_root != NULL);
		CHECK(tp_tagview_current(&parked->view) ==
			tp_tagview_current(&saved_view));
		CHECK(tp_layout_contains(
			&parked->layouts[tp_tags_slot(tp_tagview_current(&saved_view))],
			c1));
	}

	/* When a monitor with the remembered name returns, c1 moves back.
	 * Headless outputs never reuse a name, so rename what was kept. */
	free(c1->home_output);
	c1->home_output = strdup("HEADLESS-3");
	if (parked) {
		free(parked->name);
		parked->name = strdup("HEADLESS-3");
	}
	struct wlr_output *o3 = wlr_headless_add_output(headless, 800, 600);
	CHECK(o3 != NULL && strcmp(o3->name, "HEADLESS-3") == 0);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(c1->mon == o3->data);
	CHECK(c1->home_output == NULL);
	CHECK(c1->tags == saved_tags);
	struct tp_monitor *m3 = o3->data;
	CHECK(tp_tagview_current(&m3->view) == tp_tagview_current(&saved_view));
	CHECK(m3->view.slot[m3->view.cur ^ 1u] == tp_tag_mask(3));
	CHECK(tp_layout_contains(tp_monitor_layout(m3), c1));
	/* The same tiled layout tree, not a fresh one built around c1. */
	CHECK((const void *)m3->layouts[saved_slot].dwindle.root == saved_root);
	CHECK(c1->float_geom.x == m3->area.x + 100 &&
		c1->float_geom.y == m3->area.y + 80 && c1->float_geom.w == 300 &&
		c1->float_geom.h == 200);
	CHECK(tp_monitor_parked_find(&server, "HEADLESS-3") == NULL);

	/* 0.2.14: monitor mode commits a mode, and refuses an output that is
	 * off, where a commit would retrain nothing. */
	{
		char why[128] = {0};
		CHECK(tp_monitor_set_mode(&server, o3->name, 1024, 768, 50000, why,
			sizeof(why)));
		CHECK(o3->width == 1024 && o3->height == 768 && o3->refresh == 50000);
		CHECK(m3->area.w == 1024 && m3->area.h == 768);
		CHECK(!tp_monitor_set_mode(&server, "NO-SUCH-OUTPUT", 800, 600, 0, why,
			sizeof(why)));
		CHECK(strcmp(why, "unknown monitor") == 0);
		CHECK(tp_output_power_set(&server, o3, false));
		CHECK(!tp_monitor_set_mode(&server, o3->name, 800, 600, 60000, why,
			sizeof(why)));
		CHECK(strstr(why, "off") != NULL);
		CHECK(o3->width == 1024);
		CHECK(tp_output_power_set(&server, o3, true));
		CHECK(tp_monitor_set_mode(&server, o3->name, 800, 600, 60000, why,
			sizeof(why)));
		CHECK(o3->width == 800 && o3->height == 600 && o3->refresh == 60000);
		for (unsigned int i = 0; i < 20; i++)
			if (!pump(&client, &server)) goto fail;
	}

	/*
	 * Lua 0.1.0 + 0.1.1 integration: load a real init.lua setting custom gaps,
	 * custom bindings, unbinding a default, and plain Lua callbacks (including
	 * a runaway callback guarded by the watchdog); then verify a runaway
	 * init.lua rolls back both settings and bindings atomically.
	 */
	CHECK(server.lua != NULL);
	char lua_dir[] = "/tmp/tam-pane-headless-lua.XXXXXX";
	CHECK(mkdtemp(lua_dir) != NULL);
	char lua_cfg[256];
	snprintf(lua_cfg, sizeof(lua_cfg), "%s/init.lua", lua_dir);
	FILE *lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.config{ gap_inner = 20, gap_outer = 30 }\n"
			  "tp.unbind('Meta+v')\n"
			  "tp.bind('Meta+q', function() tp.dsp.toggle_float()() end)\n"
			  "tp.bind('Meta+b', function() while true do end end)\n",
			  lf);
		fclose(lf);
	}
	CHECK(tp_lua_load_file(server.lua, lua_cfg, true, &server.settings,
						   &server.settings));
	CHECK(server.settings.gap_inner == 20);
	CHECK(server.settings.gap_outer == 30);
	tp_arrange(c1->mon);
	wl_event_loop_dispatch(server.loop, 0);
	CHECK(c1->geom.x == c1->mon->usable.x + 30);
	CHECK(c1->geom.y == c1->mon->usable.y + 30);
	CHECK(c1->geom.w == c1->mon->usable.w - 60);
	CHECK(c1->geom.h == c1->mon->usable.h - 60);

	/* Meta+v was unbound so it no longer toggles floating; Meta+q callback
	 * invokes tp.dsp.toggle_float()() and does toggle floating. */
	tp_focus(&server, c1);
	CHECK(!c1->floating);
	chord(&keyboard, KEY_V, false, false);
	CHECK(!c1->floating);
	chord(&keyboard, KEY_Q, false, false);
	CHECK(c1->floating);
	/* Runaway Lua callback on Meta+b is cut off cleanly; compositor stays live. */
	chord(&keyboard, KEY_B, false, false);
	chord(&keyboard, KEY_Q, false, false);
	CHECK(!c1->floating);

	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.config{ gap_outer = 99 }\n"
			  "tp.clear_binds()\n"
			  "while true do end\n",
			  lf);
		fclose(lf);
	}
	CHECK(!tp_lua_load_file(server.lua, lua_cfg, true, &server.settings,
							&server.settings));
	CHECK(server.settings.gap_outer == 30);
	/* Bindings rolled back too: Meta+q still toggles floating. */
	chord(&keyboard, KEY_Q, false, false);
	CHECK(c1->floating);

	/*
	 * Lua 0.1.2 integration: deferred event hooks for window.open,
	 * window.close, window.focus, tag.change, monitor.add, monitor.remove,
	 * dispatcher invocation from a hook, and runaway hook watchdog cutoff.
	 */
	tp_lua_set_spawn_fn(server.lua, record_hook_spawn);
	hook_event_count = 0;
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.on('window.open', function(ev)\n"
			  "  tp.spawn('open:' .. ev.app_id .. ':' .. ev.title .. ':' .. ev.monitor)\n"
			  "  if ev.title == 'auto-float' then tp.dsp.toggle_float()() end\n"
			  "end)\n"
			  "tp.on('window.close', function(ev)\n"
			  "  tp.spawn('close:' .. ev.title)\n"
			  "end)\n"
			  "tp.on('window.focus', function(ev)\n"
			  "  tp.spawn('focus:' .. tostring(ev.focused) .. ':' .. ev.title)\n"
			  "end)\n"
			  "tp.on('tag.change', function(ev)\n"
			  "  tp.spawn(string.format('tag:%s:%d->%d:%d', ev.monitor, ev.prev_tags, ev.tags, ev.tag))\n"
			  "end)\n"
			  "tp.on('monitor.add', function(ev)\n"
			  "  tp.spawn(string.format('madd:%s:%dx%d', ev.name, ev.w, ev.h))\n"
			  "end)\n"
			  "tp.on('monitor.remove', function(ev)\n"
			  "  tp.spawn(string.format('mrem:%s:%dx%d', ev.name, ev.w, ev.h))\n"
			  "end)\n",
			  lf);
		fclose(lf);
	}
	CHECK(tp_lua_load_file(server.lua, lua_cfg, true, &server.settings,
						   &server.settings));

	/* Open a window titled 'auto-float': deferred window.open fires before
	 * window.focus and safely toggles floating via tp.dsp.toggle_float()(). */
	struct test_window hook_win;
	configure_window(&client, &hook_win, "auto-float");
	for (unsigned int i = 0; i < 100 && !hook_win.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	int idx_open = find_hook_event("open:tam-pane-test:auto-float:");
	int idx_focus = find_hook_event("focus:true:auto-float");
	CHECK(idx_open >= 0);
	CHECK(idx_focus > idx_open);
	struct tp_client *hook_c = tp_focused(&server);
	CHECK(hook_c != NULL && hook_c->floating);

	/* Tag change fires tag.change and focus clear (empty tag 5), then
	 * Meta+Tab returns to previous tag and restores focus. */
	hook_event_count = 0;
	chord(&keyboard, KEY_5, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	int idx_tag = find_hook_event("tag:");
	int idx_unfocus = find_hook_event("focus:false:");
	CHECK(idx_tag >= 0);
	CHECK(idx_unfocus > idx_tag);
	CHECK(has_hook_event("tag:") && strstr(hook_events[idx_tag], ":5") != NULL);

	hook_event_count = 0;
	chord(&keyboard, KEY_TAB, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(has_hook_event("tag:"));
	CHECK(has_hook_event("focus:true:auto-float"));

	/* Closing hook_win fires window.close before window.focus. */
	hook_event_count = 0;
	destroy_window(&hook_win);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	int idx_close = find_hook_event("close:auto-float");
	CHECK(idx_close >= 0);

	/* Adding and removing a headless output fires monitor.add / monitor.remove. */
	hook_event_count = 0;
	struct wlr_output *o4 = wlr_headless_add_output(headless, 640, 480);
	CHECK(o4 != NULL);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(has_hook_event("madd:HEADLESS-4:640x480"));

	/* A window tiled on a removed monitor stays in its kept layout until
	 * it closes; closing it drops it there too, so the kept layout never
	 * holds a freed window. */
	struct tp_monitor *m4 = o4->data;
	struct tp_monitor *keep_selmon = server.selmon;
	struct tp_client *keep_focus = tp_focused(&server);
	tp_monitor_focus(&server, m4);
	struct test_window parked_win;
	configure_window(&client, &parked_win, "parked-tile");
	for (unsigned int i = 0; i < 100 && !parked_win.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_client *parked_c = tp_focused(&server);
	CHECK(parked_c != NULL && parked_c->mon == m4 && !parked_c->floating);
	CHECK(tp_layout_contains(tp_monitor_layout(m4), parked_c));

	hook_event_count = 0;
	wlr_output_destroy(o4);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(has_hook_event("mrem:HEADLESS-4:640x480"));
	struct tp_parked_monitor *p4 = tp_monitor_parked_find(&server, "HEADLESS-4");
	CHECK(p4 != NULL);
	bool p4_held = false;
	for (unsigned int i = 0; p4 && i < TP_TAG_SLOTS; i++)
		p4_held |= tp_layout_contains(&p4->layouts[i], parked_c);
	CHECK(p4_held);
	destroy_window(&parked_win);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	p4 = tp_monitor_parked_find(&server, "HEADLESS-4");
	CHECK(p4 != NULL);
	for (unsigned int i = 0; p4 && i < TP_TAG_SLOTS; i++)
		CHECK(tp_layout_count(&p4->layouts[i]) == 0);
	tp_monitor_focus(&server, keep_selmon);
	if (keep_focus)
		tp_focus(&server, keep_focus);
	hook_event_count = 0;

	/* Runaway while true do end in a hook is cut off by the watchdog and the
	 * compositor keeps handling subsequent input. */
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.on('tag.change', function() while true do end end)\n"
			  "tp.on('tag.change', function() tp.spawn('after-watchdog') end)\n",
			  lf);
		fclose(lf);
	}
	CHECK(tp_lua_load_file(server.lua, lua_cfg, true, &server.settings,
						   &server.settings));
	hook_event_count = 0;
	chord(&keyboard, KEY_4, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(has_hook_event("after-watchdog"));
	CHECK(tp_tagview_current(&server.selmon->view) == tp_tag_mask(4));

	/* A hook whose action emits its own event again (tag.change switching
	 * tags) must not hang the event loop: the cascade is cut after
	 * TP_EVENT_CASCADE_MAX rounds and the compositor keeps handling input. */
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("local n = 0\n"
			  "tp.on('tag.change', function(ev)\n"
			  "  n = n + 1\n"
			  "  tp.spawn('loop:' .. n)\n"
			  "  tp.dsp.tag('view', ev.tag == 6 and 7 or 6)()\n"
			  "end)\n",
			  lf);
		fclose(lf);
	}
	CHECK(tp_lua_load_file(server.lua, lua_cfg, true, &server.settings,
						   &server.settings));
	hook_event_count = 0;
	signal(SIGALRM, on_hang_alarm);
	alarm(10);
	chord(&keyboard, KEY_6, false, false);
	for (unsigned int i = 0; i < 200; i++)
		if (!pump(&client, &server)) goto fail;
	alarm(0);
	CHECK(has_hook_event("loop:1"));
	CHECK(hook_event_count >= 2);
	CHECK(hook_event_count <= TP_EVENT_CASCADE_MAX + 1);
	CHECK(server.events.len == 0);
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("-- no hooks\n", lf);
		fclose(lf);
	}
	CHECK(tp_lua_load_file(server.lua, lua_cfg, true, &server.settings,
						   &server.settings));
	chord(&keyboard, KEY_4, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(tp_tagview_current(&server.selmon->view) == tp_tag_mask(4));

	/*
	 * Lua window rules (0.1.3): floating/size/position, tag routing without
	 * stealing focus, multi-tag, multi-monitor, unknown-monitor fallback,
	 * and window.open observing rule-applied state.
	 */
	chord(&keyboard, KEY_1, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(tp_tagview_current(&server.selmon->view) == tp_tag_mask(1));

	struct wlr_output *o5 = wlr_headless_add_output(headless, 800, 600);
	CHECK(o5 != NULL);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_monitor *mon_primary = server.selmon;
	struct tp_monitor *mon_second = tp_monitor_by_name(&server, "HEADLESS-5");
	CHECK(mon_primary != NULL && mon_second != NULL && mon_primary != mon_second);

	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.rule{\n"
			  "  match = { app_id = '^rule%-float$' },\n"
			  "  floating = true,\n"
			  "  size = { 500, 320 },\n"
			  "  position = { 40, 50 },\n"
			  "}\n"
			  "tp.rule{\n"
			  "  match = { title = '^RuleTag2$' },\n"
			  "  tag = 2,\n"
			  "}\n"
			  "tp.rule{\n"
			  "  match = { app_id = '^rule%-multi$' },\n"
			  "  tags = { 1, 3 },\n"
			  "  floating = false,\n"
			  "}\n"
			  "tp.rule{\n"
			  "  match = { app_id = '^rule%-mon2$' },\n"
			  "  monitor = 'HEADLESS-5',\n"
			  "  floating = true,\n"
			  "  size = { 300, 200 },\n"
			  "  position = 'center',\n"
			  "}\n"
			  "tp.rule{\n"
			  "  match = { app_id = '^rule%-badmon$' },\n"
			  "  monitor = 'NO-SUCH-MON',\n"
			  "  floating = true,\n"
			  "}\n"
			  "tp.on('window.open', function(ev)\n"
			  "  tp.spawn(string.format('ropen:%s:%s:%d:%s:%dx%d+%d+%d',\n"
			  "    ev.app_id, ev.monitor, ev.tag, tostring(ev.floating),\n"
			  "    ev.w, ev.h, ev.x, ev.y))\n"
			  "end)\n",
			  lf);
		fclose(lf);
	}
	CHECK(tp_lua_load_file(server.lua, lua_cfg, true, &server.settings,
						   &server.settings));
	CHECK(tp_lua_rule_count(server.lua) == 5);

	/* 1) Floating + size + position rule on primary monitor. */
	hook_event_count = 0;
	struct test_window rw_float;
	configure_window_app(&client, &rw_float, "rule-float", "FloatWin");
	for (unsigned int i = 0; i < 100 && !rw_float.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_client *c_float = tp_focused(&server);
	CHECK(c_float != NULL && c_float->floating);
	CHECK(c_float->geom.w == 500 && c_float->geom.h == 320);
	CHECK(c_float->geom.x == mon_primary->usable.x + 40);
	CHECK(c_float->geom.y == mon_primary->usable.y + 50);
	char expected_ropen[128];
	snprintf(expected_ropen, sizeof(expected_ropen),
			 "ropen:rule-float:%s:1:true:500x320+%d+%d",
			 mon_primary->output->name,
			 mon_primary->usable.x + 40, mon_primary->usable.y + 50);
	CHECK(has_hook_event(expected_ropen));

	/* 2) Title rule routes window to tag 2 without stealing focus on tag 1. */
	struct test_window rw_tag2;
	configure_window_app(&client, &rw_tag2, "tam-pane-test", "RuleTag2");
	for (unsigned int i = 0; i < 100 && !rw_tag2.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(tp_focused(&server) == c_float);
	chord(&keyboard, KEY_2, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_client *c_tag2 = tp_focused(&server);
	CHECK(c_tag2 != NULL && c_tag2 != c_float);
	CHECK(c_tag2->tags == tp_tag_mask(2) && tp_client_visible(c_tag2));
	chord(&keyboard, KEY_1, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;

	/* 3) Multi-tag rule { 1, 3 } with floating = false. */
	struct test_window rw_multi;
	configure_window_app(&client, &rw_multi, "rule-multi", "Multi");
	for (unsigned int i = 0; i < 100 && !rw_multi.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_client *c_multi = tp_focused(&server);
	CHECK(c_multi != NULL && !c_multi->floating);
	CHECK(c_multi->tags == (tp_tag_mask(1) | tp_tag_mask(3)));

	/* 4) Monitor rule places centered floating window on HEADLESS-5 without
	 * stealing focus from mon_primary. */
	hook_event_count = 0;
	struct test_window rw_mon2;
	configure_window_app(&client, &rw_mon2, "rule-mon2", "Mon2");
	for (unsigned int i = 0; i < 100 && !rw_mon2.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.selmon == mon_primary);
	CHECK(tp_focused(&server) == c_multi);
	int32_t exp_cx = mon_second->usable.x + (mon_second->usable.w - 300) / 2;
	int32_t exp_cy = mon_second->usable.y + (mon_second->usable.h - 200) / 2;
	snprintf(expected_ropen, sizeof(expected_ropen),
			 "ropen:rule-mon2:HEADLESS-5:1:true:300x200+%d+%d", exp_cx, exp_cy);
	CHECK(has_hook_event(expected_ropen));

	/* 5) Unknown monitor falls back to active monitor while applying other fields. */
	struct test_window rw_badmon;
	configure_window_app(&client, &rw_badmon, "rule-badmon", "BadMon");
	for (unsigned int i = 0; i < 100 && !rw_badmon.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_client *c_badmon = tp_focused(&server);
	CHECK(c_badmon != NULL && c_badmon->mon == mon_primary && c_badmon->floating);

	destroy_window(&rw_badmon);
	destroy_window(&rw_mon2);
	destroy_window(&rw_multi);
	destroy_window(&rw_tag2);
	destroy_window(&rw_float);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;

	/* ---- 0.1.4 Live configuration reload ---- */
	tp_server_set_config_path(&server, lua_cfg);

	/* Map a tiled window on mon_primary so we can observe live gap relayout. */
	struct test_window rw_live;
	configure_window_app(&client, &rw_live, "live-tile", "LiveTile");
	for (unsigned int i = 0; i < 100 && !rw_live.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_client *c_live = tp_focused(&server);
	CHECK(c_live != NULL && !c_live->floating);
	int32_t orig_w = c_live->geom.w;

	const char pending_reload[] =
		"tp.bind('F9', function() tp.spawn('obsolete-release') end, { release = true })";
	CHECK(tp_lua_exec_string(server.lua, pending_reload, sizeof(pending_reload) - 1,
							 "pending-reload", 100000, 100, &server.settings,
							 &server.settings, NULL, 0));
	send_key(&keyboard, KEY_F9, WL_KEYBOARD_KEY_STATE_PRESSED, 290);
	CHECK(key_consumed(&server, KEY_F9));

	/* 1) Write updated config and trigger Meta+Shift+R (default reload bind). */
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.config{\n"
			  "  gap_inner = 20,\n"
			  "  gap_outer = 34,\n"
			  "  width_bias = 1.5,\n"
			  "  preserve_split = true,\n"
			  "  proportional_split = true,\n"
			  "  repeat_rate = 45,\n"
			  "  repeat_delay = 250,\n"
			  "}\n"
			  "tp.bind('Meta+u', function() tp.spawn('reloaded-u') end)\n"
			  "tp.bind('Meta+Shift+u', function() tp.dsp.reload()() end)\n"
			  "tp.rule{ match = { app_id = '^post%-reload$' }, floating = true, size = { 420, 260 } }\n",
			  lf);
		fclose(lf);
	}
	chord(&keyboard, KEY_R, true, false);
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;

	hook_event_count = 0;
	client.key_events = 0;
	send_key(&keyboard, KEY_F9, WL_KEYBOARD_KEY_STATE_RELEASED, 291);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(hook_event_count == 0 && client.key_events == 0);
	CHECK(!key_consumed(&server, KEY_F9));

	CHECK(server.settings.gap_inner == 20 && server.settings.gap_outer == 34);
	CHECK(c_live->geom.w == mon_primary->usable.w - 68);
	CHECK(c_live->geom.w != orig_w);
	CHECK(mon_primary->layouts[1].dwindle.preserve_split == true);
	CHECK(mon_primary->layouts[1].dwindle.proportional_split == true);
	CHECK(mon_primary->layouts[1].dwindle.width_bias > 1.49f &&
		  mon_primary->layouts[1].dwindle.width_bias < 1.51f);
	CHECK(keyboard.repeat_info.rate == 45 &&
		  keyboard.repeat_info.delay == 250);
	hook_event_count = 0;
	chord(&keyboard, KEY_U, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(has_hook_event("reloaded-u"));
	CHECK(tp_lua_rule_count(server.lua) == 1);

	/* 2) Broken config via Meta+Shift+R keeps last good config untouched. */
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.config{ gap_outer = 2 }\nerror('bad-live-reload')\n", lf);
		fclose(lf);
	}
	chord(&keyboard, KEY_R, true, false);
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.settings.gap_outer == 34);
	CHECK(c_live->geom.w == mon_primary->usable.w - 68);
	CHECK(keyboard.repeat_info.rate == 45);
	hook_event_count = 0;
	chord(&keyboard, KEY_U, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(has_hook_event("reloaded-u"));

	/* 3) Deferred reload from inside a Lua callback (Meta+Shift+U) works
	 * safely without re-entrancy issues and reverts omitted settings. */
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.config{ gap_outer = 10 }\n"
			  "tp.bind('Meta+y', function() tp.spawn('reloaded-y') end)\n",
			  lf);
		fclose(lf);
	}
	chord(&keyboard, KEY_U, true, false);
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.settings.gap_outer == 10);
	CHECK(c_live->geom.w == mon_primary->usable.w - 20);
	CHECK(mon_primary->layouts[1].dwindle.preserve_split == false);
	CHECK(keyboard.repeat_info.rate == 25 &&
		  keyboard.repeat_info.delay == 600);
	hook_event_count = 0;
	chord(&keyboard, KEY_U, false, false);
	chord(&keyboard, KEY_Y, false, false);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(!has_hook_event("reloaded-u"));
	CHECK(has_hook_event("reloaded-y"));

	destroy_window(&rw_live);
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;

	wlr_output_destroy(o5);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;

	/* Compositor keys (0.1.9), handled in C before Lua and clients. With no
	 * session (nested, headless) Ctrl+Alt+F-keys pass through; with one,
	 * they switch VT and are consumed. */
	server.change_vt = NULL;
	send_key(&keyboard, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED, 300);
	send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 301);
	send_key(&keyboard, KEY_F3, WL_KEYBOARD_KEY_STATE_PRESSED, 302);
	CHECK(!key_consumed(&server, KEY_F3));
	send_key(&keyboard, KEY_F3, WL_KEYBOARD_KEY_STATE_RELEASED, 303);
	server.change_vt = mock_change_vt;
	last_vt = 0;
	send_key(&keyboard, KEY_F2, WL_KEYBOARD_KEY_STATE_PRESSED, 304);
	CHECK(last_vt == 2);
	CHECK(key_consumed(&server, KEY_F2));
	send_key(&keyboard, KEY_F2, WL_KEYBOARD_KEY_STATE_RELEASED, 305);
	CHECK(!key_consumed(&server, KEY_F2));
	send_key(&keyboard, KEY_F12, WL_KEYBOARD_KEY_STATE_PRESSED, 306);
	CHECK(last_vt == 12);
	send_key(&keyboard, KEY_F12, WL_KEYBOARD_KEY_STATE_RELEASED, 307);
	send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 308);
	send_key(&keyboard, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED, 309);
	/* Plain F2 is an ordinary key. */
	last_vt = 0;
	send_key(&keyboard, KEY_F2, WL_KEYBOARD_KEY_STATE_PRESSED, 310);
	CHECK(last_vt == 0 && !key_consumed(&server, KEY_F2));
	send_key(&keyboard, KEY_F2, WL_KEYBOARD_KEY_STATE_RELEASED, 311);

	/* The emergency quit works even when the config removed every
	 * binding. */
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.clear_binds()\n", lf);
		fclose(lf);
	}
	CHECK(tp_lua_load_file(server.lua, lua_cfg, true, &server.settings,
						   &server.settings));
	CHECK(tp_lua_binding_count(server.lua) == 0);
	server.quit_requested = false;
	chord_mod(&keyboard, KEY_E, true, false, false); /* Meta+Shift+E: unbound */
	CHECK(!server.quit_requested);
	send_key(&keyboard, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED, 320);
	send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 321);
	send_key(&keyboard, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_PRESSED, 322);
	CHECK(!server.quit_requested); /* Ctrl+Alt+Backspace alone is not it */
	send_key(&keyboard, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_RELEASED, 323);
	send_key(&keyboard, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_PRESSED, 324);
	send_key(&keyboard, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_PRESSED, 325);
	CHECK(server.quit_requested);
	CHECK(key_consumed(&server, KEY_BACKSPACE));
	send_key(&keyboard, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_RELEASED, 326);
	send_key(&keyboard, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_RELEASED, 327);
	send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 328);
	send_key(&keyboard, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED, 329);

	/* ---- 0.1.18: privileged-protocol filter (R1 finding 9) ---- */
	/* Every global advertised by tam-pane must be explicitly classified. */
	CHECK(client.unclassified_globals == 0);
	CHECK(tp_security_classify_global("zwlr_layer_shell_v1") == TP_GLOBAL_PRIVILEGED);
	CHECK(tp_security_classify_global("wp_security_context_manager_v1") == TP_GLOBAL_PRIVILEGED);
	CHECK(tp_security_classify_global("ext_output_image_capture_source_manager_v1") == TP_GLOBAL_PRIVILEGED);
	CHECK(tp_security_classify_global("ext_foreign_toplevel_image_capture_source_manager_v1") == TP_GLOBAL_PRIVILEGED);
	CHECK(tp_security_classify_global("ext_data_control_manager_v1") == TP_GLOBAL_PRIVILEGED);
	CHECK(tp_security_classify_global("xdg_wm_base") == TP_GLOBAL_UNPRIVILEGED);
	CHECK(tp_security_classify_global("unclassified_future_global_v1") == TP_GLOBAL_UNKNOWN);

	/* Cross-UID and security-context policy decisions. */
	CHECK(tp_security_allow_privileged(false, 1000, 1000) == true);
	CHECK(tp_security_allow_privileged(true, 1000, 1000) == false);
	CHECK(tp_security_allow_privileged(false, 1001, 1000) == false);
	CHECK(tp_security_allow_privileged(true, 1001, 1000) == false);

	/* Direct same-UID client sees both layer-shell and security-context-v1. */
	CHECK(client.layer_shell != NULL && client.layer_shell_name != 0);
	CHECK(client.security_context_mgr != NULL);

	/* Sandboxed client via wp_security_context_v1: unprivileged globals are
	 * offered, privileged globals are hidden, and binding a privileged global
	 * by its numeric name is rejected with a protocol error. */
	if (client.security_context_mgr) {
		char sb_dir[] = "/tmp/tam-pane-sec-XXXXXX";
		CHECK(mkdtemp(sb_dir) != NULL);
		char sb_sock[108];
		snprintf(sb_sock, sizeof(sb_sock), "%s/sandbox.sock", sb_dir);

		int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		CHECK(listen_fd >= 0);
		struct sockaddr_un addr = { .sun_family = AF_UNIX };
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sb_sock);
		CHECK(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
		CHECK(listen(listen_fd, 4) == 0);

		int close_pipe[2] = { -1, -1 };
		CHECK(pipe2(close_pipe, O_CLOEXEC) == 0);

		struct wp_security_context_v1 *sc =
			wp_security_context_manager_v1_create_listener(
				client.security_context_mgr, listen_fd, close_pipe[0]);
		CHECK(sc != NULL);
		wp_security_context_v1_set_sandbox_engine(sc, "test-sandbox");
		wp_security_context_v1_set_app_id(sc, "org.example.Sandboxed");
		wp_security_context_v1_set_instance_id(sc, "inst-1");
		wp_security_context_v1_commit(sc);
		wp_security_context_v1_destroy(sc);
		close(listen_fd);
		close(close_pipe[0]);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;

		struct test_client sb = { .shm_fd = -1 };
		sb.display = wl_display_connect(sb_sock);
		CHECK(sb.display != NULL);
		if (sb.display) {
			sb.registry = wl_display_get_registry(sb.display);
			wl_registry_add_listener(sb.registry, &registry_listener, &sb);
			for (unsigned int i = 0; i < 20; i++) {
				if (!pump(&sb, &server)) break;
			}
			CHECK(sb.compositor != NULL);
			CHECK(sb.shm != NULL);
			CHECK(sb.wm_base != NULL);
			CHECK(sb.seat != NULL);
			CHECK(sb.has_idle_inhibit);
			CHECK(sb.has_cursor_shape);
			CHECK(sb.has_xdg_output);
			CHECK(sb.has_text_input);
			CHECK(sb.has_xdg_activation);
			CHECK(sb.has_primary_selection);
			CHECK(sb.has_xdg_decoration);
			CHECK(sb.layer_shell == NULL);
			CHECK(sb.security_context_mgr == NULL);
			CHECK(!sb.has_wlr_foreign_toplevel);
			CHECK(!sb.has_ext_foreign_toplevel);
			CHECK(!sb.has_ext_workspace);
			CHECK(!sb.has_ext_session_lock);
			CHECK(!sb.has_idle_notify);
			CHECK(!sb.has_output_power);
			CHECK(!sb.has_output_mgmt);
			CHECK(!sb.has_gamma_control);
			CHECK(!sb.has_screencopy);
			CHECK(!sb.has_image_copy);
			CHECK(!sb.has_output_capture_src);
			CHECK(!sb.has_toplevel_capture_src);
			CHECK(!sb.has_input_method);
			CHECK(!sb.has_shortcuts_inhibit);
			CHECK(!sb.has_wlr_data_control);
			CHECK(!sb.has_ext_data_control);

			/* Defense-in-depth: sandboxed clients are also denied exclusive
			 * OVERLAY focus directly at the focus predicate. */
			bool saw_denied_sandbox_client = false;
			struct wl_client *iter;
			wl_client_for_each(iter, wl_display_get_client_list(server.display)) {
				if (!tp_security_allow_exclusive_overlay(&server, iter))
					saw_denied_sandbox_client = true;
			}
			CHECK(saw_denied_sandbox_client);

			/* Active bind rejection: attempting to bind the hidden
			 * zwlr_layer_shell_v1 global by its numeric name must fail
			 * with a Wayland protocol error (EPROTO). */
			struct zwlr_layer_shell_v1 *bad_bind = wl_registry_bind(
				sb.registry, client.layer_shell_name,
				&zwlr_layer_shell_v1_interface, 1);
			(void)bad_bind;
			for (unsigned int i = 0; i < 10; i++) {
				if (!pump(&sb, &server)) break;
			}
			CHECK(wl_display_get_error(sb.display) == EPROTO);
			wl_display_disconnect(sb.display);
		}
		close(close_pipe[1]);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		unlink(sb_sock);
		rmdir(sb_dir);
	}

	/* ---- 0.1.19: explicit focused window (R1 finding 16) ---- */
	/* Restore default bindings via reload (emergency-quit test cleared them). */
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("-- default bindings\n", lf);
		fclose(lf);
	}
	CHECK(tp_lua_reload_file(&server.lua, lua_cfg, &server.settings, NULL, 0));

	struct test_window fw1 = {0}, fw2 = {0};
	configure_window_app(&client, &fw1, "fw1", "focus-win-1");
	for (unsigned int i = 0; i < 200 && !fw1.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_client *fc1 = tp_focused(&server);
	CHECK(fc1 != NULL);

	configure_window_app(&client, &fw2, "fw2", "focus-win-2");
	for (unsigned int i = 0; i < 200 && !fw2.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 15; i++)
		if (!pump(&client, &server)) goto fail;
	struct tp_client *fc2 = tp_focused(&server);
	CHECK(fc2 != NULL && fc2 != fc1);

	/* Focus fw1 explicitly so fpop belongs to the focused window. */
	tp_focus(&server, fc1);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(tp_focused(&server) == fc1);

	/* Open an xdg popup on fw1 and give the seat's keyboard focus to the
	 * popup surface, simulating a popup grab. */
	struct test_popup fpop = { .client = &client };
	struct xdg_positioner *fpos = xdg_wm_base_create_positioner(client.wm_base);
	xdg_positioner_set_size(fpos, 80, 60);
	xdg_positioner_set_anchor_rect(fpos, 10, 10, 20, 20);
	xdg_positioner_set_anchor(fpos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
	xdg_positioner_set_gravity(fpos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	fpop.surface = wl_compositor_create_surface(client.compositor);
	fpop.xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base, fpop.surface);
	xdg_surface_add_listener(fpop.xdg_surface, &popup_surface_listener, &fpop);
	fpop.popup = xdg_surface_get_popup(fpop.xdg_surface, fw1.xdg_surface, fpos);
	xdg_popup_add_listener(fpop.popup, &popup_listener, &fpop);
	xdg_positioner_destroy(fpos);
	wl_surface_commit(fpop.surface);
	for (unsigned int i = 0; i < 200 && !fpop.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(fpop.configured);
	CHECK(!wl_list_empty(&fc1->toplevel->base->popups));
	if (!wl_list_empty(&fc1->toplevel->base->popups)) {
		struct wlr_xdg_popup *wpop =
			wl_container_of(fc1->toplevel->base->popups.next, wpop, link);
		wlr_seat_keyboard_notify_enter(server.seat, wpop->base->surface,
									   NULL, 0, NULL);
		for (unsigned int i = 0; i < 5; i++)
			if (!pump(&client, &server)) goto fail;

		/* Explicit focus must still report fc1, and Meta+V must still float it. */
		CHECK(tp_focused(&server) == fc1);
		bool was_floating = fc1->floating;
		chord(&keyboard, KEY_V, false, false);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(fc1->floating == !was_floating);

		/* Unmapping fc1 while the popup holds seat keyboard focus must still
		 * recognize had_focus and restore focus to fc2. */
		xdg_popup_destroy(fpop.popup);
		xdg_surface_destroy(fpop.xdg_surface);
		wl_surface_destroy(fpop.surface);
		destroy_window(&fw1);
		for (unsigned int i = 0; i < 20; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_focused(&server) == fc2);
	} else {
		xdg_popup_destroy(fpop.popup);
		xdg_surface_destroy(fpop.xdg_surface);
		wl_surface_destroy(fpop.surface);
		destroy_window(&fw1);
	}
	destroy_window(&fw2);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;

	/* ---- 0.2.0: P3 shell protocols and binding gate ---- */
	CHECK(client.unclassified_globals == 0);
	CHECK(tp_security_classify_global("zwp_idle_inhibit_manager_v1") ==
		  TP_GLOBAL_UNPRIVILEGED);
	CHECK(client.has_wlr_foreign_toplevel);
	CHECK(client.has_ext_foreign_toplevel);
	CHECK(client.has_ext_workspace);
	CHECK(!client.has_ext_session_lock); /* hidden from non-locker */
	CHECK(client.has_idle_notify);
	CHECK(client.has_idle_inhibit);
	CHECK(client.has_output_power);

	/* 1) Foreign toplevel (zwlr_foreign_toplevel_manager_v1 + ext_foreign_toplevel_list_v1). */
	tp_monitor_focus(&server, mon_primary);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	client.ft_wlr_count = 0;
	client.ft_ext_count = 0;
	struct test_window ft1 = {0};
	configure_window_app(&client, &ft1, "ft-app", "ft-title-1");
	for (unsigned int i = 0; i < 200 && !ft1.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(client.ft_wlr_count == 1 && client.ft_ext_count == 1);
	if (client.ft_wlr_count == 1 && client.ft_ext_count == 1) {
		struct test_ft_wlr *fw = &client.ft_wlr[0];
		struct test_ft_ext *fe = &client.ft_ext[0];
		CHECK(strcmp(fw->app_id, "ft-app") == 0);
		CHECK(strcmp(fw->title, "ft-title-1") == 0);
		CHECK(fw->output_entered);
		CHECK(fw->activated);
		CHECK(!fw->fullscreen);
		CHECK(strcmp(fe->app_id, "ft-app") == 0);
		CHECK(strcmp(fe->title, "ft-title-1") == 0);

		/* Live title and app_id updates propagate to both protocols. */
		xdg_toplevel_set_title(ft1.toplevel, "ft-title-2");
		xdg_toplevel_set_app_id(ft1.toplevel, "ft-app-2");
		wl_surface_commit(ft1.surface);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(strcmp(fw->title, "ft-title-2") == 0);
		CHECK(strcmp(fw->app_id, "ft-app-2") == 0);
		CHECK(strcmp(fe->title, "ft-title-2") == 0);
		CHECK(strcmp(fe->app_id, "ft-app-2") == 0);

		/* Fullscreen request via zwlr foreign toplevel. */
		struct tp_client *c_ft = tp_focused(&server);
		CHECK(c_ft != NULL && !c_ft->fullscreen);
		zwlr_foreign_toplevel_handle_v1_set_fullscreen(fw->handle, NULL);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(c_ft && c_ft->fullscreen && fw->fullscreen);
		zwlr_foreign_toplevel_handle_v1_unset_fullscreen(fw->handle);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(c_ft && !c_ft->fullscreen && !fw->fullscreen);

		/* Switch to tag 2, then activate ft1 via foreign toplevel: must switch
		 * back to tag 1 and focus ft1. */
		chord(&keyboard, KEY_2, false, false);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&mon_primary->view) == tp_tag_mask(2));
		zwlr_foreign_toplevel_handle_v1_activate(fw->handle, client.seat);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&mon_primary->view) == tp_tag_mask(1));
		CHECK(tp_focused(&server) == c_ft);

		/* Close request via foreign toplevel. */
		zwlr_foreign_toplevel_handle_v1_close(fw->handle);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(ft1.closed);
	}
	destroy_window(&ft1);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	if (client.ft_wlr_count == 1 && client.ft_ext_count == 1) {
		CHECK(client.ft_wlr[0].closed);
		CHECK(client.ft_ext[0].closed);
	}

	/* 2) ext-workspace: 9 tags per monitor, activate and deactivate. */
	struct test_ws_item *ws1 = NULL, *ws2 = NULL;
	unsigned int live_ws = 0;
	char mon_ws1[96], mon_ws2[96];
	snprintf(mon_ws1, sizeof(mon_ws1), "%s:1", mon_primary->output->name);
	snprintf(mon_ws2, sizeof(mon_ws2), "%s:2", mon_primary->output->name);
	for (size_t i = 0; i < client.ws_count; i++) {
		if (client.ws_items[i].removed)
			continue;
		live_ws++;
		if (strcmp(client.ws_items[i].id, mon_ws1) == 0)
			ws1 = &client.ws_items[i];
		else if (strcmp(client.ws_items[i].id, mon_ws2) == 0)
			ws2 = &client.ws_items[i];
	}
	unsigned int mon_count = (unsigned int)wl_list_length(&server.monitors);
	CHECK(client.ws_group_count >= mon_count);
	CHECK(live_ws == mon_count * TP_TAG_COUNT);
	CHECK(ws1 != NULL && ws2 != NULL);
	if (ws1 && ws2) {
		CHECK((ws1->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0);
		CHECK((ws2->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) == 0);

		ext_workspace_handle_v1_activate(ws2->handle);
		ext_workspace_manager_v1_commit(client.ws_mgr);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&mon_primary->view) == tp_tag_mask(2));
		CHECK((ws2->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0);
		CHECK((ws1->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) == 0);

		/* Combined view 1+2, then deactivate 2 -> returns to tag 1. */
		chord(&keyboard, KEY_1, false, true);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK((ws1->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0);
		CHECK((ws2->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0);

		ext_workspace_handle_v1_deactivate(ws2->handle);
		ext_workspace_manager_v1_commit(client.ws_mgr);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&mon_primary->view) == tp_tag_mask(1));
		CHECK((ws1->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0);
		CHECK((ws2->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) == 0);

		/* Deactivating the last active tag is refused. */
		ext_workspace_handle_v1_deactivate(ws1->handle);
		ext_workspace_manager_v1_commit(client.ws_mgr);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&mon_primary->view) == tp_tag_mask(1));
		CHECK((ws1->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0);
	}

	/* 3) Session lock, per-role grant, binding gate, and fail-locked crash. */
	lf = fopen(lua_cfg, "wb");
	CHECK(lf != NULL);
	if (lf) {
		fputs("tp.bind('Meta+n', function() tp.spawn('unlocked-only') end)\n"
			  "tp.bind('Meta+k', function() tp.spawn('locked-ok') end, { locked = true })\n",
			  lf);
		fclose(lf);
	}
	CHECK(tp_lua_reload_file(&server.lua, lua_cfg, &server.settings, NULL, 0));

	/* Create an authorized locker client via socketpair + wl_client_create. */
	int lsv[2] = {-1, -1};
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lsv) == 0);
	struct wl_client *srv_locker = wl_client_create(server.display, lsv[0]);
	CHECK(srv_locker != NULL);
	tp_security_set_locker_client(&server, srv_locker);

	struct test_client lk1 = { .shm_fd = -1 };
	lk1.display = wl_display_connect_to_fd(lsv[1]);
	CHECK(lk1.display != NULL);
	if (lk1.display) {
		lk1.registry = wl_display_get_registry(lk1.display);
		wl_registry_add_listener(lk1.registry, &registry_listener, &lk1);
		for (unsigned int i = 0; i < 20; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(lk1.has_ext_session_lock && lk1.lock_mgr != NULL);
		CHECK(lk1.lock_mgr_name != 0 && lk1.output != NULL);
		CHECK(create_shm_buffer(&lk1) == 0);

		/* Direct same-UID non-locker client attempting to bind
		 * ext_session_lock_manager_v1 by numeric name gets EPROTO. */
		struct test_client rogue = { .shm_fd = -1 };
		rogue.display = wl_display_connect(server.socket);
		CHECK(rogue.display != NULL);
		if (rogue.display) {
			rogue.registry = wl_display_get_registry(rogue.display);
			wl_registry_add_listener(rogue.registry, &registry_listener, &rogue);
			for (unsigned int i = 0; i < 15; i++)
				if (!pump(&rogue, &server)) break;
			CHECK(!rogue.has_ext_session_lock);
			struct ext_session_lock_manager_v1 *bad_lock = wl_registry_bind(
				rogue.registry, lk1.lock_mgr_name,
				&ext_session_lock_manager_v1_interface, 1);
			(void)bad_lock;
			for (unsigned int i = 0; i < 10; i++)
				if (!pump(&rogue, &server)) break;
			CHECK(wl_display_get_error(rogue.display) == EPROTO);
			wl_display_disconnect(rogue.display);
		}

		const char pending_lock[] =
			"tp.bind('F9', function() tp.spawn('held-across-lock') end,"
			" { release = true, locked = true })";
		CHECK(tp_lua_exec_string(server.lua, pending_lock, sizeof(pending_lock) - 1,
								 "pending-lock", 100000, 100, &server.settings,
								 &server.settings, NULL, 0));
		send_key(&keyboard, KEY_F9, WL_KEYBOARD_KEY_STATE_PRESSED, 390);
		CHECK(key_consumed(&server, KEY_F9));

		/* Lock the session and map a lock surface. */
		struct test_lock_state ls1 = {0};
		struct ext_session_lock_v1 *lock1 =
			ext_session_lock_manager_v1_lock(lk1.lock_mgr);
		ext_session_lock_v1_add_listener(lock1, &lock_listener, &ls1);
		for (unsigned int i = 0; i < 20 && !ls1.locked; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(ls1.locked && !ls1.finished);
		CHECK(server.locked);
		CHECK(!server.layer[TP_LAYER_TILE]->node.enabled);

		struct wl_surface *ls_wl = wl_compositor_create_surface(lk1.compositor);
		struct test_lock_surf_state lss = { .client = &lk1, .surface = ls_wl };
		struct ext_session_lock_surface_v1 *lsurf =
			ext_session_lock_v1_get_lock_surface(lock1, ls_wl, lk1.output);
		ext_session_lock_surface_v1_add_listener(lsurf, &lock_surf_listener, &lss);
		for (unsigned int i = 0; i < 30 && !lss.configured; i++)
			if (!pump(&lk1, &server)) break;
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(lss.configured && lss.width == 1280 && lss.height == 720);
		CHECK(lk1.keyboard_focus == ls_wl);

		hook_event_count = 0;
		lk1.key_events = 0;
		send_key(&keyboard, KEY_F9, WL_KEYBOARD_KEY_STATE_RELEASED, 391);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(hook_event_count == 0 && lk1.key_events == 0);
		CHECK(!key_consumed(&server, KEY_F9));

		/* While locked: normal Meta+n is gated and passes to locker;
		 * Meta+k ({ locked = true }) runs; emergency quit is gated;
		 * VT switch still works. */
		hook_event_count = 0;
		lk1.key_events = 0;
		chord(&keyboard, KEY_N, false, false);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(!has_hook_event("unlocked-only"));
		CHECK(lk1.key_events > 0);

		hook_event_count = 0;
		chord(&keyboard, KEY_K, false, false);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(has_hook_event("locked-ok"));

		server.quit_requested = false;
		send_key(&keyboard, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED, 400);
		send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 401);
		send_key(&keyboard, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_PRESSED, 402);
		send_key(&keyboard, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_PRESSED, 403);
		CHECK(!server.quit_requested);
		send_key(&keyboard, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_RELEASED, 404);
		send_key(&keyboard, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_RELEASED, 405);
		last_vt = 0;
		server.change_vt = mock_change_vt;
		send_key(&keyboard, KEY_F2, WL_KEYBOARD_KEY_STATE_PRESSED, 406);
		CHECK(last_vt == 2);
		send_key(&keyboard, KEY_F2, WL_KEYBOARD_KEY_STATE_RELEASED, 407);
		send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 408);
		send_key(&keyboard, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED, 409);

		send_key(&keyboard, KEY_F9, WL_KEYBOARD_KEY_STATE_PRESSED, 410);
		CHECK(key_consumed(&server, KEY_F9));
		/* Unlock restores layers and normal bindings. */
		ext_session_lock_surface_v1_destroy(lsurf);
		wl_surface_destroy(ls_wl);
		ext_session_lock_v1_unlock_and_destroy(lock1);
		for (unsigned int i = 0; i < 20; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(!server.locked);
		CHECK(server.layer[TP_LAYER_TILE]->node.enabled);

		hook_event_count = 0;
		send_key(&keyboard, KEY_F9, WL_KEYBOARD_KEY_STATE_RELEASED, 411);
		CHECK(hook_event_count == 0 && !key_consumed(&server, KEY_F9));

		hook_event_count = 0;
		chord(&keyboard, KEY_N, false, false);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(has_hook_event("unlocked-only"));

		/* Fail-locked on locker crash: lock again, then disconnect without
		 * unlock_and_destroy. Session must stay locked until a new locker
		 * takes over and unlocks. */
		struct test_lock_state ls2 = {0};
		struct ext_session_lock_v1 *lock2 =
			ext_session_lock_manager_v1_lock(lk1.lock_mgr);
		ext_session_lock_v1_add_listener(lock2, &lock_listener, &ls2);
		for (unsigned int i = 0; i < 20 && !ls2.locked; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(ls2.locked && server.locked);

		if (lk1.shm_data && lk1.shm_data != MAP_FAILED)
			munmap(lk1.shm_data, 16);
		if (lk1.shm_fd >= 0) close(lk1.shm_fd);
		wl_display_disconnect(lk1.display);
		for (unsigned int i = 0; i < 20; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(server.locked);
		CHECK(server.active_lock == NULL);
		CHECK(!server.layer[TP_LAYER_TILE]->node.enabled);

		/* Normal binding is still gated while in crash-locked state. */
		hook_event_count = 0;
		chord(&keyboard, KEY_N, false, false);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(!has_hook_event("unlocked-only"));

		/* Recovery locker connects, locks, and unlocks cleanly. */
		int lsv2[2] = {-1, -1};
		CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lsv2) == 0);
		struct wl_client *srv_locker2 = wl_client_create(server.display, lsv2[0]);
		CHECK(srv_locker2 != NULL);
		tp_security_set_locker_client(&server, srv_locker2);
		struct test_client lk2 = { .shm_fd = -1 };
		lk2.display = wl_display_connect_to_fd(lsv2[1]);
		CHECK(lk2.display != NULL);
		if (lk2.display) {
			lk2.registry = wl_display_get_registry(lk2.display);
			wl_registry_add_listener(lk2.registry, &registry_listener, &lk2);
			for (unsigned int i = 0; i < 20; i++)
				if (!pump(&lk2, &server)) break;
			CHECK(lk2.lock_mgr != NULL);
			struct test_lock_state ls3 = {0};
			struct ext_session_lock_v1 *lock3 =
				ext_session_lock_manager_v1_lock(lk2.lock_mgr);
			ext_session_lock_v1_add_listener(lock3, &lock_listener, &ls3);
			for (unsigned int i = 0; i < 20 && !ls3.locked; i++)
				if (!pump(&lk2, &server)) break;
			CHECK(ls3.locked && server.locked);
			ext_session_lock_v1_unlock_and_destroy(lock3);
			for (unsigned int i = 0; i < 20; i++)
				if (!pump(&lk2, &server)) break;
			CHECK(!server.locked);
			CHECK(server.layer[TP_LAYER_TILE]->node.enabled);
			wl_display_disconnect(lk2.display);
		}
	}

	/* 4) Idle notify & visibility-gated idle inhibit. */
	if (client.idle_notifier && client.idle_inhibit_mgr && client.seat) {
		struct test_idle_state idst = {0};
		struct ext_idle_notification_v1 *notif =
			ext_idle_notifier_v1_get_idle_notification(
				client.idle_notifier, 40, client.seat);
		ext_idle_notification_v1_add_listener(notif, &idle_notif_listener, &idst);
		for (unsigned int i = 0; i < 40 && !idst.idled; i++) {
			usleep(10000);
			if (!pump(&client, &server)) goto fail;
		}
		CHECK(idst.idled);

		/* Keyboard activity resumes idle. */
		send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED, 450);
		send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED, 451);
		for (unsigned int i = 0; i < 15 && !idst.resumed; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(idst.resumed);

		/* Visible window with idle inhibitor prevents idled. */
		struct test_window iw = {0};
		configure_window_app(&client, &iw, "idle-app", "IdleApp");
		for (unsigned int i = 0; i < 200 && !iw.configured; i++)
			if (!pump(&client, &server)) goto fail;
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;

		struct zwp_idle_inhibitor_v1 *inh =
			zwp_idle_inhibit_manager_v1_create_inhibitor(
				client.idle_inhibit_mgr, iw.surface);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		send_key(&keyboard, KEY_B, WL_KEYBOARD_KEY_STATE_PRESSED, 452);
		send_key(&keyboard, KEY_B, WL_KEYBOARD_KEY_STATE_RELEASED, 453);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		idst.idled = false;
		for (unsigned int i = 0; i < 10; i++) {
			usleep(10000);
			if (!pump(&client, &server)) goto fail;
		}
		CHECK(!idst.idled);

		/* Switching to tag 2 hides iw, so idle inhibition lifts and idled fires. */
		chord(&keyboard, KEY_2, false, false);
		for (unsigned int i = 0; i < 40 && !idst.idled; i++) {
			usleep(10000);
			if (!pump(&client, &server)) goto fail;
		}
		CHECK(idst.idled);

		zwp_idle_inhibitor_v1_destroy(inh);
		ext_idle_notification_v1_destroy(notif);
		destroy_window(&iw);
		chord(&keyboard, KEY_1, false, false);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
	}

	/* 5) Output power management (DPMS off/on). */
	if (client.output_power_mgr && client.output) {
		struct test_power_state pst = {0};
		struct zwlr_output_power_v1 *op =
			zwlr_output_power_manager_v1_get_output_power(
				client.output_power_mgr, client.output);
		zwlr_output_power_v1_add_listener(op, &power_listener, &pst);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(pst.got_mode && pst.mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);

		zwlr_output_power_v1_set_mode(op, ZWLR_OUTPUT_POWER_V1_MODE_OFF);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(pst.mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF);
		struct tp_monitor *pm;
		bool any_disabled = false;
		wl_list_for_each(pm, &server.monitors, link) {
			if (!pm->output->enabled)
				any_disabled = true;
		}
		CHECK(any_disabled);

		zwlr_output_power_v1_set_mode(op, ZWLR_OUTPUT_POWER_V1_MODE_ON);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(pst.mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
		bool all_enabled = true;
		wl_list_for_each(pm, &server.monitors, link) {
			if (!pm->output->enabled)
				all_enabled = false;
		}
		CHECK(all_enabled);

		zwlr_output_power_v1_destroy(op);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
	}

	/* ---- 0.2.1: screens and capture ---- */
	CHECK(client.unclassified_globals == 0);
	CHECK(tp_security_classify_global("wp_cursor_shape_manager_v1") ==
		  TP_GLOBAL_UNPRIVILEGED);
	CHECK(tp_security_classify_global("zxdg_output_manager_v1") ==
		  TP_GLOBAL_UNPRIVILEGED);
	CHECK(client.has_output_mgmt);
	CHECK(client.has_gamma_control);
	CHECK(client.has_cursor_shape);
	CHECK(client.has_xdg_output);
	/* Ordinary direct same-UID client must NOT see raw-capture globals. */
	CHECK(!client.has_screencopy);
	CHECK(!client.has_image_copy);
	CHECK(!client.has_output_capture_src);
	CHECK(!client.has_toplevel_capture_src);

	/* 1) Authorized capture client via socketpair + tp_security_add_capture_client:
	 *    exercises screencopy, ext-image-copy-capture (output + toplevel),
	 *    unauthorized bind rejection, and automatic revocation on disconnect. */
	{
		struct test_window cw = {0};
		configure_window_app(&client, &cw, "cap-app", "Capture Target");
		for (unsigned int i = 0; i < 200 && !cw.configured; i++)
			if (!pump(&client, &server)) goto fail;
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;

		int csv[2] = {-1, -1};
		CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, csv) == 0);
		struct wl_client *srv_cap = wl_client_create(server.display, csv[0]);
		CHECK(srv_cap != NULL);
		CHECK(tp_security_add_capture_client(&server, srv_cap));
		CHECK(tp_security_allow_capture(&server, srv_cap));

		struct test_client cap = { .shm_fd = -1 };
		cap.display = wl_display_connect_to_fd(csv[1]);
		CHECK(cap.display != NULL);
		if (cap.display) {
			cap.registry = wl_display_get_registry(cap.display);
			wl_registry_add_listener(cap.registry, &registry_listener, &cap);
			for (unsigned int i = 0; i < 20; i++)
				if (!pump(&cap, &server)) break;
			CHECK(cap.has_screencopy && cap.screencopy_mgr != NULL);
			CHECK(cap.has_image_copy && cap.image_copy_mgr != NULL);
			CHECK(cap.has_output_capture_src && cap.output_src_mgr != NULL);
			CHECK(cap.has_toplevel_capture_src && cap.toplevel_src_mgr != NULL);
			CHECK(cap.screencopy_mgr_name != 0 && cap.output != NULL);

			/* Unauthorized direct client attempting to bind screencopy gets EPROTO. */
			struct test_client rogue_cap = { .shm_fd = -1 };
			rogue_cap.display = wl_display_connect(server.socket);
			CHECK(rogue_cap.display != NULL);
			if (rogue_cap.display) {
				rogue_cap.registry = wl_display_get_registry(rogue_cap.display);
				wl_registry_add_listener(rogue_cap.registry, &registry_listener,
										 &rogue_cap);
				for (unsigned int i = 0; i < 15; i++)
					if (!pump(&rogue_cap, &server)) break;
				CHECK(!rogue_cap.has_screencopy);
				struct zwlr_screencopy_manager_v1 *bad_sc = wl_registry_bind(
					rogue_cap.registry, cap.screencopy_mgr_name,
					&zwlr_screencopy_manager_v1_interface, 1);
				(void)bad_sc;
				for (unsigned int i = 0; i < 10; i++)
					if (!pump(&rogue_cap, &server)) break;
				CHECK(wl_display_get_error(rogue_cap.display) == EPROTO);
				wl_display_disconnect(rogue_cap.display);
			}

			/* 1a) zwlr_screencopy_manager_v1 output capture. */
			struct test_screencopy_state scst = {0};
			struct zwlr_screencopy_frame_v1 *scf =
				zwlr_screencopy_manager_v1_capture_output(
					cap.screencopy_mgr, 0, cap.output);
			zwlr_screencopy_frame_v1_add_listener(scf, &sc_frame_listener, &scst);
			for (unsigned int i = 0; i < 30 && !scst.buffer_done; i++)
				if (!pump(&cap, &server)) break;
			CHECK(scst.got_buffer && scst.buffer_done && scst.width > 0 &&
				  scst.height > 0);
			if (scst.buffer_done) {
				struct wl_buffer *scbuf = create_sized_buffer_fmt(
					&cap, (int32_t)scst.width, (int32_t)scst.height,
					scst.format);
				CHECK(scbuf != NULL);
				if (scbuf) {
					zwlr_screencopy_frame_v1_copy(scf, scbuf);
					struct tp_monitor *pm;
					wl_list_for_each(pm, &server.monitors, link)
						wlr_output_schedule_frame(pm->output);
					for (unsigned int i = 0; i < 40 && !scst.ready && !scst.failed; i++) {
						usleep(5000);
						if (!pump(&cap, &server)) break;
					}
					CHECK(scst.ready && !scst.failed);
					wl_buffer_destroy(scbuf);
				}
			}
			zwlr_screencopy_frame_v1_destroy(scf);

			/* 1b) ext-image-copy-capture output capture. */
			struct ext_image_capture_source_v1 *out_src =
				ext_output_image_capture_source_manager_v1_create_source(
					cap.output_src_mgr, cap.output);
			struct ext_image_copy_capture_session_v1 *out_sess =
				ext_image_copy_capture_manager_v1_create_session(
					cap.image_copy_mgr, out_src, 0);
			struct test_icc_session_state osst = {0};
			ext_image_copy_capture_session_v1_add_listener(
				out_sess, &icc_session_listener, &osst);
			for (unsigned int i = 0; i < 30 && !osst.done; i++)
				if (!pump(&cap, &server)) break;
			CHECK(osst.done && !osst.stopped && osst.width > 0 &&
				  osst.height > 0 && osst.has_shm_format);
			if (osst.done && osst.width > 0 && osst.height > 0) {
				struct wl_buffer *obuf = create_sized_buffer_fmt(
					&cap, (int32_t)osst.width, (int32_t)osst.height,
					osst.shm_format);
				CHECK(obuf != NULL);
				if (obuf) {
					struct ext_image_copy_capture_frame_v1 *of =
						ext_image_copy_capture_session_v1_create_frame(out_sess);
					struct test_icc_frame_state ofst = {0};
					ext_image_copy_capture_frame_v1_add_listener(
						of, &icc_frame_listener, &ofst);
					ext_image_copy_capture_frame_v1_attach_buffer(of, obuf);
					ext_image_copy_capture_frame_v1_capture(of);
					struct tp_monitor *pm;
					wl_list_for_each(pm, &server.monitors, link)
						wlr_output_schedule_frame(pm->output);
					for (unsigned int i = 0; i < 40 && !ofst.ready && !ofst.failed; i++) {
						usleep(5000);
						if (!pump(&cap, &server)) break;
					}
					CHECK(ofst.ready && !ofst.failed);
					ext_image_copy_capture_frame_v1_destroy(of);
					wl_buffer_destroy(obuf);
				}
			}
			ext_image_copy_capture_session_v1_destroy(out_sess);
			ext_image_capture_source_v1_destroy(out_src);

			/* 1c) ext-image-copy-capture foreign toplevel capture. */
			struct ext_foreign_toplevel_handle_v1 *cap_ft = NULL;
			for (size_t i = 0; i < cap.ft_ext_count; i++) {
				if (!cap.ft_ext[i].closed &&
					strcmp(cap.ft_ext[i].app_id, "cap-app") == 0) {
					cap_ft = cap.ft_ext[i].handle;
					break;
				}
			}
			CHECK(cap_ft != NULL);
			if (cap_ft) {
				struct ext_image_capture_source_v1 *tl_src =
					ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
						cap.toplevel_src_mgr, cap_ft);
				struct ext_image_copy_capture_session_v1 *tl_sess =
					ext_image_copy_capture_manager_v1_create_session(
						cap.image_copy_mgr, tl_src, 0);
				struct test_icc_session_state tsst = {0};
				ext_image_copy_capture_session_v1_add_listener(
					tl_sess, &icc_session_listener, &tsst);
				for (unsigned int i = 0; i < 30 && !tsst.done && !tsst.stopped; i++)
					if (!pump(&cap, &server)) break;
				CHECK(tsst.done && !tsst.stopped && tsst.width > 0 &&
					  tsst.height > 0);
				if (tsst.done && tsst.width > 0 && tsst.height > 0) {
					struct wl_buffer *tbuf = create_sized_buffer_fmt(
						&cap, (int32_t)tsst.width, (int32_t)tsst.height,
						tsst.has_shm_format ? tsst.shm_format
											: WL_SHM_FORMAT_ARGB8888);
					CHECK(tbuf != NULL);
					if (tbuf) {
						struct ext_image_copy_capture_frame_v1 *tf =
							ext_image_copy_capture_session_v1_create_frame(tl_sess);
						struct test_icc_frame_state tfst = {0};
						ext_image_copy_capture_frame_v1_add_listener(
							tf, &icc_frame_listener, &tfst);
						ext_image_copy_capture_frame_v1_attach_buffer(tf, tbuf);
						ext_image_copy_capture_frame_v1_capture(tf);
						for (unsigned int i = 0; i < 40 && !tfst.ready && !tfst.failed; i++) {
							usleep(5000);
							if (!pump(&cap, &server)) break;
						}
						CHECK(tfst.ready && !tfst.failed);
						ext_image_copy_capture_frame_v1_destroy(tf);
						wl_buffer_destroy(tbuf);
					}
				}
				ext_image_copy_capture_session_v1_destroy(tl_sess);
				ext_image_capture_source_v1_destroy(tl_src);
			}

			/* Unfocused client cursor-shape request must be ignored. */
			if (cap.cursor_shape_mgr && cap.pointer) {
				struct wp_cursor_shape_device_v1 *cdev =
					wp_cursor_shape_manager_v1_get_pointer(
						cap.cursor_shape_mgr, cap.pointer);
				/* First focus client on cw (2x2 surface at c->geom.x,y) and set
				 * pointer shape from client. */
				struct tp_client *fc = tp_focused(&server);
				CHECK(fc != NULL);
				if (fc) {
					wlr_cursor_warp(server.cursor, NULL,
									fc->geom.x + 1.0, fc->geom.y + 1.0);
					tp_pointer_refocus_now(&server);
				}
				for (unsigned int i = 0; i < 15; i++)
					if (!pump(&client, &server)) goto fail;

				struct wp_cursor_shape_device_v1 *fdev =
					wp_cursor_shape_manager_v1_get_pointer(
						client.cursor_shape_mgr, client.pointer);
				wp_cursor_shape_device_v1_set_shape(
					fdev, client.pointer_enter_serial,
					WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
				for (unsigned int i = 0; i < 15; i++)
					if (!pump(&client, &server)) goto fail;
				CHECK(server.cursor_shape_name &&
					  strcmp(server.cursor_shape_name, "pointer") == 0);

				/* Unfocused cap client cannot override cursor shape. */
				wp_cursor_shape_device_v1_set_shape(
					cdev, 0, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
				for (unsigned int i = 0; i < 15; i++)
					if (!pump(&cap, &server)) break;
				CHECK(server.cursor_shape_name &&
					  strcmp(server.cursor_shape_name, "pointer") == 0);

				wp_cursor_shape_device_v1_destroy(fdev);
				wp_cursor_shape_device_v1_destroy(cdev);
			}

			wl_display_disconnect(cap.display);
		}
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(wl_list_empty(&server.capture_clients));

		destroy_window(&cw);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
	}

	/* 2) zwlr_output_manager_v1: enumerate heads, test, apply scale, restore. */
	if (client.output_mgr_name) {
		struct zwlr_output_manager_v1 *om = wl_registry_bind(
			client.registry, client.output_mgr_name,
			&zwlr_output_manager_v1_interface, 4);
		struct test_om_state omst = {0};
		zwlr_output_manager_v1_add_listener(om, &om_mgr_listener, &omst);
		for (unsigned int i = 0; i < 20 && !omst.got_done; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(omst.got_done && omst.head_count >= 1);

		/* Test configuration. */
		struct zwlr_output_configuration_v1 *tcfg =
			zwlr_output_manager_v1_create_configuration(om, omst.serial);
		struct test_om_cfg_state tcst = {0};
		zwlr_output_configuration_v1_add_listener(tcfg, &om_cfg_listener, &tcst);
		for (size_t i = 0; i < omst.head_count; i++) {
			if (omst.heads[i].finished) continue;
			struct zwlr_output_configuration_head_v1 *ch =
				zwlr_output_configuration_v1_enable_head(tcfg, omst.heads[i].head);
			if (i == 0)
				zwlr_output_configuration_head_v1_set_scale(
					ch, wl_fixed_from_double(2.0));
		}
		zwlr_output_configuration_v1_test(tcfg);
		for (unsigned int i = 0; i < 20 && !tcst.succeeded && !tcst.failed &&
									 !tcst.cancelled;
			 i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tcst.succeeded && !tcst.failed && !tcst.cancelled);
		zwlr_output_configuration_v1_destroy(tcfg);

		/* Apply configuration: set head 0 scale to 1.5, verify on server. */
		struct zwlr_output_configuration_v1 *acfg =
			zwlr_output_manager_v1_create_configuration(om, omst.serial);
		struct test_om_cfg_state acst = {0};
		zwlr_output_configuration_v1_add_listener(acfg, &om_cfg_listener, &acst);
		for (size_t i = 0; i < omst.head_count; i++) {
			if (omst.heads[i].finished) continue;
			struct zwlr_output_configuration_head_v1 *ch =
				zwlr_output_configuration_v1_enable_head(acfg, omst.heads[i].head);
			if (i == 0)
				zwlr_output_configuration_head_v1_set_scale(
					ch, wl_fixed_from_double(1.5));
		}
		omst.got_done = false;
		zwlr_output_configuration_v1_apply(acfg);
		for (unsigned int i = 0; i < 25 && (!acst.succeeded || !omst.got_done); i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(acst.succeeded && !acst.failed && !acst.cancelled);
		struct tp_monitor *m0 = tp_monitor_by_name(&server, omst.heads[0].name);
		CHECK(m0 != NULL && m0->output->scale > 1.49f &&
			  m0->output->scale < 1.51f);
		zwlr_output_configuration_v1_destroy(acfg);

		/* Restore head 0 scale to 1.0. */
		struct zwlr_output_configuration_v1 *rcfg =
			zwlr_output_manager_v1_create_configuration(om, omst.serial);
		struct test_om_cfg_state rcst = {0};
		zwlr_output_configuration_v1_add_listener(rcfg, &om_cfg_listener, &rcst);
		for (size_t i = 0; i < omst.head_count; i++) {
			if (omst.heads[i].finished) continue;
			struct zwlr_output_configuration_head_v1 *ch =
				zwlr_output_configuration_v1_enable_head(rcfg, omst.heads[i].head);
			if (i == 0)
				zwlr_output_configuration_head_v1_set_scale(
					ch, wl_fixed_from_double(1.0));
		}
		zwlr_output_configuration_v1_apply(rcfg);
		for (unsigned int i = 0; i < 20 && !rcst.succeeded && !rcst.failed; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(rcst.succeeded);
		zwlr_output_configuration_v1_destroy(rcfg);
		zwlr_output_manager_v1_stop(om);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
	}

	/* 3) zwlr_gamma_control_manager_v1: headless output responds cleanly. */
	if (client.gamma_mgr && client.output) {
		struct test_gamma_state gst = {0};
		struct zwlr_gamma_control_v1 *gc =
			zwlr_gamma_control_manager_v1_get_gamma_control(client.gamma_mgr,
															client.output);
		zwlr_gamma_control_v1_add_listener(gc, &gamma_listener, &gst);
		for (unsigned int i = 0; i < 20 && !gst.got_size && !gst.failed; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(gst.got_size || gst.failed);
		zwlr_gamma_control_v1_destroy(gc);
	}

	/* 4) zxdg_output_manager_v1: logical geometry and output name. */
	if (client.xdg_output_mgr && client.output) {
		struct test_xdg_out_state xost = {0};
		struct zxdg_output_v1 *xo =
			zxdg_output_manager_v1_get_xdg_output(client.xdg_output_mgr,
												  client.output);
		zxdg_output_v1_add_listener(xo, &xdg_out_listener, &xost);
		for (unsigned int i = 0; i < 20 && !(xost.w > 0 && xost.name[0]); i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(xost.w > 0 && xost.h > 0 && xost.name[0] != '\0');
		zxdg_output_v1_destroy(xo);
	}

	/* ---- 0.2.2: text-input, input-method, xdg-activation, primary-selection,
	 *             data-control, xdg-decoration ---- */
	CHECK(client.unclassified_globals == 0);
	CHECK(client.has_text_input && client.text_input_mgr != NULL);
	CHECK(client.has_input_method && client.input_method_mgr != NULL);
	CHECK(client.has_xdg_activation && client.xdg_activation != NULL);
	CHECK(client.has_primary_selection && client.primary_selection_mgr != NULL);
	CHECK(client.has_wlr_data_control && client.wlr_data_control_mgr != NULL);
	CHECK(client.has_ext_data_control && client.ext_data_control_mgr != NULL);
	CHECK(client.has_xdg_decoration && client.xdg_decoration_mgr != NULL);

	/* 1) xdg-decoration: server-side enforced on tiled windows; floating
	 *    windows may use client-side when requested, and returning to tiled
	 *    restores server-side. Create decoration on a fresh toplevel before
	 *    attaching a buffer (per xdg-decoration-unstable-v1 spec). */
	if (client.xdg_decoration_mgr && client.compositor && client.wm_base) {
		struct test_window dwin = { .client = &client };
		dwin.surface = wl_compositor_create_surface(client.compositor);
		dwin.xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base, dwin.surface);
		xdg_surface_add_listener(dwin.xdg_surface, &surface_listener, &dwin);
		dwin.toplevel = xdg_surface_get_toplevel(dwin.xdg_surface);
		xdg_toplevel_add_listener(dwin.toplevel, &toplevel_listener, &dwin);
		xdg_toplevel_set_app_id(dwin.toplevel, "decor-test");

		struct test_decor_state dst = {0};
		struct zxdg_toplevel_decoration_v1 *dec =
			zxdg_decoration_manager_v1_get_toplevel_decoration(
				client.xdg_decoration_mgr, dwin.toplevel);
		zxdg_toplevel_decoration_v1_add_listener(dec, &decor_listener, &dst);
		zxdg_toplevel_decoration_v1_set_mode(
			dec, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
		wl_surface_commit(dwin.surface);

		for (unsigned int i = 0; i < 20 && (!dwin.configured || dst.configures == 0); i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(dwin.configured && dst.configures > 0);
		CHECK(dst.mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

		/* Pump so the mapped toplevel takes focus. */
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		struct tp_client *dc = tp_focused(&server);
		CHECK(dc != NULL && dc->toplevel && dc->toplevel->app_id &&
			  strcmp(dc->toplevel->app_id, "decor-test") == 0);
		if (dc) {
			tp_client_set_floating(dc, true);
			for (unsigned int i = 0; i < 20 &&
				 dst.mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE; i++)
				if (!pump(&client, &server)) goto fail;
			CHECK(dst.mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
			wl_surface_commit(dwin.surface);
			for (unsigned int i = 0; i < 10; i++)
				if (!pump(&client, &server)) goto fail;

			tp_client_set_floating(dc, false);
			for (unsigned int i = 0; i < 20 &&
				 dst.mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE; i++)
				if (!pump(&client, &server)) goto fail;
			CHECK(dst.mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
		zxdg_toplevel_decoration_v1_destroy(dec);
		xdg_toplevel_destroy(dwin.toplevel);
		xdg_surface_destroy(dwin.xdg_surface);
		wl_surface_destroy(dwin.surface);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
	}

	/* 2) xdg-activation: switches tag and focuses target window; ignored while
	 *    session is locked. */
	if (client.xdg_activation && client.windows[0].surface &&
		client.windows[1].surface) {
		struct tp_client *c0 = NULL, *c1 = NULL, *it;
		wl_list_for_each(it, &server.clients, link) {
			if (it->toplevel && it->toplevel->base &&
				it->toplevel->base->surface) {
				if (!c0) c0 = it;
				else if (!c1) c1 = it;
			}
		}
		CHECK(c0 != NULL && c1 != NULL);
		if (c0 && c1) {
			tp_client_set_tags(c0, tp_tag_mask(1));
			tp_client_set_tags(c1, tp_tag_mask(2));
			tp_tagview_show(&c0->mon->view, tp_tag_mask(1));
			tp_arrange(c0->mon);
			tp_focus(&server, c0);
			CHECK(tp_focused(&server) == c0);

			struct wl_surface *target_surf =
				(c1->toplevel->base->surface->resource &&
				 c1 == tp_focused(&server))
					? client.windows[0].surface
					: (wl_resource_get_id(c1->toplevel->base->surface->resource) ==
					   wl_proxy_get_id((struct wl_proxy *)client.windows[1].surface)
						   ? client.windows[1].surface
						   : client.windows[0].surface);
			struct xdg_activation_token_v1 *tok =
				xdg_activation_v1_get_activation_token(client.xdg_activation);
			struct test_act_token_state ast = {0};
			xdg_activation_token_v1_add_listener(tok, &act_token_listener, &ast);
			xdg_activation_token_v1_commit(tok);
			for (unsigned int i = 0; i < 20 && !ast.done; i++)
				if (!pump(&client, &server)) goto fail;
			CHECK(ast.done && ast.token[0] != '\0');
			xdg_activation_v1_activate(client.xdg_activation, ast.token,
									   target_surf);
			for (unsigned int i = 0; i < 20 && tp_focused(&server) != c1; i++)
				if (!pump(&client, &server)) goto fail;
			CHECK(tp_focused(&server) == c1);
			CHECK(tp_tagview_current(&c1->mon->view) == tp_tag_mask(2));
			xdg_activation_token_v1_destroy(tok);

			/* Restore both windows to tag 1 and focus c0. */
			tp_client_set_tags(c1, tp_tag_mask(1));
			tp_tagview_show(&c0->mon->view, tp_tag_mask(1));
			tp_arrange(c0->mon);
			tp_focus(&server, c0);
		}
	}

	/* 3) data-control (wlr + ext) and primary-selection: setting clipboard and
	 *    primary selection via zwlr_data_control_v1 is observed by
	 *    ext_data_control_v1. */
	if (client.wlr_data_control_mgr && client.ext_data_control_mgr &&
		client.seat) {
		struct test_ext_dc_state dcst = {0};
		struct ext_data_control_device_v1 *ext_dev =
			ext_data_control_manager_v1_get_data_device(
				client.ext_data_control_mgr, client.seat);
		ext_data_control_device_v1_add_listener(ext_dev, &ext_dc_dev_listener,
												&dcst);
		struct zwlr_data_control_device_v1 *wlr_dev =
			zwlr_data_control_manager_v1_get_data_device(
				client.wlr_data_control_mgr, client.seat);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;

		struct zwlr_data_control_source_v1 *src =
			zwlr_data_control_manager_v1_create_data_source(
				client.wlr_data_control_mgr);
		zwlr_data_control_source_v1_add_listener(src, &wlr_dc_src_listener, NULL);
		zwlr_data_control_source_v1_offer(src, "text/plain");
		zwlr_data_control_device_v1_set_selection(wlr_dev, src);
		for (unsigned int i = 0; i < 20 && !dcst.got_sel; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(dcst.got_sel && strcmp(dcst.last_mime, "text/plain") == 0);

		struct zwlr_data_control_source_v1 *psrc =
			zwlr_data_control_manager_v1_create_data_source(
				client.wlr_data_control_mgr);
		zwlr_data_control_source_v1_add_listener(psrc, &wlr_dc_src_listener, NULL);
		zwlr_data_control_source_v1_offer(psrc, "text/plain;charset=utf-8");
		zwlr_data_control_device_v1_set_primary_selection(wlr_dev, psrc);
		for (unsigned int i = 0; i < 20 && !dcst.got_prim; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(dcst.got_prim &&
			  strcmp(dcst.last_mime, "text/plain;charset=utf-8") == 0);

		zwlr_data_control_device_v1_set_selection(wlr_dev, NULL);
		zwlr_data_control_device_v1_set_primary_selection(wlr_dev, NULL);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		if (dcst.sel_offer) ext_data_control_offer_v1_destroy(dcst.sel_offer);
		if (dcst.prim_offer) ext_data_control_offer_v1_destroy(dcst.prim_offer);
		zwlr_data_control_source_v1_destroy(src);
		zwlr_data_control_source_v1_destroy(psrc);
		zwlr_data_control_device_v1_destroy(wlr_dev);
		ext_data_control_device_v1_destroy(ext_dev);
	}

	/* 4) text-input-v3 + input-method-v2 relay + session-lock isolation
	 *    (tam-hide): IME receives activate/surrounding_text, relays preedit
	 *    and commit_string, grabs keys when unlocked, and is deactivated &
	 *    bypassed while locked so keys reach the lock surface instead. */
	if (client.text_input_mgr && client.input_method_mgr && client.seat) {
		struct test_im_state imst = {0};
		struct zwp_input_method_v2 *im =
			zwp_input_method_manager_v2_get_input_method(
				client.input_method_mgr, client.seat);
		zwp_input_method_v2_add_listener(im, &im_listener, &imst);

		struct test_ti_state tist = {0};
		struct zwp_text_input_v3 *ti =
			zwp_text_input_manager_v3_get_text_input(
				client.text_input_mgr, client.seat);
		zwp_text_input_v3_add_listener(ti, &ti_listener, &tist);

		for (unsigned int i = 0; i < 20 && !tist.entered; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tist.entered != NULL);

		zwp_text_input_v3_enable(ti);
		zwp_text_input_v3_set_surrounding_text(ti, "hello", 5, 5);
		zwp_text_input_v3_commit(ti);
		for (unsigned int i = 0; i < 20 && (!imst.active || imst.surrounding[0] == '\0'); i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(imst.active);
		CHECK(strcmp(imst.surrounding, "hello") == 0);

		zwp_input_method_v2_set_preedit_string(im, "nihao", 0, 5);
		zwp_input_method_v2_commit_string(im, "ok");
		zwp_input_method_v2_commit(im, imst.dones);
		for (unsigned int i = 0; i < 20 && tist.commit[0] == '\0'; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(strcmp(tist.preedit, "nihao") == 0);
		CHECK(strcmp(tist.commit, "ok") == 0);

		/* Keyboard grab forwards keys to IME when unlocked. */
		struct test_im_grab_state gst = {0};
		struct zwp_input_method_keyboard_grab_v2 *grab =
			zwp_input_method_v2_grab_keyboard(im);
		zwp_input_method_keyboard_grab_v2_add_listener(
			grab, &im_grab_listener, &gst);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;

		unsigned int client_keys_before = client.key_events;
		send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED, 400);
		send_key(&keyboard, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED, 401);
		for (unsigned int i = 0; i < 15 && gst.key_events < 2; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(gst.key_events >= 2 && gst.last_key == KEY_A);
		CHECK(client.key_events == client_keys_before);

		/* Lock session (tam-hide scenario): IME must deactivate, text-input
		 * must leave, and keys must bypass IME grab and reach the lock surface. */
		int lsv[2] = {-1, -1};
		CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lsv) == 0);
		struct wl_client *lcl = wl_client_create(server.display, lsv[0]);
		CHECK(lcl != NULL);
		tp_security_set_locker_client(&server, lcl);
		struct test_client lc = { .shm_fd = -1 };
		lc.display = wl_display_connect_to_fd(lsv[1]);
		CHECK(lc.display != NULL);
		if (lc.display) {
			lc.registry = wl_display_get_registry(lc.display);
			wl_registry_add_listener(lc.registry, &registry_listener, &lc);
			for (unsigned int i = 0; i < 20 && (!lc.lock_mgr || !lc.compositor || !lc.output || !lc.shm || !lc.keyboard); i++) {
				pump(&lc, &server);
				pump(&client, &server);
			}
			CHECK(lc.lock_mgr != NULL);
			struct test_lock_state lst = {0};
			struct ext_session_lock_v1 *lock =
				ext_session_lock_manager_v1_lock(lc.lock_mgr);
			ext_session_lock_v1_add_listener(lock, &lock_listener, &lst);
			struct wl_surface *lsurf_wl =
				wl_compositor_create_surface(lc.compositor);
			struct test_lock_surf_state lss = {
				.client = &lc, .surface = lsurf_wl
			};
			struct ext_session_lock_surface_v1 *lsurf =
				ext_session_lock_v1_get_lock_surface(lock, lsurf_wl, lc.output);
			ext_session_lock_surface_v1_add_listener(lsurf, &lock_surf_listener, &lss);
			for (unsigned int i = 0; i < 30 && (!lst.locked || !lss.configured ||
				 lc.keyboard_focus != lsurf_wl || imst.active || tist.entered); i++) {
				pump(&lc, &server);
				pump(&client, &server);
			}
			CHECK(server.locked);
			CHECK(lc.keyboard_focus == lsurf_wl);
			CHECK(!imst.active);
			CHECK(tist.entered == NULL);

			/* Key press while locked must go to lock surface, NOT IME grab. */
			unsigned int im_keys_before = gst.key_events;
			unsigned int lc_keys_before = lc.key_events;
			send_key(&keyboard, KEY_B, WL_KEYBOARD_KEY_STATE_PRESSED, 410);
			send_key(&keyboard, KEY_B, WL_KEYBOARD_KEY_STATE_RELEASED, 411);
			for (unsigned int i = 0; i < 15 && lc.key_events == lc_keys_before; i++) {
				pump(&lc, &server);
				pump(&client, &server);
			}
			CHECK(lc.key_events > lc_keys_before);
			CHECK(gst.key_events == im_keys_before);

			/* xdg-activation while locked must be ignored. */
			if (client.xdg_activation && client.windows[0].surface) {
				struct xdg_activation_token_v1 *ltok =
					xdg_activation_v1_get_activation_token(client.xdg_activation);
				struct test_act_token_state last = {0};
				xdg_activation_token_v1_add_listener(ltok, &act_token_listener, &last);
				xdg_activation_token_v1_commit(ltok);
				for (unsigned int i = 0; i < 15 && !last.done; i++)
					pump(&client, &server);
				xdg_activation_v1_activate(client.xdg_activation, last.token,
										   client.windows[0].surface);
				for (unsigned int i = 0; i < 10; i++) {
					pump(&client, &server);
					pump(&lc, &server);
				}
				CHECK(server.locked);
				CHECK(lc.keyboard_focus == lsurf_wl);
				xdg_activation_token_v1_destroy(ltok);
			}

			ext_session_lock_surface_v1_destroy(lsurf);
			wl_surface_destroy(lsurf_wl);
			ext_session_lock_v1_unlock_and_destroy(lock);
			for (unsigned int i = 0; i < 20 && (server.locked || !tist.entered); i++) {
				pump(&lc, &server);
				pump(&client, &server);
			}
			CHECK(!server.locked);
			CHECK(tist.entered != NULL);
			wl_display_disconnect(lc.display);
		}
		tp_security_set_locker_client(&server, NULL);

		zwp_input_method_keyboard_grab_v2_release(grab);
		zwp_text_input_v3_destroy(ti);
		zwp_input_method_v2_destroy(im);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
	}

	/* ---- 0.2.3: IPC socket, queries, dispatch, lock gate, reload, subscribe ---- */
	char ipc_sock_path[256] = {0};
	{
		const char *env_ipc = getenv("TAM_PANE_SOCKET");
		CHECK(env_ipc && *env_ipc);
		CHECK(tp_ipc_socket_path(ipc_sock_path, sizeof(ipc_sock_path),
								 getenv("XDG_RUNTIME_DIR"), server.socket));
		CHECK(env_ipc && strcmp(env_ipc, ipc_sock_path) == 0);

		struct stat st;
		CHECK(stat(ipc_sock_path, &st) == 0);
		CHECK(S_ISSOCK(st.st_mode));
		CHECK((st.st_mode & 0777) == 0600);

		char rep[8192];
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "version\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "OK\nversion\tversion=" TP_VERSION "\tsocket=") !=
			  NULL);
		CHECK(strstr(rep, "\nEND\n") != NULL);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "monitors\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "monitor\tname=HEADLESS-1\t") != NULL);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "windows\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "window\tmonitor=") != NULL);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "focused\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "window\tmonitor=") != NULL);
		CHECK(strstr(rep, "\tfocused=1\t") != NULL);

		struct tp_client *target = tp_focused(&server);
		CHECK(target != NULL);
		if (target) {
			char dispatch[100];
			snprintf(dispatch, sizeof(dispatch),
					 "dispatch send_key_if_focused 0x%llx Ctrl+Shift+v\n",
					 (unsigned long long)target->id);
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									dispatch, rep, sizeof(rep)));
			CHECK(strncmp(rep, "OK\n", 3) == 0);
			snprintf(dispatch, sizeof(dispatch),
					 "dispatch send_key_if_focused 0x%llx Ctrl+v\n",
					 (unsigned long long)(target->id + 1));
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									dispatch, rep, sizeof(rep)));
			CHECK(strstr(rep, "ERR focused window changed") != NULL);
		}

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "tags\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "tag\tmonitor=HEADLESS-1\t") != NULL);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "cursor\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "cursor\tx=") != NULL);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "binds\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "bind\tmods=") != NULL);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"lock /usr/bin/true\n", rep, sizeof(rep)));
		CHECK(strstr(rep, "lock takes no arguments") != NULL);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"lock\n", rep, sizeof(rep)));
		CHECK(strstr(rep, "no locker configured") != NULL);

		snprintf(server.settings.locker, sizeof(server.settings.locker),
				 "/usr/bin/true");
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"lock\n", rep, sizeof(rep)));
		CHECK(strncmp(rep, "OK\n", 3) == 0);
		server.settings.locker[0] = '\0';
		for (unsigned int i = 0; i < 20 && server.locker_client; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(server.locker_client == NULL);

		/* Unknown command & bad dispatch return ERR */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "bogus\n",
								rep, sizeof(rep)));
		CHECK(strncmp(rep, "ERR ", 4) == 0);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch tag view 99\n", rep, sizeof(rep)));
		CHECK(strncmp(rep, "ERR ", 4) == 0);

		/* Subscribe without Lua hooks: must receive tag.change via IPC */
		int sfd = ipc_test_connect(ipc_sock_path);
		CHECK(sfd >= 0);
		if (sfd >= 0) {
			const char *sub_cmd = "subscribe tag.change window.focus\n";
			CHECK(write(sfd, sub_cmd, strlen(sub_cmd)) ==
				  (ssize_t)strlen(sub_cmd));
			char sbuf[4096] = {0};
			size_t slen = 0;
			for (unsigned int i = 0; i < 20 && !strstr(sbuf, "OK\n"); i++) {
				pump(&client, &server);
				ssize_t n = read(sfd, sbuf + slen, sizeof(sbuf) - 1 - slen);
				if (n > 0) {
					slen += (size_t)n;
					sbuf[slen] = '\0';
				}
			}
			CHECK(strncmp(sbuf, "OK\n", 3) == 0);
			CHECK(tp_ipc_has_subscribers(&server, TP_EVENT_TAG_CHANGE));

			/* Focus HEADLESS-1 explicitly, switch to a different tag, and
			 * verify subscriber receives tag.change for HEADLESS-1. */
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"dispatch monitor focus HEADLESS-1\n", rep,
									sizeof(rep)));
			CHECK(strcmp(rep, "OK\nEND\n") == 0);
			tp_tags_t cur_t = tp_tagview_current(&server.selmon->view);
			unsigned int next_tag = (cur_t == tp_tag_mask(2)) ? 3 : 2;
			char dcmd[64], want_ev[128];
			snprintf(dcmd, sizeof(dcmd), "dispatch tag view %u\n", next_tag);
			snprintf(want_ev, sizeof(want_ev),
					 "event\ttag.change\tmonitor=HEADLESS-1\ttags=%u\t",
					 tp_tag_mask(next_tag));
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, dcmd, rep,
									sizeof(rep)));
			CHECK(strcmp(rep, "OK\nEND\n") == 0);

			for (unsigned int i = 0; i < 30 && !strstr(sbuf, want_ev); i++) {
				pump(&client, &server);
				ssize_t n = read(sfd, sbuf + slen, sizeof(sbuf) - 1 - slen);
				if (n > 0) {
					slen += (size_t)n;
					sbuf[slen] = '\0';
				}
			}
			CHECK(strstr(sbuf, want_ev) != NULL);

			/* Switch back to tag 1 */
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"dispatch tag view 1\n", rep, sizeof(rep)));
			CHECK(strcmp(rep, "OK\nEND\n") == 0);
			close(sfd);
			for (unsigned int i = 0; i < 10; i++)
				pump(&client, &server);
			CHECK(!tp_ipc_has_subscribers(&server, TP_EVENT_TAG_CHANGE));
		}

		/* Lock gate: dispatch refused while locked; queries and reload allowed */
		server.locked = true;
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch tag view 3\n", rep, sizeof(rep)));
		CHECK(strstr(rep, "ERR session is locked") != NULL);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "version\n",
								rep, sizeof(rep)));
		CHECK(strncmp(rep, "OK\n", 3) == 0);

		/* Live reload over IPC (allowed while locked): valid then broken */
		FILE *f = fopen(lua_cfg, "w");
		CHECK(f != NULL);
		if (f) {
			fputs("tp.config{ gaps = 14 }\n", f);
			fclose(f);
		}
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "reload\n",
								rep, sizeof(rep)));
		CHECK(strcmp(rep, "OK\nEND\n") == 0);
		CHECK(server.settings.gap_inner == 14);

		f = fopen(lua_cfg, "w");
		CHECK(f != NULL);
		if (f) {
			fputs("tp.config{ gaps = \n", f);
			fclose(f);
		}
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "reload\n",
								rep, sizeof(rep)));
		CHECK(strncmp(rep, "ERR ", 4) == 0);
		CHECK(server.settings.gap_inner == 14);
		server.locked = false;

		/* Oversized request line rejected cleanly */
		char big[TP_IPC_MAX_LINE + 32];
		memset(big, 'x', sizeof(big) - 2);
		big[sizeof(big) - 2] = '\n';
		big[sizeof(big) - 1] = '\0';
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, big, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "ERR ", 4) == 0);
	}

	/* ---- 0.2.4 (R1 finding 15): deferred per-monitor arrange on idle ---- */
	{
		struct tp_monitor *m = server.selmon;
		struct tp_client *fc = tp_focused(&server);
		CHECK(m != NULL && fc != NULL && !fc->floating && !fc->fullscreen);
		if (m && fc && !fc->floating && !fc->fullscreen) {
			/* 1) Back-to-back tp_arrange(m) calls coalesce into 1 idle pass,
			 *    leaving clean monitors untouched. */
			wl_event_loop_dispatch(server.loop, 0);
			struct wlr_output *extra_out =
				wlr_headless_add_output(headless, 640, 480);
			CHECK(extra_out != NULL);
			wl_event_loop_dispatch(server.loop, 0);
			struct tp_monitor *m_clean = extra_out ? extra_out->data : NULL;
			CHECK(m_clean != NULL && m_clean != m);

			uint64_t cnt_m = m->arrange_count;
			uint64_t cnt_clean = m_clean ? m_clean->arrange_count : 0;
			int32_t saved_outer = server.settings.gap_outer;
			int32_t before_x = fc->geom.x;

			server.settings.gap_outer = saved_outer + 10;
			tp_arrange(m);
			tp_arrange(m);
			tp_arrange(m);
			/* Deferred: layout must not run eagerly before idle. */
			CHECK(m->arrange_dirty);
			CHECK(m->arrange_count == cnt_m);
			CHECK(fc->geom.x == before_x);
			wl_event_loop_dispatch(server.loop, 0);
			CHECK(!m->arrange_dirty);
			CHECK(m->arrange_count == cnt_m + 1);
			if (m_clean)
				CHECK(m_clean->arrange_count == cnt_clean);
			CHECK(fc->geom.x == before_x + 10);

			/* 2) Rapid tiled pointer resize motions coalesce into 1 pass. */
			struct tp_client *peer = NULL, *it;
			wl_list_for_each(it, &server.clients, link) {
				if (it != fc && it->mapped) {
					peer = it;
					break;
				}
			}
			if (peer) {
				if (peer->mon != m)
					tp_client_set_monitor(peer, m, false);
				if (peer->floating)
					tp_client_set_floating(peer, false);
				if (peer->fullscreen)
					tp_client_set_fullscreen(peer, false);
				tp_client_set_tags(peer, tp_tagview_current(&m->view));
				wl_event_loop_dispatch(server.loop, 0);
				int32_t w_before = fc->geom.w;
				uint64_t cnt_drag = m->arrange_count;
				wlr_cursor_warp(server.cursor, &pointer.base,
					fc->geom.x + 1.0, fc->geom.y + 1.0);
				emit_motion(&server, &pointer, 0, 0, 900);
				send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 901);
				emit_button(&server, &pointer, BTN_RIGHT,
					WL_POINTER_BUTTON_STATE_PRESSED, 902);
				CHECK(server.grab.mode == TP_GRAB_RESIZE && server.grab.tiled);
				for (int step = 1; step <= 5; step++)
					emit_motion(&server, &pointer, 8.0, 0.0, 902 + step);
				CHECK(m->arrange_count == cnt_drag);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(m->arrange_count == cnt_drag + 1);
				CHECK(fc->geom.w != w_before);
				emit_button(&server, &pointer, BTN_RIGHT,
					WL_POINTER_BUTTON_STATE_RELEASED, 910);
				send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 911);
				CHECK(server.grab.mode == TP_GRAB_NONE);
			}

			/* 3) Cross-monitor move marks both dirty and runs 1 pass each. */
			if (m_clean) {
				uint64_t c_old = m->arrange_count;
				uint64_t c_new = m_clean->arrange_count;
				tp_client_set_monitor(fc, m_clean, false);
				CHECK(m->arrange_dirty && m_clean->arrange_dirty);
				CHECK(m->arrange_count == c_old && m_clean->arrange_count == c_new);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(m->arrange_count == c_old + 1);
				CHECK(m_clean->arrange_count == c_new + 1);
				CHECK(fc->geom.x >= m_clean->usable.x);

				/* Move back to m, then mark m_clean dirty and destroy it
				 * before idle: idle must skip freed monitor cleanly. */
				tp_client_set_monitor(fc, m, false);
				wl_event_loop_dispatch(server.loop, 0);
				tp_arrange(m_clean);
				CHECK(m_clean->arrange_dirty);
				wlr_output_destroy(extra_out);
				wl_event_loop_dispatch(server.loop, 0);
			}

			/* 4) IPC windows query flushes pending dirty arrange. */
			server.settings.gap_outer = saved_outer;
			tp_arrange(m);
			CHECK(m->arrange_dirty);
			if (ipc_sock_path[0]) {
				char rep[4096];
				char want_x[64];
				snprintf(want_x, sizeof(want_x), "\tx=%d\t", before_x);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"windows\n", rep, sizeof(rep)));
				CHECK(!m->arrange_dirty);
				CHECK(strstr(rep, want_x) != NULL);
			} else {
				wl_event_loop_dispatch(server.loop, 0);
			}
			CHECK(fc->geom.x == before_x);

			/* ---- 0.2.5: master-stack, scroller, monocle & scratchpad ---- */
			if (peer) {
				struct tp_action a = {0};

				/* Switch to master-stack and check IPC monitors reports it. */
				a.kind = TP_ACT_LAYOUT_SET;
				a.dir = TP_LAYOUT_MASTER;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(tp_monitor_layout(m)->kind == TP_LAYOUT_MASTER);
				if (ipc_sock_path[0]) {
					char rep[4096];
					CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
											"monitors\n", rep, sizeof(rep)));
					CHECK(strstr(rep, "\tlayout=master\tdpms=1\tdescription=") != NULL);

					/* 0.2.7: keyboards query and dispatch dpms off/on. */
					CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
											"keyboards\n", rep, sizeof(rep)));
					CHECK(strncmp(rep, "OK\n", 3) == 0);
					CHECK(strstr(rep, "keyboard\tname=") != NULL);
					CHECK(strstr(rep, "\tactive_keymap=") != NULL);

					CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
											"dispatch dpms off HEADLESS-1\n",
											rep, sizeof(rep)));
					CHECK(strcmp(rep, "OK\nEND\n") == 0);
					CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
											"monitors\n", rep, sizeof(rep)));
					CHECK(strstr(rep, "\tdpms=0\t") != NULL);

					CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
											"dispatch dpms on HEADLESS-1\n",
											rep, sizeof(rep)));
					CHECK(strcmp(rep, "OK\nEND\n") == 0);
					CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
											"monitors\n", rep, sizeof(rep)));
					CHECK(strstr(rep, "\tdpms=1\t") != NULL);
				}
				/* Resize master wider and verify geometry updates. */
				int32_t mw_before = fc->geom.w;
				a.kind = TP_ACT_RESIZE;
				a.dir = TP_DIR_RIGHT;
				a.step = 60;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(fc->geom.w != mw_before);

				/* Switch to scroller: columns sit side by side; focusing
				 * another window scrolls / keeps it visible. */
				a.kind = TP_ACT_LAYOUT_SET;
				a.dir = TP_LAYOUT_SCROLLER;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(tp_monitor_layout(m)->kind == TP_LAYOUT_SCROLLER);
				CHECK(fc->tree->node.enabled && peer->tree->node.enabled);

				/* Switch to monocle: only the focused tiled window is enabled
				 * in the scene, filling usable minus outer gap; cycling focus
				 * flips which window is enabled. */
				a.kind = TP_ACT_LAYOUT_SET;
				a.dir = TP_LAYOUT_MONOCLE;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(tp_monitor_layout(m)->kind == TP_LAYOUT_MONOCLE);
				tp_focus(&server, fc);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(fc->tree->node.enabled && !peer->tree->node.enabled);
				tp_focus(&server, peer);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(peer->tree->node.enabled && !fc->tree->node.enabled);

				/* Per-tag independence: tag 2 keeps its own layout (dwindle). */
				a.kind = TP_ACT_TAG_VIEW;
				a.tag = 2;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(tp_monitor_layout(m)->kind == TP_LAYOUT_DWINDLE);
				a.kind = TP_ACT_TAG_BACK;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(tp_monitor_layout(m)->kind == TP_LAYOUT_MONOCLE);

				/* Cycle back to dwindle. */
				a.kind = TP_ACT_LAYOUT_CYCLE;
				a.dir = 1;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(tp_monitor_layout(m)->kind == TP_LAYOUT_DWINDLE);
				CHECK(fc->tree->node.enabled && peer->tree->node.enabled);

				/* Scratchpad: move focused window (peer) to scratchpad ->
				 * hidden & floating; toggle shows it and focuses it; tag
				 * switch keeps it visible; toggle again hides it; release
				 * returns it to normal tag management. */
				tp_focus(&server, peer);
				a.kind = TP_ACT_SCRATCHPAD_MOVE;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(peer->scratchpad && !peer->scratchpad_visible &&
					  peer->floating);
				CHECK(!peer->tree->node.enabled);
				CHECK(tp_focused(&server) == fc);

				a.kind = TP_ACT_SCRATCHPAD_TOGGLE;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(peer->scratchpad && peer->scratchpad_visible);
				CHECK(peer->tree->node.enabled);
				CHECK(tp_focused(&server) == peer);
				if (ipc_sock_path[0]) {
					char rep[4096];
					CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
											"focused\n", rep, sizeof(rep)));
					CHECK(strstr(rep, "\tscratchpad=1\t") != NULL);
				}

				/* Switching tags keeps shown scratchpad visible. */
				a.kind = TP_ACT_TAG_VIEW;
				a.tag = 2;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(tp_client_visible(peer) && peer->tree->node.enabled);
				a.kind = TP_ACT_TAG_BACK;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);

				/* Toggle off hides it again. */
				a.kind = TP_ACT_SCRATCHPAD_TOGGLE;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(!peer->scratchpad_visible && !peer->tree->node.enabled);

				/* Toggle back on and release via scratchpad move. */
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(tp_focused(&server) == peer);
				a.kind = TP_ACT_SCRATCHPAD_MOVE;
				tp_action_run(&server, &a);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(!peer->scratchpad && !peer->scratchpad_visible);
			}
		}
	}

	/* 0.2.6 XWayland security classification & IPC xwayland/display fields. */
	CHECK(tp_security_classify_global("xwayland_shell_v1") ==
		  TP_GLOBAL_PRIVILEGED);
	CHECK(!tp_security_allow_xwayland(&server, NULL));
	if (ipc_sock_path[0]) {
		char rep[4096];
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "version\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "\tdisplay=") != NULL);
#if defined(TP_XWAYLAND) && TP_XWAYLAND
		CHECK(server.xwayland != NULL);
		CHECK(server.xwayland_display != NULL &&
			  server.xwayland_display[0] == ':');
		CHECK(strstr(rep, "\tdisplay=:") != NULL);
#else
		CHECK(server.xwayland == NULL);
		CHECK(server.xwayland_display == NULL);
		CHECK(strstr(rep, "\tdisplay=\n") != NULL);
#endif
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "windows\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "\txwayland=0\t") != NULL);
	}

#if defined(TP_XWAYLAND) && TP_XWAYLAND
	/* Real XWayland integration check: spawn build/sandbox/demo-x11 with an
	 * override-redirect popup against the compositor's lazy Xwayland display.
	 * Verifies lazy startup, WM_CLASS/WM_NAME -> app_id/title, tiling, IPC
	 * xwayland=1, foreign-toplevel export, OR popup on TP_LAYER_OVERLAY,
	 * float/fullscreen/scratchpad transitions, and WM_DELETE_WINDOW close. */
	if (server.xwayland && server.xwayland_display &&
		access("build/sandbox/demo-x11", X_OK) == 0) {
		pid_t xpid = fork();
		CHECK(xpid >= 0);
		if (xpid == 0) {
			setenv("DISPLAY", server.xwayland_display, 1);
			execl("build/sandbox/demo-x11", "demo-x11", "--title", "x11-check",
				  "--popup", (char *)NULL);
			_exit(127);
		}
		if (xpid > 0) {
			struct tp_client *xc = NULL;
			for (int i = 0; i < 400 && !xc; i++) {
				wl_event_loop_dispatch(server.loop, 20);
				wl_display_flush_clients(server.display);
				struct tp_client *it;
				wl_list_for_each(it, &server.clients, link) {
					if (tp_client_is_xwayland(it) && it->mapped) {
						xc = it;
						break;
					}
				}
			}
			CHECK(xc != NULL);
			if (xc) {
				/* Drain idle arrange and let OR popup map. */
				for (int i = 0; i < 20; i++) {
					wl_event_loop_dispatch(server.loop, 10);
					wl_display_flush_clients(server.display);
				}
				CHECK(tp_client_app_id(xc) &&
					  strcmp(tp_client_app_id(xc), "demo-x11") == 0);
				CHECK(tp_client_title(xc) &&
					  strcmp(tp_client_title(xc), "x11-check") == 0);
				CHECK(!xc->floating);
				CHECK(xc->geom.w > 0 && xc->geom.h > 0);
				CHECK(xc->foreign != NULL);
				/* OR popup mapped on TP_LAYER_OVERLAY without entering clients. */
				int xcount = 0;
				struct tp_client *it;
				wl_list_for_each(it, &server.clients, link) {
					if (tp_client_is_xwayland(it))
						xcount++;
				}
				CHECK(xcount == 1);
				CHECK(!wl_list_empty(&server.layer[TP_LAYER_OVERLAY]->children));

				if (ipc_sock_path[0]) {
					char rep[4096];
					CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
											"windows\n", rep, sizeof(rep)));
					CHECK(strstr(rep, "\txwayland=1\t") != NULL);
					CHECK(strstr(rep, "\tapp_id=demo-x11\t") != NULL);
					CHECK(strstr(rep, "\ttitle=x11-check\n") != NULL);
				}

				/* Float, fullscreen, scratchpad transitions on XWayland window. */
				tp_client_set_floating(xc, true);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(xc->floating);
				tp_client_set_fullscreen(xc, true);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(xc->fullscreen);
				tp_client_set_fullscreen(xc, false);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(!xc->fullscreen);
				tp_client_scratchpad_move(xc);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(xc->scratchpad && !xc->scratchpad_visible);
				tp_scratchpad_toggle(&server);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(xc->scratchpad && xc->scratchpad_visible);
				tp_client_scratchpad_move(xc);
				tp_client_set_floating(xc, false);
				wl_event_loop_dispatch(server.loop, 0);
				CHECK(!xc->scratchpad && !xc->floating);

				/* Close via WM_DELETE_WINDOW and verify clean unmap & exit. */
				tp_client_close(xc);
				bool unmapped = false;
				for (int i = 0; i < 200 && !unmapped; i++) {
					wl_event_loop_dispatch(server.loop, 10);
					wl_display_flush_clients(server.display);
					bool found = false;
					wl_list_for_each(it, &server.clients, link) {
						if (tp_client_is_xwayland(it))
							found = true;
					}
					if (!found)
						unmapped = true;
				}
				CHECK(unmapped);
			}
			int xstatus = 0;
			pid_t got = 0;
			for (int i = 0; i < 100; i++) {
				got = waitpid(xpid, &xstatus, WNOHANG);
				if (got == xpid)
					break;
				wl_event_loop_dispatch(server.loop, 10);
			}
			if (got != xpid) {
				kill(xpid, SIGKILL);
				waitpid(xpid, &xstatus, 0);
			}
			CHECK(got == xpid && WIFEXITED(xstatus) &&
				  WEXITSTATUS(xstatus) == 0);
		}
	}
#endif

	/* ---- Renderer-loss recovery (R1 finding 11, 0.2.10) ---- */
	{
		struct wlr_renderer *r0 = server.renderer;
		struct wlr_allocator *a0 = server.allocator;
		CHECK(r0 != NULL && a0 != NULL);
		CHECK(server.renderer_recoveries == 0);

		wl_signal_emit_mutable(&server.renderer->events.lost, NULL);
		wl_event_loop_dispatch(server.loop, 0);
		CHECK(server.renderer_recoveries == 1);
		CHECK(server.renderer != NULL && server.renderer != r0);
		CHECK(server.allocator != NULL && server.allocator != a0);
		CHECK(server.renderer_lost.notify != NULL);
		struct tp_monitor *rm;
		wl_list_for_each(rm, &server.monitors, link) {
			CHECK(rm->output->renderer == server.renderer);
			CHECK(rm->output->allocator == server.allocator);
		}

		/* Second back-to-back reset verifies listener re-arming. */
		struct wlr_renderer *r1 = server.renderer;
		struct wlr_allocator *a1 = server.allocator;
		wl_signal_emit_mutable(&server.renderer->events.lost, NULL);
		wl_event_loop_dispatch(server.loop, 0);
		CHECK(server.renderer_recoveries == 2);
		CHECK(server.renderer != NULL && server.renderer != r1);
		CHECK(server.allocator != NULL && server.allocator != a1);
		wl_list_for_each(rm, &server.monitors, link) {
			CHECK(rm->output->renderer == server.renderer);
			CHECK(rm->output->allocator == server.allocator);
		}
	}

	/* Focus decorations are checked after the layout fixtures. */
	{
		server.settings.border_width = 3;
		struct test_window first = {0}, second = {0};
		configure_window_app(&client, &first, "border-first", "Border first");
		for (unsigned int i = 0; i < 30; i++)
			if (!pump(&client, &server)) goto fail;
		struct tp_client *first_client = tp_focused(&server);
		configure_window_app(&client, &second, "border-second", "Border second");
		for (unsigned int i = 0; i < 30; i++)
			if (!pump(&client, &server)) goto fail;
		struct tp_client *second_client = tp_focused(&server);
		CHECK(first_client && second_client && first_client != second_client);
		if (first_client && second_client && first_client != second_client) {
			struct tp_rect first_content = first_client->content_geom;
			struct tp_rect second_content = second_client->content_geom;
			const struct wlr_scene_rect *edge = second_client->border[0];
			CHECK(edge && edge->node.enabled);
			if (edge) {
				CHECK(edge->height == 3 && edge->width == second_client->geom.w);
				CHECK(edge->color[0] == 0.0f && edge->color[1] == 1.0f &&
					  edge->color[2] == 0.0f && edge->color[3] == 1.0f);
			}
			CHECK(second_content.w == second_client->geom.w - 6);
			double x = server.cursor->x, y = server.cursor->y;
			char rep[256];
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
					"dispatch focus_app border-first\n", rep, sizeof(rep)));
			CHECK(tp_focused(&server) == first_client);
			CHECK(server.cursor->x == x && server.cursor->y == y);
			CHECK(first_client->border[0] && first_client->border[0]->node.enabled);
			CHECK(edge && !edge->node.enabled);
			CHECK(first_client->content_geom.w == first_content.w &&
				  first_client->content_geom.h == first_content.h);
			CHECK(second_client->content_geom.w == second_content.w &&
				  second_client->content_geom.h == second_content.h);
			tp_client_set_fullscreen(first_client, true);
			tp_arrange_flush(&server);
			CHECK(!first_client->border[0]->node.enabled);
			tp_client_set_fullscreen(first_client, false);
			tp_arrange_flush(&server);
			CHECK(first_client->border[0]->node.enabled);
			server.settings.border_width = 0;
			tp_arrange_all(&server);
			tp_arrange_flush(&server);
			CHECK(!first_client->border[0]->node.enabled);
		}
		destroy_window(&first);
		destroy_window(&second);
	}

	/* ---- 0.2.16: lock_state/events, DPMS-while-locked, gamma, freeze, pixel ---- */
	{
		char rep[512];
		/* 1. Native gamma query, dispatch, and gamma.change event */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"gamma\n", rep, sizeof(rep)));
		CHECK(strstr(rep, "gamma\ttemperature=6500\tenabled=0\n") != NULL);

		int sub_fd = ipc_test_connect(ipc_sock_path);
		CHECK(sub_fd >= 0);
		if (sub_fd >= 0) {
			const char *sub_cmd =
				"subscribe session.lock session.unlock gamma.change\n";
			CHECK(write(sub_fd, sub_cmd, strlen(sub_cmd)) ==
				  (ssize_t)strlen(sub_cmd));
			for (unsigned int i = 0; i < 10; i++)
				wl_event_loop_dispatch(server.loop, 5);
			char sbuf[1024] = {0};
			ssize_t sn = read(sub_fd, sbuf, sizeof(sbuf) - 1);
			CHECK(sn > 0 && strncmp(sbuf, "OK\n", 3) == 0);

			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"dispatch gamma 4000\n", rep, sizeof(rep)));
			CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"gamma\n", rep, sizeof(rep)));
			CHECK(strstr(rep, "gamma\ttemperature=4000\tenabled=1\n") != NULL);

			memset(sbuf, 0, sizeof(sbuf));
			for (unsigned int i = 0; i < 10 && !sbuf[0]; i++) {
				wl_event_loop_dispatch(server.loop, 5);
				sn = read(sub_fd, sbuf, sizeof(sbuf) - 1);
				if (sn < 0)
					sbuf[0] = '\0';
			}
			CHECK(strstr(sbuf, "event\tgamma.change\ttemperature=4000\tenabled=1\n") != NULL);

			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"dispatch gamma reset\n", rep, sizeof(rep)));
			CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"gamma\n", rep, sizeof(rep)));
			CHECK(strstr(rep, "gamma\ttemperature=6500\tenabled=0\n") != NULL);

			memset(sbuf, 0, sizeof(sbuf));
			for (unsigned int i = 0; i < 10 && !sbuf[0]; i++) {
				wl_event_loop_dispatch(server.loop, 5);
				sn = read(sub_fd, sbuf, sizeof(sbuf) - 1);
				if (sn < 0)
					sbuf[0] = '\0';
			}
			CHECK(strstr(sbuf, "event\tgamma.change\ttemperature=6500\tenabled=0\n") != NULL);

			/* 2. Pixel sampling & native screen freeze */
			static const float color_a[4] = {
				51.0f / 255.0f, 102.0f / 255.0f, 153.0f / 255.0f, 1.0f
			};
			static const float color_b[4] = {
				170.0f / 255.0f, 34.0f / 255.0f, 17.0f / 255.0f, 1.0f
			};
			struct wlr_scene_rect *swatch =
				wlr_scene_rect_create(server.layer[TP_LAYER_TILE], 100, 100, color_a);
			CHECK(swatch != NULL);
			if (swatch) {
				wlr_scene_node_set_position(&swatch->node, 0, 0);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"pixel 20 20\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "pixel\tx=20\ty=20\tr=51\tg=102\tb=153\thex=#336699\n") != NULL);

				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"freeze\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "freeze\tactive=0\n") != NULL);

				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"dispatch freeze on\n", rep, sizeof(rep)));
				CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"freeze\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "freeze\tactive=1\n") != NULL);

				/* Mutate underlying scene while frozen: pixel still reads frozen frame! */
				wlr_scene_rect_set_color(swatch, color_b);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"pixel 20 20\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "hex=#336699\n") != NULL);

				/* Unfreeze: live scene color #aa2211 is visible again. */
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"dispatch freeze off\n", rep, sizeof(rep)));
				CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"pixel 20 20\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "pixel\tx=20\ty=20\tr=170\tg=34\tb=17\thex=#aa2211\n") != NULL);

				/* Re-freeze to verify locking auto-unfreezes. */
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"dispatch freeze on\n", rep, sizeof(rep)));
				CHECK(server.frozen);
			}

			/* 3. lock_state query, session.lock/unlock events, DPMS-while-locked */
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"lock_state\n", rep, sizeof(rep)));
			CHECK(strstr(rep, "lock\tlocked=0\tactive=0\tlocker_running=0\tsurfaces=0\toutputs=") != NULL);

			int lsv[2];
			CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lsv) == 0);
			struct wl_client *lcl = wl_client_create(server.display, lsv[0]);
			CHECK(lcl != NULL);
			tp_security_set_locker_client(&server, lcl);
			struct test_client lc = {.shm_fd = -1};
			lc.display = wl_display_connect_to_fd(lsv[1]);
			CHECK(lc.display != NULL);
			if (lc.display) {
				lc.registry = wl_display_get_registry(lc.display);
				wl_registry_add_listener(lc.registry, &registry_listener, &lc);
				for (unsigned int i = 0; i < 20 && (!lc.lock_mgr || !lc.compositor || !lc.output || !lc.shm); i++) {
					pump(&lc, &server);
					pump(&client, &server);
				}
				CHECK(lc.lock_mgr != NULL);
				struct test_lock_state lst = {0};
				struct ext_session_lock_v1 *lock =
					ext_session_lock_manager_v1_lock(lc.lock_mgr);
				ext_session_lock_v1_add_listener(lock, &lock_listener, &lst);
				struct wl_surface *lsurf_wl =
					wl_compositor_create_surface(lc.compositor);
				struct test_lock_surf_state lss = {
					.client = &lc, .surface = lsurf_wl
				};
				struct ext_session_lock_surface_v1 *lsurf =
					ext_session_lock_v1_get_lock_surface(lock, lsurf_wl, lc.output);
				ext_session_lock_surface_v1_add_listener(lsurf, &lock_surf_listener, &lss);
				for (unsigned int i = 0;
					 i < 30 && (!lst.locked || !lss.configured ||
								tp_session_lock_mapped_surface_count(&server) == 0);
					 i++) {
					pump(&lc, &server);
					pump(&client, &server);
				}
				CHECK(server.locked);
				CHECK(!server.frozen); /* auto-unfrozen on lock */

				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"lock_state\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "lock\tlocked=1\tactive=1\tlocker_running=1\tsurfaces=1\toutputs=") != NULL);

				memset(sbuf, 0, sizeof(sbuf));
				for (unsigned int i = 0; i < 10 && !sbuf[0]; i++) {
					wl_event_loop_dispatch(server.loop, 5);
					sn = read(sub_fd, sbuf, sizeof(sbuf) - 1);
					if (sn < 0)
						sbuf[0] = '\0';
				}
				CHECK(strstr(sbuf, "event\tsession.lock\tlocked=1\n") != NULL);

				/* DPMS is allowed while locked; close/freeze/pixel are refused. */
				struct tp_monitor *m0 =
					wl_container_of(server.monitors.next, m0, link);
				char dpms_cmd[128];
				snprintf(dpms_cmd, sizeof(dpms_cmd), "dispatch dpms off %s\n",
						 m0->output->name);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										dpms_cmd, rep, sizeof(rep)));
				CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
				CHECK(!m0->output->enabled);
				snprintf(dpms_cmd, sizeof(dpms_cmd), "dispatch dpms on %s\n",
						 m0->output->name);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										dpms_cmd, rep, sizeof(rep)));
				CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
				CHECK(m0->output->enabled);

				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"dispatch close\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "ERR session is locked") != NULL);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"dispatch freeze on\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "ERR session is locked") != NULL);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"pixel 20 20\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "ERR session is locked") != NULL);

				ext_session_lock_surface_v1_destroy(lsurf);
				wl_surface_destroy(lsurf_wl);
				ext_session_lock_v1_unlock_and_destroy(lock);
				for (unsigned int i = 0; i < 20 && server.locked; i++) {
					pump(&lc, &server);
					pump(&client, &server);
				}
				CHECK(!server.locked);

				memset(sbuf, 0, sizeof(sbuf));
				for (unsigned int i = 0; i < 10 && !sbuf[0]; i++) {
					wl_event_loop_dispatch(server.loop, 5);
					sn = read(sub_fd, sbuf, sizeof(sbuf) - 1);
					if (sn < 0)
						sbuf[0] = '\0';
				}
				CHECK(strstr(sbuf, "event\tsession.unlock\tlocked=0\n") != NULL);

				wl_display_disconnect(lc.display);
				for (unsigned int i = 0; i < 10; i++)
					wl_event_loop_dispatch(server.loop, 5);
				CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
										"lock_state\n", rep, sizeof(rep)));
				CHECK(strstr(rep, "lock\tlocked=0\tactive=0\tlocker_running=0\tsurfaces=0\toutputs=") != NULL);
			}
			if (swatch)
				wlr_scene_node_destroy(&swatch->node);
			close(sub_fd);
		}
	}

	/* ---- 0.2.17: monitor enable/disable, devices IPC, window targeting/pin,
	 *      and themeable focused/unfocused borders ---- */
	{
		char rep[4096];

		/* 1. devices IPC lists input devices arriving via backend->events.new_input */
		{
			struct wlr_pointer tp_dev = {0};
			wlr_pointer_init(&tp_dev, &pointer_impl, "SynPS/2 Synaptics TouchPad");
			wl_signal_emit_mutable(&server.backend->events.new_input,
								   &tp_dev.base);

			struct wlr_input_device touch_dev = {
				.type = WLR_INPUT_DEVICE_TOUCH,
				.name = "ELAN Touchscreen",
			};
			wl_signal_init(&touch_dev.events.destroy);
			wl_signal_emit_mutable(&server.backend->events.new_input,
								   &touch_dev);

			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"devices\n", rep, sizeof(rep)));
			CHECK(strstr(rep, "device\ttype=pointer\tname=SynPS/2 Synaptics TouchPad\n") != NULL);
			CHECK(strstr(rep, "device\ttype=touch\tname=ELAN Touchscreen\n") != NULL);

			wl_signal_emit_mutable(&touch_dev.events.destroy, &touch_dev);
			wlr_pointer_finish(&tp_dev);

			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
									"devices\n", rep, sizeof(rep)));
			CHECK(strstr(rep, "SynPS/2 Synaptics TouchPad") == NULL);
			CHECK(strstr(rep, "ELAN Touchscreen") == NULL);
		}

		/* 2. Monitor enable/disable with window displacement & reclamation */
		struct wlr_output *out_d =
			wlr_headless_add_output(headless, 800, 600);
		CHECK(out_d != NULL);
		for (unsigned int i = 0; i < 10; i++)
			pump(&client, &server);
		struct tp_monitor *md = tp_monitor_by_name(&server, out_d->name);
		CHECK(md != NULL && !md->disabled);

		struct tp_monitor *m0 =
			wl_container_of(server.monitors.next, m0, link);
		tp_monitor_focus(&server, md);
		size_t n_before = wl_list_length(&server.clients);
		struct test_window wd = {0};
		wd.client = &client;
		configure_window(&client, &wd, "win-d");
		for (unsigned int i = 0; i < 40 && (size_t)wl_list_length(&server.clients) == n_before; i++)
			pump(&client, &server);
		struct tp_client *cd = tp_focused(&server);
		CHECK(cd != NULL && cd->mon == md);

		char cmd[128];
		snprintf(cmd, sizeof(cmd), "dispatch monitor disable %s\n",
				 out_d->name);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
		CHECK(md->disabled && !out_d->enabled);
		CHECK(server.selmon == m0);
		CHECK(cd && cd->mon == m0 && cd->home_output &&
			  strcmp(cd->home_output, out_d->name) == 0);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "monitors\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "\tenabled=0\n") != NULL);
		CHECK(strstr(rep, "\tenabled=1\n") != NULL);

		snprintf(cmd, sizeof(cmd), "dispatch monitor enable %s\n", out_d->name);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
		CHECK(!md->disabled && out_d->enabled);
		CHECK(cd && cd->mon == md && cd->home_output == NULL);

		/* Clean up extra output and its window */
		destroy_window(&wd);
		for (unsigned int i = 0; i < 10; i++)
			pump(&client, &server);
		wlr_output_destroy(out_d);
		for (unsigned int i = 0; i < 10; i++)
			pump(&client, &server);

		/* 3. Pin, float, resize_exact, move_exact, center, raise, close by ID */
		tp_monitor_focus(&server, m0);
		n_before = wl_list_length(&server.clients);
		struct test_window wa = {0};
		wa.client = &client;
		configure_window(&client, &wa, "win-a");
		for (unsigned int i = 0; i < 40 && (size_t)wl_list_length(&server.clients) == n_before; i++)
			pump(&client, &server);
		struct tp_client *ca = tp_focused(&server);
		CHECK(ca != NULL);
		uint64_t id_a = ca ? ca->id : 0;

		n_before = wl_list_length(&server.clients);
		struct test_window wb = {0};
		wb.client = &client;
		configure_window(&client, &wb, "win-b");
		for (unsigned int i = 0; i < 40 && (size_t)wl_list_length(&server.clients) == n_before; i++)
			pump(&client, &server);
		struct tp_client *cb = tp_focused(&server);
		CHECK(cb != NULL && cb != ca);

		/* Pin non-focused window ca by ID */
		snprintf(cmd, sizeof(cmd), "dispatch pin on 0x%llx\n",
				 (unsigned long long)id_a);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
		CHECK(ca->floating && ca->pinned);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "windows\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "\tpinned=1\t") != NULL);

		/* Switch to tag 2: pinned ca stays visible, unpinned cb hides */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch tag view 2\n", rep, sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
		tp_arrange_flush(&server);
		CHECK(tp_client_visible(ca));
		CHECK(!tp_client_visible(cb));

		/* resize_exact, move_exact, center, raise by ID */
		snprintf(cmd, sizeof(cmd), "dispatch resize_exact 420 310 0x%llx\n",
				 (unsigned long long)id_a);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
		CHECK(ca->float_geom.w == 420 && ca->float_geom.h == 310);

		snprintf(cmd, sizeof(cmd), "dispatch move_exact 15 25 0x%llx\n",
				 (unsigned long long)id_a);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
		CHECK(ca->float_geom.x == m0->usable.x + 15 &&
			  ca->float_geom.y == m0->usable.y + 25);

		snprintf(cmd, sizeof(cmd), "dispatch center 0x%llx\n",
				 (unsigned long long)id_a);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
		CHECK(ca->float_geom.x == m0->usable.x + (m0->usable.w - 420) / 2 &&
			  ca->float_geom.y == m0->usable.y + (m0->usable.h - 310) / 2);

		snprintf(cmd, sizeof(cmd), "dispatch raise 0x%llx\n",
				 (unsigned long long)id_a);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);

		/* Unpin ca: now hidden on tag 2 */
		snprintf(cmd, sizeof(cmd), "dispatch pin off 0x%llx\n",
				 (unsigned long long)id_a);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
		tp_arrange_flush(&server);
		CHECK(!ca->pinned && !tp_client_visible(ca));

		/* Back to tag 1 for border & close tests */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch tag view 1\n", rep, sizeof(rep)));
		tp_arrange_flush(&server);

		/* 4. Themeable focused & unfocused borders with live reload */
		{
			FILE *f = fopen(lua_cfg, "w");
			CHECK(f != NULL);
			fputs("tp.config({ border_width = 3, "
				  "border_color_focused = '#ff0000', "
				  "border_color_unfocused = '#0000ff80' })\n",
				  f);
			fclose(f);
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "reload\n",
									rep, sizeof(rep)));
			CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
			tp_arrange_flush(&server);
			struct tp_client *foc = tp_focused(&server);
			struct tp_client *unfoc = (foc == ca) ? cb : ca;
			CHECK(foc && unfoc);
			CHECK(foc->border[0] != NULL && foc->border[0]->node.enabled);
			CHECK(foc->border[0]->color[0] > 0.99f &&
				  foc->border[0]->color[1] == 0.0f &&
				  foc->border[0]->color[3] == 1.0f);
			CHECK(unfoc->border[0] != NULL && unfoc->border[0]->node.enabled);
			CHECK(unfoc->border[0]->color[2] > 0.49f &&
				  unfoc->border[0]->color[2] < 0.51f &&
				  unfoc->border[0]->color[3] > 0.49f &&
				  unfoc->border[0]->color[3] < 0.51f);

			/* Reload with unfocused border disabled ('none') */
			f = fopen(lua_cfg, "w");
			CHECK(f != NULL);
			fputs("tp.config({ border_width = 3, "
				  "border_color_focused = '#00ff00', "
				  "border_color_unfocused = 'none' })\n",
				  f);
			fclose(f);
			CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "reload\n",
									rep, sizeof(rep)));
			CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);
			tp_arrange_flush(&server);
			CHECK(foc->border[0]->node.enabled &&
				  foc->border[0]->color[1] > 0.99f);
			CHECK(!unfoc->border[0]->node.enabled);
		}

		/* Unknown window ID returns ERR */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch close 0xdeadbeef\n", rep,
								sizeof(rep)));
		CHECK(strstr(rep, "ERR unknown window") != NULL);

		/* Close non-focused window by ID */
		snprintf(cmd, sizeof(cmd), "dispatch close 0x%llx\n",
				 (unsigned long long)id_a);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, cmd, rep,
								sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);

		destroy_window(&wa);
		destroy_window(&wb);
		for (unsigned int i = 0; i < 10; i++)
			pump(&client, &server);
	}

	/* ---- 0.3.0: R1-17 keyboard grouping and UTF-8-safe event truncation ---- */
	{
		char rep[4096];
		FILE *f = fopen(lua_cfg, "w");
		CHECK(f != NULL);
		fputs("tp.bind('Meta+1', tp.dsp.tag('view', 1))\n"
			  "tp.bind('Meta+2', tp.dsp.tag('view', 2))\n"
			  "tp.on('window.open', function(ev)\n"
			  "  if ev.title and #ev.title == 253 and "
			  "ev.title:sub(-3) == '\xe3\x81\x82' then\n"
			  "    tp.dsp.tag('view', 3)()\n"
			  "  end\n"
			  "end)\n",
			  f);
		fclose(f);
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "reload\n",
								rep, sizeof(rep)));
		CHECK(strncmp(rep, "OK\nEND\n", 7) == 0);

		/* Add a second keyboard and verify modifiers combine across devices. */
		struct wlr_keyboard keyboard2;
		wlr_keyboard_init(&keyboard2, &keyboard_impl, "test keyboard 2");
		tp_keyboard_add(&server, &keyboard2.base);

		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path, "keyboards\n",
								rep, sizeof(rep)));
		CHECK(strstr(rep, "name=test keyboard\t") != NULL);
		CHECK(strstr(rep, "name=test keyboard 2\t") != NULL);

		/* Ensure we start on tag 1 before testing Meta+2 across keyboards. */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch tag view 1\n", rep, sizeof(rep)));
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(1));

		/* Hold Meta on keyboard 1, press '2' on keyboard 2 -> switches to tag 2. */
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 600);
		send_key(&keyboard2, KEY_2, WL_KEYBOARD_KEY_STATE_PRESSED, 601);
		send_key(&keyboard2, KEY_2, WL_KEYBOARD_KEY_STATE_RELEASED, 602);
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 603);
		for (unsigned int i = 0; i < 10; i++)
			pump(&client, &server);
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(2));

		/* Remove keyboard 2 and verify keyboard 1 still works cleanly. */
		wlr_keyboard_finish(&keyboard2);
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 604);
		send_key(&keyboard, KEY_1, WL_KEYBOARD_KEY_STATE_PRESSED, 605);
		send_key(&keyboard, KEY_1, WL_KEYBOARD_KEY_STATE_RELEASED, 606);
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 607);
		for (unsigned int i = 0; i < 10; i++)
			pump(&client, &server);
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(1));

		/* UTF-8-safe event string truncation: 'a' + 86 * 3-byte 'あ' = 259 bytes.
		 * Truncating to 255 bytes must back up to 253 (complete UTF-8). */
		char long_utf8[300];
		size_t pos = 0;
		long_utf8[pos++] = 'a';
		for (int i = 0; i < 86; i++) {
			long_utf8[pos++] = (char)0xe3;
			long_utf8[pos++] = (char)0x81;
			long_utf8[pos++] = (char)0x82;
		}
		long_utf8[pos] = '\0';
		struct test_window wu = {0};
		wu.client = &client;
		configure_window(&client, &wu, long_utf8);
		for (unsigned int i = 0; i < 60 &&
			 tp_tagview_current(&m1->view) != tp_tag_mask(3); i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(3));
		destroy_window(&wu);
		for (unsigned int i = 0; i < 10; i++)
			pump(&client, &server);
	}

	/* ---- Keyboard shortcuts inhibit (zwp_keyboard_shortcuts_inhibit_manager_v1) ---- */
	{
		char rep[4096];
		CHECK(tp_security_classify_global(
				  "zwp_keyboard_shortcuts_inhibit_manager_v1") ==
			  TP_GLOBAL_PRIVILEGED);
		CHECK(client.has_shortcuts_inhibit);
		CHECK(client.shortcuts_inhibit_mgr != NULL);

		/* Start on tag 1 with Meta+1 / Meta+2 bound from the previous block. */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch tag view 1\n", rep, sizeof(rep)));
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(1));

		struct test_window wa = {0}, wb = {0};
		wa.client = &client;
		wb.client = &client;
		configure_window(&client, &wa, "si-a");
		for (unsigned int i = 0; i < 40 && client.keyboard_focus != wa.surface;
			 i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(client.keyboard_focus == wa.surface);

		/* Creating an inhibitor on the focused toplevel activates immediately. */
		struct test_shortcut_inhibit_state si_a = {0};
		struct zwp_keyboard_shortcuts_inhibitor_v1 *inh_a =
			zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
				client.shortcuts_inhibit_mgr, wa.surface, client.seat);
		zwp_keyboard_shortcuts_inhibitor_v1_add_listener(
			inh_a, &test_si_listener, &si_a);
		for (unsigned int i = 0; i < 30 && !si_a.active; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(si_a.active);
		CHECK(si_a.active_count == 1);
		CHECK(tp_shortcuts_inhibited(&server));

		/* Bound shortcut Meta+2 must NOT switch tags; KEY_2 reaches client. */
		client.key_events = 0;
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 700);
		send_key(&keyboard, KEY_2, WL_KEYBOARD_KEY_STATE_PRESSED, 701);
		send_key(&keyboard, KEY_2, WL_KEYBOARD_KEY_STATE_RELEASED, 702);
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 703);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(1));
		bool saw_key2 = false;
		for (unsigned int i = 0; i < client.key_events && i < 8; i++) {
			if (client.keys[i] == KEY_2 &&
				client.key_states[i] == WL_KEYBOARD_KEY_STATE_PRESSED)
				saw_key2 = true;
		}
		CHECK(saw_key2);

		/* Hard-wired VT switch (Ctrl+Alt+F1) is never inhibited. */
		last_vt = 0;
		server.change_vt = mock_change_vt;
		send_key(&keyboard, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED, 704);
		send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED, 705);
		send_key(&keyboard, KEY_F1, WL_KEYBOARD_KEY_STATE_PRESSED, 706);
		send_key(&keyboard, KEY_F1, WL_KEYBOARD_KEY_STATE_RELEASED, 707);
		send_key(&keyboard, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED, 708);
		send_key(&keyboard, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED, 709);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(last_vt == 1);

		/* Focusing a second window deactivates wa's inhibitor and restores binds. */
		configure_window(&client, &wb, "si-b");
		for (unsigned int i = 0; i < 40 && client.keyboard_focus != wb.surface;
			 i++)
			if (!pump(&client, &server)) goto fail;
		for (unsigned int i = 0; i < 20 && si_a.active; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(!si_a.active);
		CHECK(si_a.inactive_count == 1);
		CHECK(!tp_shortcuts_inhibited(&server));

		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 710);
		send_key(&keyboard, KEY_2, WL_KEYBOARD_KEY_STATE_PRESSED, 711);
		send_key(&keyboard, KEY_2, WL_KEYBOARD_KEY_STATE_RELEASED, 712);
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 713);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(2));

		/* Switch back to tag 1, close wb so focus returns to wa -> re-activates. */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch tag view 1\n", rep, sizeof(rep)));
		destroy_window(&wb);
		for (unsigned int i = 0; i < 40 && !si_a.active; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(si_a.active);
		CHECK(si_a.active_count == 2);

		/* Session lock deactivates inhibitor; unlock re-activates it. */
		server.locked = true;
		tp_shortcuts_inhibit_update(&server);
		for (unsigned int i = 0; i < 20 && si_a.active; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(!si_a.active);
		CHECK(!tp_shortcuts_inhibited(&server));
		server.locked = false;
		tp_shortcuts_inhibit_update(&server);
		for (unsigned int i = 0; i < 20 && !si_a.active; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(si_a.active);

		/* Destroying the inhibitor immediately restores compositor shortcuts. */
		zwp_keyboard_shortcuts_inhibitor_v1_destroy(inh_a);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(!tp_shortcuts_inhibited(&server));
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 720);
		send_key(&keyboard, KEY_2, WL_KEYBOARD_KEY_STATE_PRESSED, 721);
		send_key(&keyboard, KEY_2, WL_KEYBOARD_KEY_STATE_RELEASED, 722);
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 723);
		for (unsigned int i = 0; i < 10; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(2));

		/* OVERLAY + ON_DEMAND layer surface (Explorer.qml pattern). */
		CHECK(ipc_test_exchange(&client, &server, ipc_sock_path,
								"dispatch tag view 1\n", rep, sizeof(rep)));
		struct test_layer explorer_layer;
		create_layer(&client, &explorer_layer,
					 ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
					 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
						 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
					 400, 300, 0,
					 ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
		for (unsigned int i = 0; i < 40 && !explorer_layer.configured; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(explorer_layer.configured);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;

		/* Click the overlay surface to focus it, then inhibit shortcuts. */
		wlr_cursor_warp(server.cursor, &pointer.base,
						server.selmon->area.x + 1.0,
						server.selmon->area.y + 1.0);
		emit_motion(&server, &pointer, 0, 0, 725);
		emit_button(&server, &pointer, BTN_LEFT,
					WL_POINTER_BUTTON_STATE_PRESSED, 726);
		emit_button(&server, &pointer, BTN_LEFT,
					WL_POINTER_BUTTON_STATE_RELEASED, 727);
		for (unsigned int i = 0;
			 i < 30 && client.keyboard_focus != explorer_layer.surface; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(client.keyboard_focus == explorer_layer.surface);

		struct test_shortcut_inhibit_state si_l = {0};
		struct zwp_keyboard_shortcuts_inhibitor_v1 *inh_l =
			zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
				client.shortcuts_inhibit_mgr, explorer_layer.surface,
				client.seat);
		zwp_keyboard_shortcuts_inhibitor_v1_add_listener(
			inh_l, &test_si_listener, &si_l);
		for (unsigned int i = 0; i < 30 && !si_l.active; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(si_l.active);
		CHECK(tp_shortcuts_inhibited(&server));

		/* Meta+2 goes to the Explorer layer surface, not the compositor. */
		client.key_events = 0;
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED, 730);
		send_key(&keyboard, KEY_2, WL_KEYBOARD_KEY_STATE_PRESSED, 731);
		send_key(&keyboard, KEY_2, WL_KEYBOARD_KEY_STATE_RELEASED, 732);
		send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 733);
		for (unsigned int i = 0; i < 15; i++)
			if (!pump(&client, &server)) goto fail;
		CHECK(tp_tagview_current(&m1->view) == tp_tag_mask(1));
		bool saw_layer_key2 = false;
		for (unsigned int i = 0; i < client.key_events && i < 8; i++) {
			if (client.keys[i] == KEY_2 &&
				client.key_states[i] == WL_KEYBOARD_KEY_STATE_PRESSED)
				saw_layer_key2 = true;
		}
		CHECK(saw_layer_key2);

		zwp_keyboard_shortcuts_inhibitor_v1_destroy(inh_l);
		destroy_layer(&explorer_layer);
		destroy_window(&wa);
		for (unsigned int i = 0; i < 15; i++)
			pump(&client, &server);
		CHECK(!tp_shortcuts_inhibited(&server));
	}

	unlink(lua_cfg);
	rmdir(lua_dir);

	wl_display_disconnect(client.display);
	if (client.shm_data && client.shm_data != MAP_FAILED)
		munmap(client.shm_data, 16);
	if (client.shm_fd >= 0) close(client.shm_fd);
	wlr_pointer_finish(&pointer);
	wlr_keyboard_finish(&keyboard);
	tp_server_finish(&server);
	if (ipc_sock_path[0])
		CHECK(access(ipc_sock_path, F_OK) != 0);
	CHECK_DONE("test_input_headless");

fail:
	if (client.display) wl_display_disconnect(client.display);
	if (client.shm_data && client.shm_data != MAP_FAILED)
		munmap(client.shm_data, 16);
	if (client.shm_fd >= 0) close(client.shm_fd);
	wlr_pointer_finish(&pointer);
	wlr_keyboard_finish(&keyboard);
	tp_server_finish(&server);
	CHECK(false);
	CHECK_DONE("test_input_headless");
}
