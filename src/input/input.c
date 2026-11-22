/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The seat, input-device discovery and the seat's client requests. */
#include "tam-pane/input.h"
#include "tam-pane/server.h"
#include <tam.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>

void tp_input_update_caps(struct tp_server *s) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}

static void on_new_input(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_input);
	struct wlr_input_device *dev = data;

	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		tp_keyboard_add(s, dev);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		tp_pointer_add(s, dev);
		break;
	default:
		tam_log_debug("input %s: type %d not handled yet", dev->name,
					  dev->type);
		break;
	}
	tp_input_update_caps(s);
}

static void on_request_cursor(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	/* Only the client under the pointer may set its image. */
	if (ev->seat_client == s->seat->pointer_state.focused_client)
		wlr_cursor_set_surface(s->cursor, ev->surface, ev->hotspot_x,
							   ev->hotspot_y);
}

static void on_request_set_selection(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, request_set_selection);
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

static void on_request_set_primary_selection(struct wl_listener *l,
											 void *data) {
	struct tp_server *s =
		wl_container_of(l, s, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(s->seat, ev->source, ev->serial);
}

bool tp_input_init(struct tp_server *s) {
	s->seat = wlr_seat_create(s->display, "seat0");
	if (!s->seat) {
		tam_log_error("cannot create seat0");
		return false;
	}
	s->new_input.notify = on_new_input;
	wl_signal_add(&s->backend->events.new_input, &s->new_input);
	s->request_cursor.notify = on_request_cursor;
	wl_signal_add(&s->seat->events.request_set_cursor, &s->request_cursor);
	s->request_set_selection.notify = on_request_set_selection;
	wl_signal_add(&s->seat->events.request_set_selection,
				  &s->request_set_selection);
	s->request_set_primary_selection.notify = on_request_set_primary_selection;
	wl_signal_add(&s->seat->events.request_set_primary_selection,
				  &s->request_set_primary_selection);
	return true;
}

void tp_input_finish(struct tp_server *s) {
	struct wl_listener *ls[] = {&s->new_input, &s->request_cursor,
								&s->request_set_selection,
								&s->request_set_primary_selection};
	for (size_t i = 0; i < sizeof(ls) / sizeof(ls[0]); i++) {
		if (ls[i]->notify)
			wl_list_remove(&ls[i]->link);
		ls[i]->notify = NULL;
	}
}
