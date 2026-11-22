/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/monitor.h"
#include "fractional-scale-v1-client-protocol.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/input.h"
#include "tam-pane/layer.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/session_lock.h"
#include "tam-pane/workspace.h"
#include "viewporter-client-protocol.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>
#include <time.h>
#include <wayland-client-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

static void update_area(struct tp_monitor *m) {
	struct wlr_box box;
	wlr_output_layout_get_box(m->server->output_layout, m->output, &box);
	m->area = (struct tp_rect){box.x, box.y, box.width, box.height};
	tp_layers_arrange(m);
}

static void on_frame(struct wl_listener *l, void *data) {
	struct tp_monitor *m = wl_container_of(l, m, frame);
	struct timespec now;
	wlr_scene_output_commit(m->scene_output, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

static void apply_nested_scale(struct tp_monitor *m, int32_t logical_w,
							   int32_t logical_h, int32_t refresh) {
	if (!m->host_viewport || m->host_scale <= 0.0f || logical_w <= 0 ||
		logical_h <= 0)
		return;
	m->host_w = logical_w;
	m->host_h = logical_h;
	int32_t pw = (int32_t)lroundf((float)logical_w * m->host_scale);
	int32_t ph = (int32_t)lroundf((float)logical_h * m->host_scale);
	if (pw < 1)
		pw = 1;
	if (ph < 1)
		ph = 1;

	wp_viewport_set_destination(m->host_viewport, logical_w, logical_h);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, pw, ph, refresh);
	wlr_output_state_set_scale(&state, m->host_scale);
	wlr_output_commit_state(m->output, &state);
	wlr_output_state_finish(&state);
}

static void on_preferred_scale(void *data,
							   struct wp_fractional_scale_v1 *fscale,
							   uint32_t scale120) {
	struct tp_monitor *m = data;
	float scale = (float)scale120 / 120.0f;
	if (!(scale >= 0.5f && scale <= 8.0f))
		return;
	m->host_scale = scale;
	int32_t lw = m->host_w > 0 ? m->host_w
							   : (int32_t)lroundf((float)m->output->width /
												  m->output->scale);
	int32_t lh = m->host_h > 0 ? m->host_h
							   : (int32_t)lroundf((float)m->output->height /
												  m->output->scale);
	tam_log_info("output %s: host fractional scale %.3f (%dx%d logical)",
				 m->output->name, scale, lw, lh);
	apply_nested_scale(m, lw, lh, 0);
}

static const struct wp_fractional_scale_v1_listener host_fscale_listener = {
	.preferred_scale = on_preferred_scale,
};

static void on_host_global(void *data, struct wl_registry *reg, uint32_t name,
						   const char *iface, uint32_t version) {
	struct tp_monitor *m = data;
	if (!strcmp(iface, wp_viewporter_interface.name) && !m->host_viewporter) {
		m->host_viewporter =
			wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name) &&
			   !m->host_fsm) {
		m->host_fsm = wl_registry_bind(
			reg, name, &wp_fractional_scale_manager_v1_interface, 1);
	}
}

static void on_host_global_remove(void *data, struct wl_registry *reg,
								  uint32_t name) {}

static const struct wl_registry_listener host_registry_listener = {
	.global = on_host_global,
	.global_remove = on_host_global_remove,
};

static void setup_nested_scale(struct tp_monitor *m) {
	if (!wlr_output_is_wl(m->output) || !wlr_backend_is_wl(m->output->backend))
		return;
	struct wl_display *remote =
		wlr_wl_backend_get_remote_display(m->output->backend);
	struct wl_surface *surface = wlr_wl_output_get_surface(m->output);
	if (!remote || !surface)
		return;

	struct wl_event_queue *q = wl_display_create_queue(remote);
	struct wl_display *wrapped = q ? wl_proxy_create_wrapper(remote) : NULL;
	if (!wrapped) {
		if (q)
			wl_event_queue_destroy(q);
		return;
	}
	wl_proxy_set_queue((struct wl_proxy *)wrapped, q);
	struct wl_registry *reg = wl_display_get_registry(wrapped);
	wl_registry_add_listener(reg, &host_registry_listener, m);
	wl_display_roundtrip_queue(remote, q);
	wl_registry_destroy(reg);
	if (m->host_viewporter)
		wl_proxy_set_queue((struct wl_proxy *)m->host_viewporter, NULL);
	if (m->host_fsm)
		wl_proxy_set_queue((struct wl_proxy *)m->host_fsm, NULL);
	wl_proxy_wrapper_destroy(wrapped);
	wl_event_queue_destroy(q);

	if (m->host_viewporter && m->host_fsm) {
		m->host_viewport =
			wp_viewporter_get_viewport(m->host_viewporter, surface);
		m->host_fscale = wp_fractional_scale_manager_v1_get_fractional_scale(
			m->host_fsm, surface);
		if (m->host_fscale)
			wp_fractional_scale_v1_add_listener(m->host_fscale,
												&host_fscale_listener, m);
	}
}

