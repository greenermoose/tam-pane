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
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static const struct wlr_keyboard_impl keyboard_impl = { .name = "test" };
static const struct wlr_pointer_impl pointer_impl = { .name = "test" };

struct test_window {
	struct test_client *client;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	int32_t width, height;
	bool configured, closed;
};

struct test_client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct xdg_wm_base *wm_base;
	struct zwlr_layer_shell_v1 *layer_shell;
	bool has_viewporter, has_fractional_scale;
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
		wl_surface_attach(window->surface, window->client->buffer, 0, 0);
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
	((struct test_client *)data)->pointer_enters++;
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

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct test_client *client = data;
	if (!strcmp(interface, wl_compositor_interface.name))
		client->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, version < 4 ? version : 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, wl_seat_interface.name)) {
		client->seat = wl_registry_bind(registry, name, &wl_seat_interface,
			version < 5 ? version : 5);
		wl_seat_add_listener(client->seat, &seat_listener, client);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		client->wm_base = wl_registry_bind(registry, name,
			&xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(client->wm_base, &wm_listener, client);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		client->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
	} else if (!strcmp(interface, "wp_viewporter")) {
		client->has_viewporter = true;
	} else if (!strcmp(interface, "wp_fractional_scale_manager_v1")) {
		client->has_fractional_scale = true;
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
		int32_t x, int32_t y, int32_t w, int32_t h) {}
static void popup_done(void *data, struct xdg_popup *popup) {}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
};

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

static void configure_window_app(struct test_client *client,
		struct test_window *window, const char *app_id, const char *title) {
	*window = (struct test_window){ .client = client };
	window->surface = wl_compositor_create_surface(client->compositor);
	window->xdg_surface = xdg_wm_base_get_xdg_surface(client->wm_base,
		window->surface);
	xdg_surface_add_listener(window->xdg_surface, &surface_listener, window);
	window->toplevel = xdg_surface_get_toplevel(window->xdg_surface);
	xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
	xdg_toplevel_set_title(window->toplevel, title);
	xdg_toplevel_set_app_id(window->toplevel, app_id);
	wl_surface_commit(window->surface);
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

	/* EXCLUSIVE layer surface grabs keyboard focus on map, restores on destroy. */
	struct test_layer launcher;
	create_layer(&client, &launcher, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
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
