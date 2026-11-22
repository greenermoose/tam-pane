/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The cursor: motion, buttons, scrolling, and pointer focus. Keyboard
 * focus follows clicks; focus-follows-mouse becomes a Lua setting. */
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/idle.h"
#include "tam-pane/input.h"
#include "tam-pane/layer.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/session_lock.h"
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <tam.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>

static const char *resize_cursor_name(uint32_t edges) {
	if ((edges & WLR_EDGE_TOP) && (edges & WLR_EDGE_LEFT))
		return "nw-resize";
	if ((edges & WLR_EDGE_TOP) && (edges & WLR_EDGE_RIGHT))
		return "ne-resize";
	if ((edges & WLR_EDGE_BOTTOM) && (edges & WLR_EDGE_LEFT))
		return "sw-resize";
	return "se-resize";
}

void tp_pointer_refocus_now(struct tp_server *s) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	tp_pointer_refocus(s, (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000));
}

void tp_pointer_refocus(struct tp_server *s, uint32_t time_msec) {
	if (s->grab.mode != TP_GRAB_NONE)
		return;
	struct wlr_surface *surface = NULL;
	double sx = 0, sy = 0;
	tp_client_at(s, s->cursor->x, s->cursor->y, &surface, &sx, &sy);
	if (!surface) {
		s->cursor_shape_name = "default";
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
		wlr_seat_pointer_clear_focus(s->seat);
		return;
	}
	wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(s->seat, time_msec, sx, sy);
}

void tp_pointer_cancel_grab(struct tp_server *s, struct tp_client *c) {
	if (s->grab.mode == TP_GRAB_NONE)
		return;
	if (c && s->grab.client != c)
		return;
	s->grab = (struct tp_pointer_grab){0};
	if (s->cursor && s->cursor_mgr) {
		s->cursor_shape_name = "default";
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
	}
	if (s->stopping || !s->cursor || !s->seat)
		return;
	tp_pointer_refocus_now(s);
}

static void update_grab(struct tp_server *s) {
	struct tp_pointer_grab *g = &s->grab;
	struct tp_client *c = g->client;
	if (!c || !c->mapped) {
		tp_pointer_cancel_grab(s, NULL);
		return;
	}

	double dx = s->cursor->x - g->start_x;
	double dy = s->cursor->y - g->start_y;

	if (g->mode == TP_GRAB_MOVE) {
		c->float_geom.x = g->start_geom.x + (int32_t)dx;
		c->float_geom.y = g->start_geom.y + (int32_t)dy;
		struct tp_monitor *m = tp_monitor_at(s, s->cursor->x, s->cursor->y);
		if (m && m != c->mon) {
			/* The window follows the pointer, so its place is not carried
			 * over; it takes the new monitor's view and focus slot. */
			struct tp_monitor *old = c->mon;
			tp_client_attach(c, m, tp_tagview_current(&m->view));
			tp_client_clear_home(c);
			m->focus[tp_tags_slot(tp_tagview_current(&m->view))] = c;
			tp_client_place(c, c->float_geom);
			if (old)
				tp_arrange(old);
			tp_arrange(m);
			return;
		}
		tp_client_place(c, c->float_geom);
		return;
	}

	if (g->tiled) {
		if (!c->mon) {
			tp_pointer_cancel_grab(s, NULL);
			return;
		}
		struct tp_dwindle *tree = tp_monitor_tree(c->mon);
		if (!tp_dwindle_drag_valid(&g->drag, tree)) {
			/* The tree freed a node since the drag began (another window
			 * left it), so the splits the drag held may be gone. Carry on
			 * from here against the tree as it is now. */
			if (!tp_dwindle_drag_begin(tree, c, s->cursor->x, s->cursor->y,
									   &g->drag)) {
				tp_pointer_cancel_grab(s, NULL);
				return;
			}
			g->start_x = s->cursor->x;
			g->start_y = s->cursor->y;
			return;
		}
		if (tp_dwindle_drag_update(&g->drag, dx, dy))
			tp_arrange(c->mon);
		return;
	}

	struct tp_rect r = g->start_geom;
	int32_t ix = (int32_t)dx, iy = (int32_t)dy;
	if (g->edges & WLR_EDGE_LEFT) {
		int32_t right = r.x + r.w;
		r.w -= ix;
		if (r.w < TP_FLOAT_MIN_W)
			r.w = TP_FLOAT_MIN_W;
		r.x = right - r.w;
	} else if (g->edges & WLR_EDGE_RIGHT) {
		r.w += ix;
		if (r.w < TP_FLOAT_MIN_W)
			r.w = TP_FLOAT_MIN_W;
	}
	if (g->edges & WLR_EDGE_TOP) {
		int32_t bottom = r.y + r.h;
		r.h -= iy;
		if (r.h < TP_FLOAT_MIN_H)
			r.h = TP_FLOAT_MIN_H;
		r.y = bottom - r.h;
	} else if (g->edges & WLR_EDGE_BOTTOM) {
		r.h += iy;
		if (r.h < TP_FLOAT_MIN_H)
			r.h = TP_FLOAT_MIN_H;
	}
	c->float_geom = r;
	tp_client_place(c, c->float_geom);
}

