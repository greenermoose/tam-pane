/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/text_input.h"
#include "tam-pane/client.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xdg_shell.h>

struct tp_text_input {
	struct wl_list link; /* tp_text_input_relay.text_inputs */
	struct tp_text_input_relay *relay;
	struct wlr_text_input_v3 *wlr;
	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener disable;
	struct wl_listener destroy;
};

struct tp_im_popup {
	struct wl_list link; /* tp_text_input_relay.popups */
	struct tp_text_input_relay *relay;
	struct wlr_input_popup_surface_v2 *wlr;
	struct wlr_scene_tree *tree;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct tp_text_input_relay {
	struct tp_server *server;
	struct wl_list text_inputs; /* tp_text_input.link */
	struct wl_list popups;		/* tp_im_popup.link */
	struct wlr_input_method_v2 *input_method;
	struct tp_text_input *active_text_input;
	struct wlr_surface *focused_surface;

	struct wl_listener new_text_input;
	struct wl_listener new_input_method;
	struct wl_listener focused_surface_destroy;

	struct wl_listener im_commit;
	struct wl_listener im_grab_keyboard;
	struct wl_listener im_new_popup_surface;
	struct wl_listener im_destroy;
	struct wl_listener im_grab_destroy;
};

static struct tp_text_input *
find_active_text_input(struct tp_text_input_relay *relay) {
	if (!relay->input_method || relay->server->locked)
		return NULL;
	struct tp_text_input *ti;
	wl_list_for_each(ti, &relay->text_inputs, link) {
		if (ti->wlr->focused_surface && ti->wlr->current_enabled)
			return ti;
	}
	return NULL;
}

static void update_active_text_input(struct tp_text_input_relay *relay) {
	struct tp_text_input *active = find_active_text_input(relay);
	if (relay->input_method && relay->active_text_input != active) {
		if (active)
			wlr_input_method_v2_send_activate(relay->input_method);
		else
			wlr_input_method_v2_send_deactivate(relay->input_method);
		wlr_input_method_v2_send_done(relay->input_method);
	}
	relay->active_text_input = active;
}

static void update_text_inputs_focus(struct tp_text_input_relay *relay) {
	struct tp_server *s = relay->server;
	struct tp_text_input *ti;
	wl_list_for_each(ti, &relay->text_inputs, link) {
		struct wlr_text_input_v3 *w = ti->wlr;
		struct wlr_surface *target = NULL;
		if (!s->locked && relay->input_method && relay->focused_surface &&
			wl_resource_get_client(w->resource) ==
				wl_resource_get_client(relay->focused_surface->resource)) {
			target = relay->focused_surface;
		}
		if (w->focused_surface == target)
			continue;
		if (w->focused_surface)
			wlr_text_input_v3_send_leave(w);
		if (target)
			wlr_text_input_v3_send_enter(w, target);
	}
}

static void send_state_to_input_method(struct tp_text_input_relay *relay) {
	if (!relay->input_method || !relay->active_text_input ||
		relay->server->locked)
		return;
	struct wlr_input_method_v2 *im = relay->input_method;
	struct wlr_text_input_v3 *w = relay->active_text_input->wlr;

	if (w->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
		wlr_input_method_v2_send_surrounding_text(
			im, w->current.surrounding.text ? w->current.surrounding.text : "",
			w->current.surrounding.cursor, w->current.surrounding.anchor);
	}
	wlr_input_method_v2_send_text_change_cause(im,
											   w->current.text_change_cause);
	if (w->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
		wlr_input_method_v2_send_content_type(im, w->current.content_type.hint,
											  w->current.content_type.purpose);
	}
	wlr_input_method_v2_send_done(im);
}

static void update_popup_position(struct tp_im_popup *popup) {
	struct tp_text_input_relay *relay = popup->relay;
	struct tp_server *s = relay->server;
	struct tp_text_input *ti = relay->active_text_input;

	if (!ti || !relay->focused_surface || !popup->wlr->surface->mapped ||
		s->locked) {
		wlr_scene_node_set_enabled(&popup->tree->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&popup->tree->node, true);

	struct wlr_box cursor_rect = {0};
	struct wlr_xdg_surface *xs =
		wlr_xdg_surface_try_from_wlr_surface(relay->focused_surface);
	if ((ti->wlr->current.features &
		 WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) &&
		xs && xs->data) {
		struct wlr_scene_tree *tree = xs->data;
		int lx = 0, ly = 0;
		if (wlr_scene_node_coords(&tree->node, &lx, &ly)) {
			cursor_rect = ti->wlr->current.cursor_rectangle;
			cursor_rect.x += lx - xs->geometry.x;
			cursor_rect.y += ly - xs->geometry.y;
		}
	}

	struct wlr_output *out = wlr_output_layout_output_at(
		s->output_layout, cursor_rect.x, cursor_rect.y);
	if (!out && s->selmon)
		out = s->selmon->output;
	if (!out)
		return;

	struct wlr_box output_box = {0};
	wlr_output_layout_get_box(s->output_layout, out, &output_box);

	struct wlr_xdg_positioner_rules rules = {
		.anchor_rect = cursor_rect,
		.anchor = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT,
		.gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
		.size = {
			.width = popup->wlr->surface->current.width,
			.height = popup->wlr->surface->current.height,
		},
		.constraint_adjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
								 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X,
	};
	struct wlr_box popup_box = {0};
	wlr_xdg_positioner_rules_get_geometry(&rules, &popup_box);
	wlr_xdg_positioner_rules_unconstrain_box(&rules, &output_box, &popup_box);

	wlr_scene_node_set_position(&popup->tree->node, popup_box.x, popup_box.y);
	wlr_scene_node_raise_to_top(&popup->tree->node);

	struct wlr_box rel = {
		.x = cursor_rect.x - popup_box.x,
		.y = cursor_rect.y - popup_box.y,
		.width = cursor_rect.width,
		.height = cursor_rect.height,
	};
	wlr_input_popup_surface_v2_send_text_input_rectangle(popup->wlr, &rel);
}

static void update_popups(struct tp_text_input_relay *relay) {
	struct tp_im_popup *p;
	wl_list_for_each(p, &relay->popups, link) update_popup_position(p);
}

/* ---- input method popup ---- */

static void on_popup_commit(struct wl_listener *l, void *data) {
	struct tp_im_popup *p = wl_container_of(l, p, commit);
	update_popup_position(p);
}

static void on_popup_destroy(struct wl_listener *l, void *data) {
	struct tp_im_popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	if (p->tree)
		wlr_scene_node_destroy(&p->tree->node);
	free(p);
}

static void on_im_new_popup_surface(struct wl_listener *l, void *data) {
	struct tp_text_input_relay *relay =
		wl_container_of(l, relay, im_new_popup_surface);
	struct wlr_input_popup_surface_v2 *ps = data;
	struct tp_im_popup *p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->relay = relay;
	p->wlr = ps;
	p->tree = wlr_scene_subsurface_tree_create(
		relay->server->layer[TP_LAYER_OVERLAY], ps->surface);
	if (!p->tree) {
		free(p);
		return;
	}
	wlr_scene_node_set_enabled(&p->tree->node, false);
	p->commit.notify = on_popup_commit;
	wl_signal_add(&ps->surface->events.commit, &p->commit);
	p->destroy.notify = on_popup_destroy;
	wl_signal_add(&ps->events.destroy, &p->destroy);
	wl_list_insert(&relay->popups, &p->link);
	update_popup_position(p);
}

/* ---- input method v2 ---- */

static void on_im_commit(struct wl_listener *l, void *data) {
	struct tp_text_input_relay *relay = wl_container_of(l, relay, im_commit);
	if (!relay->input_method || !relay->active_text_input ||
		relay->server->locked)
		return;
	struct wlr_input_method_v2 *im = relay->input_method;
	struct wlr_text_input_v3 *w = relay->active_text_input->wlr;

	if (im->current.preedit.text) {
		wlr_text_input_v3_send_preedit_string(
			w, im->current.preedit.text, im->current.preedit.cursor_begin,
			im->current.preedit.cursor_end);
	}
	if (im->current.commit_text)
		wlr_text_input_v3_send_commit_string(w, im->current.commit_text);
	if (im->current.delete.before_length || im->current.delete.after_length) {
		wlr_text_input_v3_send_delete_surrounding_text(
			w, im->current.delete.before_length,
			im->current.delete.after_length);
	}
	wlr_text_input_v3_send_done(w);
}

static void on_im_grab_destroy(struct wl_listener *l, void *data) {
	struct tp_text_input_relay *relay =
		wl_container_of(l, relay, im_grab_destroy);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	wl_list_remove(&relay->im_grab_destroy.link);
	relay->im_grab_destroy.notify = NULL;
	if (!relay->server->stopping && !relay->server->locked && grab &&
		grab->keyboard) {
		wlr_seat_keyboard_notify_modifiers(relay->server->seat,
										   &grab->keyboard->modifiers);
	}
}

static void on_im_grab_keyboard(struct wl_listener *l, void *data) {
	struct tp_text_input_relay *relay =
		wl_container_of(l, relay, im_grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(relay->server->seat);
	if (kb)
		wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb);
	if (relay->im_grab_destroy.notify)
		wl_list_remove(&relay->im_grab_destroy.link);
	relay->im_grab_destroy.notify = on_im_grab_destroy;
	wl_signal_add(&grab->events.destroy, &relay->im_grab_destroy);
}

static void detach_im_listeners(struct tp_text_input_relay *relay) {
	struct wl_listener *ls[] = {
		&relay->im_commit, &relay->im_grab_keyboard,
		&relay->im_new_popup_surface, &relay->im_destroy,
		&relay->im_grab_destroy,
	};
	for (size_t i = 0; i < sizeof(ls) / sizeof(ls[0]); i++) {
		if (ls[i]->notify) {
			wl_list_remove(&ls[i]->link);
			ls[i]->notify = NULL;
		}
	}
}

static void on_im_destroy(struct wl_listener *l, void *data) {
	struct tp_text_input_relay *relay = wl_container_of(l, relay, im_destroy);
	detach_im_listeners(relay);
	relay->input_method = NULL;
	relay->active_text_input = NULL;
	if (!relay->server->stopping) {
		update_text_inputs_focus(relay);
		update_active_text_input(relay);
	}
}

static void on_new_input_method(struct wl_listener *l, void *data) {
	struct tp_text_input_relay *relay =
		wl_container_of(l, relay, new_input_method);
	struct wlr_input_method_v2 *im = data;
	if (im->seat != relay->server->seat)
		return;
	if (relay->input_method) {
		wlr_input_method_v2_send_unavailable(im);
		return;
	}
	relay->input_method = im;
	relay->im_commit.notify = on_im_commit;
	wl_signal_add(&im->events.commit, &relay->im_commit);
	relay->im_grab_keyboard.notify = on_im_grab_keyboard;
	wl_signal_add(&im->events.grab_keyboard, &relay->im_grab_keyboard);
	relay->im_new_popup_surface.notify = on_im_new_popup_surface;
	wl_signal_add(&im->events.new_popup_surface, &relay->im_new_popup_surface);
	relay->im_destroy.notify = on_im_destroy;
	wl_signal_add(&im->events.destroy, &relay->im_destroy);

	update_text_inputs_focus(relay);
	update_active_text_input(relay);
}

/* ---- text input v3 ---- */

static void on_ti_enable(struct wl_listener *l, void *data) {
	struct tp_text_input *ti = wl_container_of(l, ti, enable);
	struct tp_text_input_relay *relay = ti->relay;
	update_active_text_input(relay);
	if (relay->active_text_input == ti) {
		update_popups(relay);
		send_state_to_input_method(relay);
	}
	wlr_text_input_v3_send_done(ti->wlr);
}

static void on_ti_disable(struct wl_listener *l, void *data) {
	struct tp_text_input *ti = wl_container_of(l, ti, disable);
	update_active_text_input(ti->relay);
	update_popups(ti->relay);
}

static void on_ti_commit(struct wl_listener *l, void *data) {
	struct tp_text_input *ti = wl_container_of(l, ti, commit);
	struct tp_text_input_relay *relay = ti->relay;
	if (relay->active_text_input == ti) {
		update_popups(relay);
		send_state_to_input_method(relay);
	}
}

static void on_ti_destroy(struct wl_listener *l, void *data) {
	struct tp_text_input *ti = wl_container_of(l, ti, destroy);
	struct tp_text_input_relay *relay = ti->relay;
	wl_list_remove(&ti->enable.link);
	wl_list_remove(&ti->commit.link);
	wl_list_remove(&ti->disable.link);
	wl_list_remove(&ti->destroy.link);
	wl_list_remove(&ti->link);
	if (relay->active_text_input == ti) {
		if (relay->input_method && !relay->server->stopping) {
			wlr_input_method_v2_send_deactivate(relay->input_method);
			wlr_input_method_v2_send_done(relay->input_method);
		}
		relay->active_text_input = NULL;
	}
	if (!relay->server->stopping)
		update_active_text_input(relay);
	free(ti);
}

static void on_new_text_input(struct wl_listener *l, void *data) {
	struct tp_text_input_relay *relay =
		wl_container_of(l, relay, new_text_input);
	struct wlr_text_input_v3 *w = data;
	if (w->seat != relay->server->seat)
		return;
	struct tp_text_input *ti = calloc(1, sizeof(*ti));
	if (!ti)
		return;
	ti->relay = relay;
	ti->wlr = w;
	wl_list_insert(&relay->text_inputs, &ti->link);

	ti->enable.notify = on_ti_enable;
	wl_signal_add(&w->events.enable, &ti->enable);
	ti->commit.notify = on_ti_commit;
	wl_signal_add(&w->events.commit, &ti->commit);
	ti->disable.notify = on_ti_disable;
	wl_signal_add(&w->events.disable, &ti->disable);
	ti->destroy.notify = on_ti_destroy;
	wl_signal_add(&w->events.destroy, &ti->destroy);

	update_text_inputs_focus(relay);
}

static void on_focused_surface_destroy(struct wl_listener *l, void *data) {
	struct tp_text_input_relay *relay =
		wl_container_of(l, relay, focused_surface_destroy);
	wl_list_remove(&relay->focused_surface_destroy.link);
	relay->focused_surface_destroy.notify = NULL;
	relay->focused_surface = NULL;
	if (!relay->server->stopping) {
		update_text_inputs_focus(relay);
		update_active_text_input(relay);
		update_popups(relay);
	}
}

/* ---- public API ---- */

bool tp_text_input_init(struct tp_server *s) {
	s->text_input_mgr = wlr_text_input_manager_v3_create(s->display);
	if (!s->text_input_mgr) {
		tam_log_error("cannot create zwp_text_input_manager_v3");
		return false;
	}
	s->input_method_mgr = wlr_input_method_manager_v2_create(s->display);
	if (!s->input_method_mgr) {
		tam_log_error("cannot create zwp_input_method_manager_v2");
		return false;
	}
	struct tp_text_input_relay *relay = calloc(1, sizeof(*relay));
	if (!relay) {
		tam_log_error("text-input: out of memory");
		return false;
	}
	relay->server = s;
	wl_list_init(&relay->text_inputs);
	wl_list_init(&relay->popups);

	relay->new_text_input.notify = on_new_text_input;
	wl_signal_add(&s->text_input_mgr->events.new_text_input,
				  &relay->new_text_input);
	relay->new_input_method.notify = on_new_input_method;
	wl_signal_add(&s->input_method_mgr->events.new_input_method,
				  &relay->new_input_method);

	s->im_relay = relay;
	return true;
}

void tp_text_input_finish(struct tp_server *s) {
	if (!s || !s->im_relay)
		return;
	struct tp_text_input_relay *relay = s->im_relay;
	s->im_relay = NULL;
	if (relay->new_text_input.notify)
		wl_list_remove(&relay->new_text_input.link);
	if (relay->new_input_method.notify)
		wl_list_remove(&relay->new_input_method.link);
	if (relay->focused_surface_destroy.notify)
		wl_list_remove(&relay->focused_surface_destroy.link);
	detach_im_listeners(relay);
	free(relay);
}

void tp_text_input_set_focus(struct tp_server *s, struct wlr_surface *surface) {
	if (!s || !s->im_relay || s->stopping)
		return;
	struct tp_text_input_relay *relay = s->im_relay;
	if (s->locked)
		surface = NULL;

	if (relay->focused_surface != surface) {
		if (relay->focused_surface_destroy.notify) {
			wl_list_remove(&relay->focused_surface_destroy.link);
			relay->focused_surface_destroy.notify = NULL;
		}
		relay->focused_surface = surface;
		if (surface) {
			relay->focused_surface_destroy.notify = on_focused_surface_destroy;
			wl_signal_add(&surface->events.destroy,
						  &relay->focused_surface_destroy);
		}
	}

	update_text_inputs_focus(relay);
	update_active_text_input(relay);
	update_popups(relay);
}

bool tp_text_input_forward_key(struct tp_server *s, struct wlr_keyboard *kb,
							   const struct wlr_keyboard_key_event *ev) {
	if (!s || !s->im_relay || s->locked || !kb || !ev)
		return false;
	struct wlr_input_method_v2 *im = s->im_relay->input_method;
	if (!im || !im->keyboard_grab)
		return false;
	wlr_input_method_keyboard_grab_v2_set_keyboard(im->keyboard_grab, kb);
	wlr_input_method_keyboard_grab_v2_send_key(
		im->keyboard_grab, ev->time_msec, ev->keycode, ev->state);
	return true;
}

bool tp_text_input_forward_modifiers(struct tp_server *s,
									 struct wlr_keyboard *kb) {
	if (!s || !s->im_relay || s->locked || !kb)
		return false;
	struct wlr_input_method_v2 *im = s->im_relay->input_method;
	if (!im || !im->keyboard_grab)
		return false;
	wlr_input_method_keyboard_grab_v2_set_keyboard(im->keyboard_grab, kb);
	wlr_input_method_keyboard_grab_v2_send_modifiers(im->keyboard_grab,
													 &kb->modifiers);
	return true;
}
