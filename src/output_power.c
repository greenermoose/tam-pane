/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/output_power.h"
#include "tam-pane/output_mgmt.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include <tam.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_power_management_v1.h>

bool tp_output_power_set(struct tp_server *s, struct wlr_output *output,
						 bool enable) {
	if (!s || !output || s->stopping)
		return false;

	if (!enable && wlr_output_is_wl(output)) {
		/*
		 * On wlroots's Wayland backend, committing enabled=false unmaps and
		 * tears down the nested host window, leaving the nested session
		 * unreachable. Leave the nested output enabled so the locker's
		 * software black frame blanks the window instead.
		 */
		tam_log_info("output-power: ignoring OFF on nested Wayland output %s",
					 output->name ? output->name : "(unnamed)");
		return true;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, enable);
	bool ok = wlr_output_commit_state(output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		tam_log_warn("output-power: cannot set output %s %s",
					 output->name ? output->name : "(unnamed)",
					 enable ? "on" : "off");
		return false;
	}
	tp_output_mgmt_update(s);
	if (output->name)
		tp_events_emit_output_power(s, output->name, enable);
	return true;
}

static void on_set_mode(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, output_power_set_mode);
	struct wlr_output_power_v1_set_mode_event *ev = data;
	if (!ev || !ev->output || s->stopping)
		return;

	bool enable = ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON;
	tp_output_power_set(s, ev->output, enable);
}

bool tp_output_power_init(struct tp_server *s) {
	s->output_power_mgr = wlr_output_power_manager_v1_create(s->display);
	if (!s->output_power_mgr) {
		tam_log_error("cannot create zwlr_output_power_manager_v1");
		return false;
	}
	s->output_power_set_mode.notify = on_set_mode;
	wl_signal_add(&s->output_power_mgr->events.set_mode,
				  &s->output_power_set_mode);
	return true;
}

void tp_output_power_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->output_power_set_mode.notify) {
		wl_list_remove(&s->output_power_set_mode.link);
		s->output_power_set_mode.notify = NULL;
	}
}