static void after_motion(struct tp_server *s, uint32_t time_msec) {
	struct tp_monitor *m = tp_monitor_at(s, s->cursor->x, s->cursor->y);
	if (m)
		s->selmon = m;
	if (s->grab.mode != TP_GRAB_NONE) {
		update_grab(s);
		return;
	}
	tp_pointer_refocus(s, time_msec);
}

static void on_motion(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_motion);
	struct wlr_pointer_motion_event *ev = data;
	tp_idle_notify_activity(s);
	wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	after_motion(s, ev->time_msec);
}

static void on_motion_absolute(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *ev = data;
	tp_idle_notify_activity(s);
	wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
	after_motion(s, ev->time_msec);
}

static bool try_begin_grab(struct tp_server *s, struct tp_client *c,
						   uint32_t button) {
	if (!c || c->fullscreen)
		return false;

	if (button == BTN_LEFT && c->floating) {
		s->grab = (struct tp_pointer_grab){
			.mode = TP_GRAB_MOVE,
			.client = c,
			.button = button,
			.start_x = s->cursor->x,
			.start_y = s->cursor->y,
			.start_geom = c->float_geom,
		};
		wlr_seat_pointer_clear_focus(s->seat);
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "grabbing");
		return true;
	}

	if (button == BTN_RIGHT && c->floating) {
		double cx = c->float_geom.x + c->float_geom.w / 2.0;
		double cy = c->float_geom.y + c->float_geom.h / 2.0;
		uint32_t edges = (s->cursor->x < cx ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
						 (s->cursor->y < cy ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
		s->grab = (struct tp_pointer_grab){
			.mode = TP_GRAB_RESIZE,
			.client = c,
			.button = button,
			.start_x = s->cursor->x,
			.start_y = s->cursor->y,
			.start_geom = c->float_geom,
			.edges = edges,
			.tiled = false,
		};
		wlr_seat_pointer_clear_focus(s->seat);
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr,
							   resize_cursor_name(edges));
		return true;
	}

	if (button == BTN_RIGHT && !c->floating && c->mon) {
		struct tp_dwindle_drag drag;
		if (!tp_dwindle_drag_begin(tp_monitor_tree(c->mon), c, s->cursor->x,
								   s->cursor->y, &drag))
			return false;
		s->grab = (struct tp_pointer_grab){
			.mode = TP_GRAB_RESIZE,
			.client = c,
			.button = button,
			.start_x = s->cursor->x,
			.start_y = s->cursor->y,
			.tiled = true,
			.drag = drag,
		};
		wlr_seat_pointer_clear_focus(s->seat);
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "grabbing");
		return true;
	}

	return false;
}

