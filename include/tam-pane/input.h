/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_INPUT_H
#define TAM_PANE_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct tp_client;
struct tp_server;
struct wlr_input_device;
struct wlr_keyboard;

struct tp_keyboard {
	struct wl_list link; /* tp_server.keyboards */
	struct tp_server *server;
	struct wlr_keyboard *wlr;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
	/* Linux KEY_MAX fits in twelve 64-bit words. */
	uint64_t consumed_keys[12];
	uint32_t last_layout_group;
};

struct tp_input_device {
	struct wl_list link; /* tp_server.input_devices */
	struct wlr_input_device *wlr;
	struct wl_listener destroy;
};

/* Seat, device discovery and seat requests (input.c). */
bool tp_input_init(struct tp_server *s);
void tp_input_finish(struct tp_server *s);
void tp_input_devices_finish(struct tp_server *s);
void tp_input_update_caps(struct tp_server *s);
const char *tp_input_device_type_name(int type);

/* Keyboards (keyboard.c). */
void tp_keyboard_add(struct tp_server *s, struct wlr_input_device *dev);
void tp_keyboards_finish(struct tp_server *s);
void tp_keyboards_apply_settings(struct tp_server *s);
bool tp_keyboard_next_layout(struct tp_server *s, const char *name);
bool tp_keyboard_send_key(struct tp_server *s, uint32_t mods,
						  uint32_t sym);

/* Cursor and pointer devices (pointer.c). */
bool tp_pointer_init(struct tp_server *s);
void tp_pointer_finish(struct tp_server *s);
void tp_pointer_add(struct tp_server *s, struct wlr_input_device *dev);
void tp_pointer_apply_settings(struct tp_server *s);

/* Re-send pointer focus after windows move under a still cursor. */
void tp_pointer_refocus(struct tp_server *s, uint32_t time_msec);
/* The same, stamped with the current CLOCK_MONOTONIC time. */
void tp_pointer_refocus_now(struct tp_server *s);

/* End an active pointer grab if it belongs to `c` (or any client when NULL). */
void tp_pointer_cancel_grab(struct tp_server *s, struct tp_client *c);

#endif
