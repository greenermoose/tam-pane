/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The cursor: motion, buttons, scrolling, and pointer focus. Keyboard
 * focus follows clicks; focus-follows-mouse becomes a Lua setting. */
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/input.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <tam.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

void tp_pointer_refocus(struct tp_server *s, uint32_t time_msec) {
	struct wlr_surface *surface = NULL;
	double sx = 0, sy = 0;
	tp_client_at(s, s->cursor->x, s->cursor->y, &surface, &sx, &sy);
	if (!surface) {
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
		wlr_seat_pointer_clear_focus(s->seat);
		return;
	}
	wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(s->seat, time_msec, sx, sy);
}

static void after_motion(struct tp_server *s, uint32_t time_msec) {
	struct tp_monitor *m = tp_monitor_at(s, s->cursor->x, s->cursor->y);
	if (m)
		s->selmon = m;
	tp_pointer_refocus(s, time_msec);
}

static void on_motion(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_motion);
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	after_motion(s, ev->time_msec);
}

static void on_motion_absolute(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
	after_motion(s, ev->time_msec);
}

static void on_button(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_button);
	struct wlr_pointer_button_event *ev = data;
	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct tp_client *c =
			tp_client_at(s, s->cursor->x, s->cursor->y, NULL, NULL, NULL);
		if (c && c != tp_focused(s))
			tp_focus(s, c);
	}
	wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button,
								   ev->state);
}

static void on_axis(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_axis);
	struct wlr_pointer_axis_event *ev = data;
	wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation,
								 ev->delta, ev->delta_discrete, ev->source,
								 ev->relative_direction);
}

static void on_frame(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_frame);
	wlr_seat_pointer_notify_frame(s->seat);
}

bool tp_pointer_init(struct tp_server *s) {
	s->cursor = wlr_cursor_create();
	s->cursor_mgr = wlr_xcursor_manager_create(s->settings.cursor_theme,
											   s->settings.cursor_size);
	if (!s->cursor || !s->cursor_mgr) {
		tam_log_error("cannot create the cursor");
		return false;
	}
	wlr_cursor_attach_output_layout(s->cursor, s->output_layout);

	s->cursor_motion.notify = on_motion;
	wl_signal_add(&s->cursor->events.motion, &s->cursor_motion);
	s->cursor_motion_absolute.notify = on_motion_absolute;
	wl_signal_add(&s->cursor->events.motion_absolute,
				  &s->cursor_motion_absolute);
	s->cursor_button.notify = on_button;
	wl_signal_add(&s->cursor->events.button, &s->cursor_button);
	s->cursor_axis.notify = on_axis;
	wl_signal_add(&s->cursor->events.axis, &s->cursor_axis);
	s->cursor_frame.notify = on_frame;
	wl_signal_add(&s->cursor->events.frame, &s->cursor_frame);
	return true;
}

void tp_pointer_add(struct tp_server *s, struct wlr_input_device *dev) {
	wlr_cursor_attach_input_device(s->cursor, dev);
}

void tp_pointer_finish(struct tp_server *s) {
	struct wl_listener *ls[] = {&s->cursor_motion, &s->cursor_motion_absolute,
								&s->cursor_button, &s->cursor_axis,
								&s->cursor_frame};
	for (size_t i = 0; i < sizeof(ls) / sizeof(ls[0]); i++) {
		if (ls[i]->notify)
			wl_list_remove(&ls[i]->link);
		ls[i]->notify = NULL;
	}
	if (s->cursor_mgr)
		wlr_xcursor_manager_destroy(s->cursor_mgr);
	if (s->cursor)
		wlr_cursor_destroy(s->cursor);
	s->cursor_mgr = NULL;
	s->cursor = NULL;
}
