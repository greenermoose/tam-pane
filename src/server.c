/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/server.h"
#include "tam-pane/client.h"
#include "tam-pane/input.h"
#include "tam-pane/layer.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include <signal.h>
#include <stdlib.h>
#include <tam.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_shell.h>

static int on_signal(int signo, void *data) {
	tam_log_info("signal %d: shutting down", signo);
	tp_server_quit(data);
	return 0;
}

bool tp_server_init(struct tp_server *s) {
	wl_list_init(&s->monitors);
	wl_list_init(&s->clients);
	wl_list_init(&s->focus_stack);
	wl_list_init(&s->keyboards);

	s->display = wl_display_create();
	if (!s->display) {
		tam_log_error("cannot create the Wayland display");
		return false;
	}
	s->loop = wl_display_get_event_loop(s->display);
	s->sigterm = wl_event_loop_add_signal(s->loop, SIGTERM, on_signal, s);
	s->sigint = wl_event_loop_add_signal(s->loop, SIGINT, on_signal, s);

	s->backend = wlr_backend_autocreate(s->loop, &s->session);
	if (!s->backend) {
		tam_log_error("cannot create a backend");
		return false;
	}
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
	wlr_viewporter_create(s->display);
	wlr_fractional_scale_manager_v1_create(s->display, 1);

	s->scene = wlr_scene_create();
	for (int i = 0; i < TP_LAYER_COUNT; i++)
		s->layer[i] = wlr_scene_tree_create(&s->scene->tree);
	s->output_layout = wlr_output_layout_create(s->display);
	s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->output_layout);

	s->xdg_shell = wlr_xdg_shell_create(s->display, 6);
	s->layer_shell = wlr_layer_shell_v1_create(s->display, 4);

	tp_monitors_init(s);
	tp_layers_init(s);
	tp_clients_init(s);
	if (!tp_input_init(s) || !tp_pointer_init(s))
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
	return true;
}

void tp_server_run(struct tp_server *s) {
	wl_display_run(s->display);
}

void tp_server_quit(struct tp_server *s) {
	wl_display_terminate(s->display);
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
	wl_display_destroy_clients(s->display);

	tp_pointer_finish(s);
	tp_input_finish(s);
	tp_clients_finish(s);
	tp_layers_finish(s);
	tp_monitors_finish(s);

	if (s->backend)
		wlr_backend_destroy(s->backend); /* outputs and devices go here */
	tp_keyboards_finish(s);
	if (s->sigterm)
		wl_event_source_remove(s->sigterm);
	if (s->sigint)
		wl_event_source_remove(s->sigint);

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
