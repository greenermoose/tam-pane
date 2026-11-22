/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/server.h"
#include "tam-pane/capture.h"
#include "tam-pane/client.h"
#include "tam-pane/decoration.h"
#include "tam-pane/foreign_toplevel.h"
#include "tam-pane/idle.h"
#include "tam-pane/input.h"
#include "tam-pane/ipc.h"
#include "tam-pane/layer.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/output_mgmt.h"
#include "tam-pane/output_power.h"
#include "tam-pane/security.h"
#include "tam-pane/session_lock.h"
#include "tam-pane/text_input.h"
#include "tam-pane/workspace.h"
#include "tam-pane/xdg_activation.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_shell.h>

static int on_signal(int signo, void *data) {
	tam_log_info("signal %d: shutting down", signo);
	tp_server_quit(data);
	return 0;
}

static void on_reload_idle(void *data) {
	struct tp_server *s = data;
	s->reload_idle = NULL;
	if (s->stopping)
		return;
	tp_server_reload(s, NULL, 0);
}

static bool session_change_vt(struct tp_server *s, unsigned int vt) {
	return wlr_session_change_vt(s->session, vt);
}

static void server_run_action(const struct tp_action *act, void *userdata) {
	tp_action_run(userdata, act);
}

static void server_lock(const char *cmd, void *userdata) {
	tp_session_lock_launch(userdata, cmd);
}

static void server_lock_argv(const char *const argv[], void *userdata) {
	tp_session_lock_launch_argv(userdata, argv);
}

static void server_capture(const char *cmd, void *userdata) {
	tp_capture_launch(userdata, cmd);
}

static void server_capture_argv(const char *const argv[], void *userdata) {
	tp_capture_launch_argv(userdata, argv);
}

bool tp_server_init(struct tp_server *s) {
	wl_list_init(&s->monitors);
	wl_list_init(&s->clients);
	wl_list_init(&s->focus_stack);
	wl_list_init(&s->keyboards);
	tp_events_init(s);

	if (!s->lua)
		s->lua = tp_lua_create();
	if (s->lua) {
		tp_lua_set_action_fn(s->lua, server_run_action, s);
		tp_lua_set_lock_fn(s->lua, server_lock, s);
		tp_lua_set_lock_argv_fn(s->lua, server_lock_argv, s);
		tp_lua_set_capture_fn(s->lua, server_capture, s);
		tp_lua_set_capture_argv_fn(s->lua, server_capture_argv, s);
	}

	s->display = wl_display_create();
	if (!s->display) {
		tam_log_error("cannot create the Wayland display");
		return false;
	}
	s->loop = wl_display_get_event_loop(s->display);
	s->sigterm = wl_event_loop_add_signal(s->loop, SIGTERM, on_signal, s);
	s->sigint = wl_event_loop_add_signal(s->loop, SIGINT, on_signal, s);
	s->sighup = wl_event_loop_add_signal(s->loop, SIGHUP, on_signal, s);

	s->backend = wlr_backend_autocreate(s->loop, &s->session);
	if (!s->backend) {
		tam_log_error("cannot create a backend");
		return false;
	}
	if (s->session)
		s->change_vt = session_change_vt;
	s->renderer = wlr_renderer_autocreate(s->backend);
	if (!s->renderer) {
		tam_log_error("cannot create a renderer");
		return false;
	}
	wlr_renderer_init_wl_display(s->renderer, s->display);
	s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
	if (!s->allocator) {
		tam_log_error("cannot create an allocator");
		return false;
	}

	s->compositor = wlr_compositor_create(s->display, 6, s->renderer);
	wlr_subcompositor_create(s->display);
	wlr_data_device_manager_create(s->display);
	s->primary_selection_mgr =
		wlr_primary_selection_v1_device_manager_create(s->display);
	s->data_control_mgr = wlr_data_control_manager_v1_create(s->display);
	s->ext_data_control_mgr =
		wlr_ext_data_control_manager_v1_create(s->display, 1);
	wlr_viewporter_create(s->display);
	wlr_fractional_scale_manager_v1_create(s->display, 1);
	/* The scene sends the feedback. GTK 4 needs it: without it, GTK times
	 * frames from the output's refresh, and divides by zero on a nested
	 * output, whose refresh is 0 (B6). */
	wlr_presentation_create(s->display, s->backend, 2);

	s->scene = wlr_scene_create();
	for (int i = 0; i < TP_LAYER_COUNT; i++)
		s->layer[i] = wlr_scene_tree_create(&s->scene->tree);
	s->output_layout = wlr_output_layout_create(s->display);
	s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->output_layout);

	s->xdg_shell = wlr_xdg_shell_create(s->display, 6);
	s->layer_shell = wlr_layer_shell_v1_create(s->display, 4);
	if (!tp_security_init(s) || !tp_foreign_toplevel_init(s) ||
		!tp_workspace_init(s) || !tp_session_lock_init(s) ||
		!tp_idle_init(s) || !tp_output_power_init(s) || !tp_capture_init(s) ||
		!tp_output_mgmt_init(s) || !tp_xdg_activation_init(s) ||
		!tp_decoration_init(s))
		return false;

	tp_monitors_init(s);
	tp_layers_init(s);
	tp_clients_init(s);
	if (!tp_input_init(s) || !tp_pointer_init(s) || !tp_text_input_init(s))
		return false;
	return true;
}

