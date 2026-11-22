/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/output_power.h"
#include "tam-pane/server.h"
#include <tam.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_power_management_v1.h>

static void on_set_mode(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, output_power_set_mode);
	struct wlr_output_power_v1_set_mode_event *ev = data;
	if (!ev || !ev->output || s->stopping)
		return;

	bool enable = ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, enable);
	if (!wlr_output_commit_state(ev->output, &state)) {
		tam_log_warn("output-power: cannot set output %s %s",
					 ev->output->name ? ev->output->name : "(unnamed)",
					 enable ? "on" : "off");
	}
	wlr_output_state_finish(&state);
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
