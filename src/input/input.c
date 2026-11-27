/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The seat, input-device discovery and the seat's client requests. */
#include "tam-pane/input.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>

const char *tp_input_device_type_name(int type) {
	switch (type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		return "keyboard";
	case WLR_INPUT_DEVICE_POINTER:
		return "pointer";
	case WLR_INPUT_DEVICE_TOUCH:
		return "touch";
	case WLR_INPUT_DEVICE_TABLET:
		return "tablet";
	case WLR_INPUT_DEVICE_TABLET_PAD:
		return "tablet_pad";
	case WLR_INPUT_DEVICE_SWITCH:
		return "switch";
	default:
		return "unknown";
	}
}

static void on_input_device_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct tp_input_device *d = wl_container_of(l, d, destroy);
	wl_list_remove(&d->destroy.link);
	wl_list_remove(&d->link);
	free(d);
}

static void track_input_device(struct tp_server *s,
							   struct wlr_input_device *dev) {
	struct tp_input_device *d = calloc(1, sizeof(*d));
	if (!d)
		return;
	d->wlr = dev;
	d->destroy.notify = on_input_device_destroy;
	wl_signal_add(&dev->events.destroy, &d->destroy);
	wl_list_insert(s->input_devices.prev, &d->link);
}

void tp_input_devices_finish(struct tp_server *s) {
	struct tp_input_device *d, *tmp;
	wl_list_for_each_safe(d, tmp, &s->input_devices, link) {
		wl_list_remove(&d->destroy.link);
		wl_list_remove(&d->link);
		free(d);
	}
}

void tp_input_update_caps(struct tp_server *s) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}

static void on_new_input(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_input);
	struct wlr_input_device *dev = data;

	track_input_device(s, dev);

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
