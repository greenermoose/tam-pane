/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise input delivery to real Wayland clients on a private compositor. */
#define _GNU_SOURCE
#include "check.h"
#include "tam-pane/client.h"
#include "tam-pane/input.h"
#include "tam-pane/focus.h"
#include "tam-pane/monitor.h"
#include <wlr/types/wlr_scene.h>
#include "tam-pane/server.h"
#include <poll.h>
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
		client->keyboard && client->pointer &&
		client->has_viewporter && client->has_fractional_scale;
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

static void configure_window(struct test_client *client,
		struct test_window *window, const char *title) {
	*window = (struct test_window){ .client = client };
	window->surface = wl_compositor_create_surface(client->compositor);
	window->xdg_surface = xdg_wm_base_get_xdg_surface(client->wm_base,
		window->surface);
	xdg_surface_add_listener(window->xdg_surface, &surface_listener, window);
	window->toplevel = xdg_surface_get_toplevel(window->xdg_surface);
	xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window);
	xdg_toplevel_set_title(window->toplevel, title);
	xdg_toplevel_set_app_id(window->toplevel, "tam-pane-test");
	wl_surface_commit(window->surface);
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
