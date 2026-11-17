/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise input setup against a private headless compositor. */
#include "check.h"
#include "tam-pane/input.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>

static const struct wlr_keyboard_impl keyboard_impl = { .name = "test" };
static const struct wlr_pointer_impl pointer_impl = { .name = "test" };

int main(void) {
	struct tp_server server = {0};
	tp_settings_defaults(&server.settings);
	if (!tp_server_init(&server) || !tp_server_start(&server)) {
		fprintf(stderr, "headless input: compositor setup failed\n");
		tp_server_finish(&server);
		return 1;
	}
	struct wlr_keyboard keyboard;
	wlr_keyboard_init(&keyboard, &keyboard_impl, "test keyboard");
	tp_keyboard_add(&server, &keyboard.base);
	tp_input_update_caps(&server);
	CHECK(wlr_seat_get_keyboard(server.seat) == &keyboard);
	CHECK(keyboard.repeat_info.rate == server.settings.repeat_rate);
	CHECK(keyboard.repeat_info.delay == server.settings.repeat_delay);
	CHECK(server.seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD);
	struct wlr_pointer pointer;
	wlr_pointer_init(&pointer, &pointer_impl, "test pointer");
	tp_pointer_add(&server, &pointer.base);
	CHECK(server.seat->capabilities & WL_SEAT_CAPABILITY_POINTER);
	wlr_pointer_finish(&pointer);
	wlr_keyboard_finish(&keyboard);
	CHECK(!(server.seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD));
	tp_server_finish(&server);
	CHECK_DONE("test_input_headless");
}
