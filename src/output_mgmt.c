/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/output_mgmt.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/box.h>

void tp_output_mgmt_update(struct tp_server *s) {
	if (!s || !s->output_mgr || s->stopping || s->output_mgmt_applying)
		return;

	struct wlr_output_configuration_v1 *cfg =
		wlr_output_configuration_v1_create();
	if (!cfg)
		return;

	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		if (!m->output)
			continue;
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(cfg, m->output);
		if (!head)
			continue;
		struct wlr_box box;
		wlr_output_layout_get_box(s->output_layout, m->output, &box);
		head->state.enabled = m->output->enabled;
		if (!wlr_box_empty(&box)) {
			head->state.x = box.x;
			head->state.y = box.y;
		}
	}

	wlr_output_manager_v1_set_configuration(s->output_mgr, cfg);
}

static void apply_or_test(struct tp_server *s,
						  struct wlr_output_configuration_v1 *config,
						  bool test_only) {
	if (!s || !config || s->stopping) {
		if (config) {
			wlr_output_configuration_v1_send_failed(config);
			wlr_output_configuration_v1_destroy(config);
		}
		return;
	}

	size_t states_len = 0;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &states_len);
	if (!states) {
		wlr_output_configuration_v1_send_failed(config);
		wlr_output_configuration_v1_destroy(config);
		return;
	}

	bool ok = test_only ? wlr_backend_test(s->backend, states, states_len)
						: wlr_backend_commit(s->backend, states, states_len);

	if (ok && !test_only) {
		s->output_mgmt_applying = true;
		struct wlr_output_configuration_head_v1 *head;
		wl_list_for_each(head, &config->heads, link) {
			struct wlr_output *out = head->state.output;
			if (!out)
				continue;
			struct tp_monitor *m = out->data;
			if (head->state.enabled) {
				bool was_in_layout =
					wlr_output_layout_get(s->output_layout, out) != NULL;
				struct wlr_output_layout_output *lo = wlr_output_layout_add(
					s->output_layout, out, head->state.x, head->state.y);
				if (!was_in_layout && m && m->scene_output && lo)
					wlr_scene_output_layout_add_output(s->scene_layout, lo,
													   m->scene_output);
			} else {
				wlr_output_layout_remove(s->output_layout, out);
			}
		}
		s->output_mgmt_applying = false;
		tp_output_mgmt_update(s);
	}

	for (size_t i = 0; i < states_len; i++)
		wlr_output_state_finish(&states[i].base);
	free(states);

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);
}

static void on_apply(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, output_mgr_apply);
	apply_or_test(s, data, false);
}

static void on_test(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, output_mgr_test);
	apply_or_test(s, data, true);
}

bool tp_output_mgmt_init(struct tp_server *s) {
	s->xdg_output_mgr =
		wlr_xdg_output_manager_v1_create(s->display, s->output_layout);
	if (!s->xdg_output_mgr) {
		tam_log_error("cannot create zxdg_output_manager_v1");
		return false;
	}

	s->output_mgr = wlr_output_manager_v1_create(s->display);
	if (!s->output_mgr) {
		tam_log_error("cannot create zwlr_output_manager_v1");
		return false;
	}
	s->output_mgr_apply.notify = on_apply;
	wl_signal_add(&s->output_mgr->events.apply, &s->output_mgr_apply);
	s->output_mgr_test.notify = on_test;
	wl_signal_add(&s->output_mgr->events.test, &s->output_mgr_test);

	s->gamma_mgr = wlr_gamma_control_manager_v1_create(s->display);
	if (!s->gamma_mgr) {
		tam_log_error("cannot create zwlr_gamma_control_manager_v1");
		return false;
	}
	wlr_scene_set_gamma_control_manager_v1(s->scene, s->gamma_mgr);
	return true;
}

void tp_output_mgmt_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->output_mgr_apply.notify) {
		wl_list_remove(&s->output_mgr_apply.link);
		s->output_mgr_apply.notify = NULL;
	}
	if (s->output_mgr_test.notify) {
		wl_list_remove(&s->output_mgr_test.link);
		s->output_mgr_test.notify = NULL;
	}
}
