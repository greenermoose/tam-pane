/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/monitor.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

static void update_area(struct tp_monitor *m) {
	struct wlr_box box;
	wlr_output_layout_get_box(m->server->output_layout, m->output, &box);
	m->area = (struct tp_rect){box.x, box.y, box.width, box.height};
	m->usable = m->area; /* panels reserve space here from 0.0.5 */
}

static void on_frame(struct wl_listener *l, void *data) {
	struct tp_monitor *m = wl_container_of(l, m, frame);
	struct timespec now;
	wlr_scene_output_commit(m->scene_output, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

static void on_request_state(struct wl_listener *l, void *data) {
	struct tp_monitor *m = wl_container_of(l, m, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(m->output, ev->state);
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct tp_monitor *m = wl_container_of(l, m, destroy);
	struct tp_server *s = m->server;

	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->request_state.link);
	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->link);
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++)
		tp_dwindle_clear(&m->trees[i]);

	/* Windows move to another monitor, keeping their tags. */
	struct tp_monitor *heir = wl_list_empty(&s->monitors)
								  ? NULL
								  : wl_container_of(s->monitors.next, heir, link);
	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) if (c->mon == m) c->mon = heir;
	if (s->selmon == m)
		s->selmon = heir;
	free(m);

	if (s->stopping)
		return;
	if (heir)
		tp_arrange(heir);
	tp_focus_refresh(s);
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
	if (!s->selmon)
		s->selmon = m;
	tam_log_info("output %s: %dx%d at %d,%d", output->name, m->area.w,
				 m->area.h, m->area.x, m->area.y);
}

static void on_layout_change(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, layout_change);
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) update_area(m);
	tp_arrange_all(s);
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

struct tp_dwindle *tp_monitor_tree(struct tp_monitor *m) {
	return &m->trees[tp_tags_slot(tp_tagview_current(&m->view))];
}

void tp_monitor_forget(struct tp_monitor *m, struct tp_client *c) {
	for (unsigned int i = 0; i < TP_TAG_SLOTS; i++)
		tp_dwindle_remove(&m->trees[i], c);
}