bool tp_server_start(struct tp_server *s) {
	s->socket = wl_display_add_socket_auto(s->display);
	if (!s->socket) {
		tam_log_error("cannot open a Wayland socket");
		return false;
	}
	if (!wlr_backend_start(s->backend)) {
		tam_log_error("cannot start the backend");
		return false;
	}
	setenv("WAYLAND_DISPLAY", s->socket, 1);
	if (!tp_ipc_start(s)) {
		tam_log_error("cannot start the IPC socket");
		return false;
	}
	/* Autostart: startup hooks run once, now that spawned programs can
	 * reach this compositor. Reloads replace hooks but never re-send it. */
	tp_events_emit(s, &(struct tp_event){.kind = TP_EVENT_STARTUP});
	return true;
}

void tp_server_run(struct tp_server *s) {
	wl_display_run(s->display);
}

void tp_server_quit(struct tp_server *s) {
	s->quit_requested = true;
	wl_display_terminate(s->display);
}

void tp_server_set_config_path(struct tp_server *s, const char *path) {
	if (!s)
		return;
	if (!path || !*path) {
		s->config_path[0] = '\0';
		return;
	}
	snprintf(s->config_path, sizeof(s->config_path), "%s", path);
}

bool tp_server_reload(struct tp_server *s, char *err_buf, size_t err_buf_size) {
	if (!s || s->stopping) {
		if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "compositor is not running");
		return false;
	}

	char resolved[PATH_MAX];
	const char *path = NULL;
	if (s->config_path[0]) {
		path = s->config_path;
	} else if (tp_lua_default_config_path(resolved, sizeof(resolved))) {
		path = resolved;
	} else {
		const char *msg = "cannot determine config path (HOME/XDG_CONFIG_HOME unset)";
		if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "%s", msg);
		tam_log_error("reload failed: %s (keeping previous configuration)", msg);
		return false;
	}

	struct tp_settings prev = s->settings;
	char local_err[512] = {0};
	if (!tp_lua_reload_file(&s->lua, path, &s->settings, local_err,
							sizeof(local_err))) {
		if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "%s", local_err);
		tam_log_error("reload failed (%s): %s (keeping previous configuration)",
					  path, local_err[0] ? local_err : "unknown error");
		return false;
	}

	tp_monitors_apply_settings(s);
	if (prev.repeat_rate != s->settings.repeat_rate ||
		prev.repeat_delay != s->settings.repeat_delay) {
		tp_keyboards_apply_settings(s);
	}
	if (prev.cursor_size != s->settings.cursor_size ||
		strcmp(prev.cursor_theme, s->settings.cursor_theme) != 0) {
		tp_pointer_apply_settings(s);
	}
	tp_arrange_all(s);

	tam_log_info("reloaded config: %s", path);
	return true;
}

void tp_server_request_reload(struct tp_server *s) {
	if (!s || s->stopping || !s->loop || s->reload_idle)
		return;
	s->reload_idle = wl_event_loop_add_idle(s->loop, on_reload_idle, s);
}

/*
 * wlroots asserts that nobody still listens to an object it destroys, so
 * every module removes its listeners first, then objects go in reverse
 * order of creation.
 */
void tp_server_finish(struct tp_server *s) {
	if (!s->display) {
		tp_lua_destroy(s->lua);
		s->lua = NULL;
		return;
	}
	s->stopping = true;
	if (s->reload_idle) {
		wl_event_source_remove(s->reload_idle);
		s->reload_idle = NULL;
	}
	tp_ipc_finish(s);
	tp_events_finish(s);
	wl_display_destroy_clients(s->display);

	tp_text_input_finish(s);
	tp_pointer_finish(s);
	tp_input_finish(s);
	tp_clients_finish(s);
	tp_layers_finish(s);
	tp_monitors_finish(s);
	tp_decoration_finish(s);
	tp_xdg_activation_finish(s);
	tp_output_mgmt_finish(s);
	tp_capture_finish(s);
	tp_output_power_finish(s);
	tp_idle_finish(s);
	tp_session_lock_finish(s);
	tp_workspace_finish(s);
	tp_foreign_toplevel_finish(s);
	tp_security_finish(s);

	if (s->backend)
		wlr_backend_destroy(s->backend); /* outputs and devices go here */
	tp_keyboards_finish(s);
	if (s->sigterm)
		wl_event_source_remove(s->sigterm);
	if (s->sigint)
		wl_event_source_remove(s->sigint);
	if (s->sighup)
		wl_event_source_remove(s->sighup);

	/* Globals (shell, seat, layout, compositor) go with the display; the
	 * scene, allocator and renderer outlive everything that used them. */
	wl_display_destroy(s->display);
	s->display = NULL;
	if (s->scene)
		wlr_scene_node_destroy(&s->scene->tree.node);
	if (s->allocator)
		wlr_allocator_destroy(s->allocator);
	if (s->renderer)
		wlr_renderer_destroy(s->renderer);
	tp_lua_destroy(s->lua);
	s->lua = NULL;
}