static void finish_nested_scale(struct tp_monitor *m) {
	if (m->host_fscale)
		wp_fractional_scale_v1_destroy(m->host_fscale);
	if (m->host_viewport)
		wp_viewport_destroy(m->host_viewport);
	if (m->host_fsm)
		wp_fractional_scale_manager_v1_destroy(m->host_fsm);
	if (m->host_viewporter)
		wp_viewporter_destroy(m->host_viewporter);
	m->host_fscale = NULL;
	m->host_viewport = NULL;
	m->host_fsm = NULL;
	m->host_viewporter = NULL;
}

static void on_request_state(struct wl_listener *l, void *data) {
	struct tp_monitor *m = wl_container_of(l, m, request_state);
	const struct wlr_output_event_request_state *ev = data;
	if (m->host_viewport && (ev->state->committed & WLR_OUTPUT_STATE_MODE) &&
		ev->state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
		m->host_w = ev->state->custom_mode.width;
		m->host_h = ev->state->custom_mode.height;
		if (m->host_scale > 0.0f) {
			apply_nested_scale(m, m->host_w, m->host_h,
							   ev->state->custom_mode.refresh);
			return;
		}
	}
	wlr_output_commit_state(m->output, ev->state);
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct tp_monitor *m = wl_container_of(l, m, destroy);
	struct tp_server *s = m->server;
	const char *name = m->output->name;
	char mon_name[64] = {0};
	if (name)
		snprintf(mon_name, sizeof(mon_name), "%s", name);
	struct tp_rect area = m->area;
	float scale = m->output->scale;

	if (s->grab.client && s->grab.client->mon == m)
		tp_pointer_cancel_grab(s, NULL);
	tp_workspace_monitor_finish(m);
	tp_layers_monitor_destroy(m);

	finish_nested_scale(m);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->request_state.link);
	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->link);
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++)
		tp_dwindle_clear(&m->trees[i]);

	/* Windows move to another monitor, keeping their tags and remembering
	 * their original output name so they return when it comes back. */
	struct tp_monitor *heir = wl_list_empty(&s->monitors)
								  ? NULL
								  : wl_container_of(s->monitors.next, heir, link);
	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) {
		if (c->mon != m)
			continue;
		if (!c->home_output && name && *name)
			c->home_output = strdup(name);
		if (heir)
			tp_client_carry_float(c, m->usable, heir->usable);
		tp_client_attach(c, heir, 0);
		if (!heir)
			wlr_scene_node_set_enabled(&c->tree->node, false);
	}
	if (s->selmon == m)
		s->selmon = heir;
	free(m);

	if (s->stopping)
		return;
	if (heir)
		tp_arrange(heir);
	tp_session_lock_arrange(s);
	tp_events_emit_monitor(s, TP_EVENT_MONITOR_REMOVE, mon_name, area, scale);
	tp_focus_refresh(s);
}

static float requested_scale(void) {
	const char *env = getenv("TAM_PANE_SCALE");
	if (!env || !*env)
		return 0.0f;
	char *end = NULL;
	float scale = strtof(env, &end);
	if (end == env || *end != '\0' || !(scale >= 0.5f && scale <= 8.0f))
		return 0.0f;
	return scale;
}

static void on_new_output(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_output);
	struct wlr_output *output = data;

	if (!wlr_output_init_render(output, s->allocator, s->renderer)) {
		tam_log_error("output %s: cannot start rendering", output->name);
		return;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);
	float scale = requested_scale();
	if (scale > 0.0f)
		wlr_output_state_set_scale(&state, scale);
	bool ok = wlr_output_commit_state(output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		tam_log_error("output %s: cannot enable", output->name);
		return;
	}

	struct tp_monitor *m = calloc(1, sizeof(*m));
	if (!m) {
		tam_log_error("output %s: out of memory", output->name);
		return;
	}
	m->server = s;
	m->output = output;
	output->data = m;
	wl_list_init(&m->layers);
	tp_tagview_init(&m->view, tp_tag_mask(1));
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++) {
		tp_dwindle_init(&m->trees[i]);
		m->trees[i].width_bias = s->settings.width_bias;
		m->trees[i].preserve_split = s->settings.preserve_split;
		m->trees[i].proportional_split = s->settings.proportional_split;
	}

	m->frame.notify = on_frame;
	wl_signal_add(&output->events.frame, &m->frame);
	m->request_state.notify = on_request_state;
	wl_signal_add(&output->events.request_state, &m->request_state);
	m->destroy.notify = on_destroy;
	wl_signal_add(&output->events.destroy, &m->destroy);
	wl_list_insert(s->monitors.prev, &m->link);

	struct wlr_output_layout_output *lo =
		wlr_output_layout_add_auto(s->output_layout, output);
	m->scene_output = wlr_scene_output_create(s->scene, output);
	wlr_scene_output_layout_add_output(s->scene_layout, lo, m->scene_output);
	update_area(m);
	setup_nested_scale(m);
	tp_workspace_monitor_init(m);
	if (!s->selmon)
		s->selmon = m;
	tp_session_lock_arrange(s);

	/* Reclaim windows displaced when this output was unplugged, and adopt
	 * any orphaned windows that had no monitor left. */
	bool moved = false;
	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) {
		bool match = c->home_output && output->name &&
					 strcmp(c->home_output, output->name) == 0;
		if (!match && c->mon != NULL)
			continue;
		struct tp_monitor *old = c->mon;
		if (old && old != m)
			tp_client_carry_float(c, old->usable, m->usable);
		tp_client_attach(c, m, 0);
		if (match)
			tp_client_clear_home(c);
		moved = true;
	}
	if (moved) {
		tp_arrange_all(s);
		if (!tp_focused(s))
			tp_focus_refresh(s);
	}
	tam_log_info("output %s: %dx%d at %d,%d scale %.2f", output->name,
				 m->area.w, m->area.h, m->area.x, m->area.y, output->scale);
	tp_events_emit_monitor(s, TP_EVENT_MONITOR_ADD, output->name, m->area,
						   output->scale);
}