static void on_button(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_button);
	struct wlr_pointer_button_event *ev = data;
	tp_idle_notify_activity(s);

	if (s->locked) {
		if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			struct wlr_surface *surface = NULL;
			tp_client_at(s, s->cursor->x, s->cursor->y, &surface, NULL, NULL);
			if (surface)
				tp_session_lock_focus_surface(s, surface);
		}
		wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button,
									   ev->state);
		return;
	}

	if (s->grab.mode != TP_GRAB_NONE) {
		if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED &&
			ev->button == s->grab.button) {
			s->grab = (struct tp_pointer_grab){0};
			wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
			tp_pointer_refocus(s, ev->time_msec);
		}
		return;
	}

	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct wlr_surface *surface = NULL;
		struct tp_client *c =
			tp_client_at(s, s->cursor->x, s->cursor->y, &surface, NULL, NULL);
		if (c) {
			if (c != tp_focused(s))
				tp_focus(s, c);
		} else if (surface) {
			struct tp_layer_surface *ls = tp_layer_from_surface(surface);
			if (ls && ls->layer_surface->current.keyboard_interactive ==
						  ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)
				tp_focus_layer(s, ls);
		}

		struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
		uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
		if ((mods & WLR_MODIFIER_LOGO) && try_begin_grab(s, c, ev->button))
			return;
	}
	wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button,
								   ev->state);
}

static void on_axis(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_axis);
	struct wlr_pointer_axis_event *ev = data;
	tp_idle_notify_activity(s);
	wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation,
								 ev->delta, ev->delta_discrete, ev->source,
								 ev->relative_direction);
}

static void on_frame(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, cursor_frame);
	wlr_seat_pointer_notify_frame(s->seat);
}

static void on_request_cursor_shape(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, request_cursor_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	if (ev->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER)
		return;
	if (s->grab.mode != TP_GRAB_NONE)
		return;
	if (ev->seat_client != s->seat->pointer_state.focused_client)
		return;
	const char *name = wlr_cursor_shape_v1_name(ev->shape);
	if (name) {
		s->cursor_shape_name = name;
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, name);
	}
}

bool tp_pointer_init(struct tp_server *s) {
	s->cursor = wlr_cursor_create();
	s->cursor_mgr = wlr_xcursor_manager_create(
		tp_settings_cursor_theme(&s->settings), s->settings.cursor_size);
	if (!s->cursor || !s->cursor_mgr) {
		tam_log_error("cannot create the cursor");
		return false;
	}
	wlr_cursor_attach_output_layout(s->cursor, s->output_layout);

	s->cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(s->display, 1);
	if (!s->cursor_shape_mgr) {
		tam_log_error("cannot create wp_cursor_shape_manager_v1");
		return false;
	}
	s->request_cursor_shape.notify = on_request_cursor_shape;
	wl_signal_add(&s->cursor_shape_mgr->events.request_set_shape,
				  &s->request_cursor_shape);

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

void tp_pointer_apply_settings(struct tp_server *s) {
	if (!s || !s->cursor)
		return;
	struct wlr_xcursor_manager *mgr = wlr_xcursor_manager_create(
		tp_settings_cursor_theme(&s->settings), s->settings.cursor_size);
	if (!mgr) {
		tam_log_warn("reload: cannot create xcursor manager; keeping current cursor");
		return;
	}
	if (s->cursor_mgr)
		wlr_xcursor_manager_destroy(s->cursor_mgr);
	s->cursor_mgr = mgr;
	if (s->grab.mode == TP_GRAB_NONE)
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
}

void tp_pointer_finish(struct tp_server *s) {
	struct wl_listener *ls[] = {&s->request_cursor_shape, &s->cursor_motion,
								&s->cursor_motion_absolute, &s->cursor_button,
								&s->cursor_axis, &s->cursor_frame};
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
