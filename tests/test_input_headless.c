/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise input delivery to real Wayland clients on a private compositor. */
#define _GNU_SOURCE
#include "check.h"
#include "tam-pane/client.h"
#include "tam-pane/input.h"
#include "tam-pane/focus.h"
#include "tam-pane/layer.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/security.h"
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_scene.h>
#include "tam-pane/server.h"
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
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
	struct zwlr_output_power_manager_v1 *output_power_mgr;
	struct zwlr_screencopy_manager_v1 *screencopy_mgr;
	struct ext_image_copy_capture_manager_v1 *image_copy_mgr;
	struct ext_output_image_capture_source_manager_v1 *output_src_mgr;
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_src_mgr;
	struct zwlr_output_manager_v1 *output_mgr;
	struct zwlr_gamma_control_manager_v1 *gamma_mgr;
	struct wp_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct zxdg_output_manager_v1 *xdg_output_mgr;
	uint32_t layer_shell_name;
	uint32_t lock_mgr_name;
	uint32_t screencopy_mgr_name;
	uint32_t output_mgr_name;
	uint32_t pointer_enter_serial;
	unsigned int unclassified_globals;
	bool has_viewporter, has_fractional_scale, has_presentation;
	bool has_wlr_foreign_toplevel, has_ext_foreign_toplevel;
	bool has_ext_workspace, has_ext_session_lock;
	bool has_idle_notify, has_idle_inhibit, has_output_power;
	bool has_screencopy, has_image_copy, has_output_capture_src, has_toplevel_capture_src;
	bool has_output_mgmt, has_gamma_control, has_cursor_shape, has_xdg_output;
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
	((struct test_client *)data)->keyboard_focus = surface;
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
	}
	client->key_events++;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t depressed, uint32_t latched,
		uint32_t locked, uint32_t group) {}

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
	struct tp_keyboard *k = wl_container_of(s->keyboards.next, k, link);
	return (k->consumed_keys[code / 64] & (UINT64_C(1) << (code % 64))) != 0;
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
	CHECK(c0->geom.w > w0_before);
	chord_mod(&keyboard, KEY_LEFT, false, false, true);
	CHECK(c0->geom.w == w0_before);

	int32_t h1_before = c1->geom.h;
	tp_focus(&server, c1);
	chord_mod(&keyboard, KEY_DOWN, false, false, true);
	CHECK(c1->geom.h > h1_before);
	chord_mod(&keyboard, KEY_UP, false, false, true);
	CHECK(c1->geom.h == h1_before);

	/* Directional swap: Meta+Shift+Down swaps c1 and c2. */
	struct tp_rect g1 = c1->geom, g2 = c2->geom;
	tp_focus(&server, c1);
	chord_mod(&keyboard, KEY_DOWN, true, false, false);
	CHECK(c1->geom.y == g2.y && c2->geom.y == g1.y);
	CHECK(tp_focused(&server) == c1);
	chord_mod(&keyboard, KEY_UP, true, false, false);
	CHECK(c1->geom.y == g1.y && c2->geom.y == g2.y);

	/* Directional move: Meta+Ctrl+Right moves c0 into c1's slot -> [[c0|c1]/c2]. */
	tp_focus(&server, c0);
	chord_mod(&keyboard, KEY_RIGHT, false, true, false);
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
	CHECK(c0->geom.w > w0_before);
	emit_button(&server, &pointer, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED, 104);
	send_key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED, 105);
	CHECK(server.grab.mode == TP_GRAB_NONE);

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
	struct tp_dwindle_node *xn = tp_dwindle_find(tp_monitor_tree(cx->mon), cx);
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
	CHECK(server.grab.drag.h_node == xn->parent ||
		server.grab.drag.v_node == xn->parent);
	destroy_window(&client.windows[2]);
	for (unsigned int i = 0; i < 100 && wl_list_length(&server.clients) == 3; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(wl_list_length(&server.clients) == 2);
	w0_before = c0->geom.w;
	emit_motion(&server, &pointer, 60.0, 0.0, 143);
	emit_motion(&server, &pointer, 60.0, 0.0, 144);
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
	struct tp_dwindle_node *nn = tp_dwindle_find(tp_monitor_tree(cn->mon), cn);
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
	CHECK(!tp_dwindle_find(tp_monitor_tree(cf->mon), cf));

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
	CHECK(tp_dwindle_find(tp_monitor_tree(cf->mon), cf) != NULL);
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

	/* Exclusive OVERLAY keyboard focus is reserved for the configured shell
	 * connection (0.1.18, R1 #9): an ordinary client's exclusive OVERLAY
	 * layer surface does not steal keyboard focus, while the designated
	 * shell client's does. */
	struct test_layer overlay_excl;
	create_layer(&client, &overlay_excl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
		200, 100, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	for (unsigned int i = 0; i < 200 && !overlay_excl.configured; i++)
		if (!pump(&client, &server)) goto fail;
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.focused_layer == NULL);
	CHECK(tp_focused(&server) == c0);
	tp_security_set_shell_client(&server,
		wl_resource_get_client(c0->toplevel->resource));
	tp_focus_refresh(&server);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(server.focused_layer != NULL);
	CHECK(client.keyboard_focus == overlay_excl.surface);
	CHECK(tp_focused(&server) == NULL);
	destroy_layer(&overlay_excl);
	tp_security_set_shell_client(&server, NULL);
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
	CHECK(c1->mon == m2 && c0->mon == m1);
	CHECK(c1->geom.x >= m2->usable.x);
	chord(&keyboard, KEY_DOT, false, false);
	CHECK(server.selmon == m2 && tp_focused(&server) == c1);

	/* Dragging a floating window onto another monitor moves it there in
	 * full: monitor, tags, focus slots and layout trees (R1 findings 6 and
	 * 13). Then Meta+Shift+Comma brings it back and it tiles again. */
	tp_focus(&server, c0);
	chord(&keyboard, KEY_V, false, false);
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
		CHECK(!tp_dwindle_find(&m1->trees[i], c0));
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
	CHECK(!c0->floating && c0->mon == m1);
	CHECK(tp_dwindle_find(tp_monitor_tree(m1), c0) != NULL);
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

	/* When a monitor with the remembered name returns, c1 moves back. */
	free(c1->home_output);
	c1->home_output = strdup("HEADLESS-3");
	struct wlr_output *o3 = wlr_headless_add_output(headless, 800, 600);
	CHECK(o3 != NULL && strcmp(o3->name, "HEADLESS-3") == 0);
	for (unsigned int i = 0; i < 20; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(c1->mon == o3->data);
	CHECK(c1->home_output == NULL);
	CHECK(c1->tags == saved_tags);

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
	hook_event_count = 0;
	wlr_output_destroy(o4);
	for (unsigned int i = 0; i < 10; i++)
		if (!pump(&client, &server)) goto fail;
	CHECK(has_hook_event("mrem:HEADLESS-4:640x480"));

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

	CHECK(server.settings.gap_inner == 20 && server.settings.gap_outer == 34);
	CHECK(c_live->geom.w == mon_primary->usable.w - 68);
	CHECK(c_live->geom.w != orig_w);
	CHECK(mon_primary->trees[1].preserve_split == true);
	CHECK(mon_primary->trees[1].proportional_split == true);
	CHECK(mon_primary->trees[1].width_bias > 1.49f &&
		  mon_primary->trees[1].width_bias < 1.51f);
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
	CHECK(mon_primary->trees[1].preserve_split == false);
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

		/* Unlock restores layers and normal bindings. */
		ext_session_lock_surface_v1_destroy(lsurf);
		wl_surface_destroy(ls_wl);
		ext_session_lock_v1_unlock_and_destroy(lock1);
		for (unsigned int i = 0; i < 20; i++)
			if (!pump(&lk1, &server)) break;
		CHECK(!server.locked);
		CHECK(server.layer[TP_LAYER_TILE]->node.enabled);

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

	unlink(lua_cfg);
	rmdir(lua_dir);

	wl_display_disconnect(client.display);
	if (client.shm_data && client.shm_data != MAP_FAILED)
		munmap(client.shm_data, 16);
	if (client.shm_fd >= 0) close(client.shm_fd);
	wlr_pointer_finish(&pointer);
	wlr_keyboard_finish(&keyboard);
	tp_server_finish(&server);
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
