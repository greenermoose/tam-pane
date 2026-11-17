/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_PANE_INPUT_H
#define TAM_PANE_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

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
};

/* Seat, device discovery and seat requests (input.c). */
bool tp_input_init(struct tp_server *s);
void tp_input_finish(struct tp_server *s);
void tp_input_update_caps(struct tp_server *s);

/* Keyboards (keyboard.c). */
void tp_keyboard_add(struct tp_server *s, struct wlr_input_device *dev);
void tp_keyboards_finish(struct tp_server *s);

/* Cursor and pointer devices (pointer.c). */
bool tp_pointer_init(struct tp_server *s);
void tp_pointer_finish(struct tp_server *s);
void tp_pointer_add(struct tp_server *s, struct wlr_input_device *dev);

/* Re-send pointer focus after windows move under a still cursor. */
void tp_pointer_refocus(struct tp_server *s, uint32_t time_msec);

#endif