static void on_layout_change(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, layout_change);
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) update_area(m);
	tp_arrange_all(s);
	tp_session_lock_arrange(s);
}

void tp_monitors_init(struct tp_server *s) {
	s->new_output.notify = on_new_output;
	wl_signal_add(&s->backend->events.new_output, &s->new_output);
	s->layout_change.notify = on_layout_change;
	wl_signal_add(&s->output_layout->events.change, &s->layout_change);
}

void tp_monitors_finish(struct tp_server *s) {
	if (s->new_output.notify)
		wl_list_remove(&s->new_output.link);
	if (s->layout_change.notify)
		wl_list_remove(&s->layout_change.link);
	s->new_output.notify = s->layout_change.notify = NULL;
}

struct tp_monitor *tp_monitor_at(struct tp_server *s, double x, double y) {
	struct wlr_output *o = wlr_output_layout_output_at(s->output_layout, x, y);
	return o ? o->data : NULL;
}

struct tp_monitor *tp_monitor_by_name(struct tp_server *s, const char *name) {
	if (!s || !name || !*name)
		return NULL;
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		if (m->output && m->output->name &&
			strcmp(m->output->name, name) == 0)
			return m;
	}
	return NULL;
}

struct tp_monitor *tp_monitor_cycle(struct tp_server *s, int dir) {
	if (!s->selmon || wl_list_length(&s->monitors) < 2)
		return NULL;
	struct wl_list *pos = dir >= 0 ? s->selmon->link.next : s->selmon->link.prev;
	if (pos == &s->monitors)
		pos = dir >= 0 ? s->monitors.next : s->monitors.prev;
	return wl_container_of(pos, (struct tp_monitor *)NULL, link);
}

struct tp_monitor *tp_monitor_in_dir(struct tp_server *s, struct tp_monitor *m,
									 enum tp_dir dir) {
	if (!m)
		return NULL;
	enum wlr_direction wdir;
	switch (dir) {
	case TP_DIR_LEFT:
		wdir = WLR_DIRECTION_LEFT;
		break;
	case TP_DIR_RIGHT:
		wdir = WLR_DIRECTION_RIGHT;
		break;
	case TP_DIR_UP:
		wdir = WLR_DIRECTION_UP;
		break;
	case TP_DIR_DOWN:
		wdir = WLR_DIRECTION_DOWN;
		break;
	default:
		return NULL;
	}
	double cx = m->area.x + m->area.w / 2.0;
	double cy = m->area.y + m->area.h / 2.0;
	struct wlr_output *o = wlr_output_layout_adjacent_output(
		s->output_layout, wdir, m->output, cx, cy);
	return o ? o->data : NULL;
}

void tp_monitor_focus(struct tp_server *s, struct tp_monitor *m) {
	if (!m)
		return;
	s->selmon = m;
	if (s->cursor && tp_monitor_at(s, s->cursor->x, s->cursor->y) != m) {
		double x = m->usable.x + m->usable.w / 2.0;
		double y = m->usable.y + m->usable.h / 2.0;
		wlr_cursor_warp(s->cursor, NULL, x, y);
	}
	tp_focus_refresh(s);
}

struct tp_dwindle *tp_monitor_tree(struct tp_monitor *m) {
	return &m->trees[tp_tags_slot(tp_tagview_current(&m->view))];
}

void tp_monitor_forget(struct tp_monitor *m, struct tp_client *c) {
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++)
		tp_dwindle_remove(&m->trees[i], c);
}

void tp_monitors_apply_settings(struct tp_server *s) {
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		for (unsigned int i = 0; i < TP_TAG_SLOTS; i++) {
			m->trees[i].width_bias = s->settings.width_bias;
			m->trees[i].preserve_split = s->settings.preserve_split;
			m->trees[i].proportional_split = s->settings.proportional_split;
		}
	}
}
