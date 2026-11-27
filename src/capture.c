/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/capture.h"
#include "tam-pane/client.h"
#include "tam-pane/security.h"
#include "tam-pane/server.h"
#include "tam-pane/spawn.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <tam.h>
#include <unistd.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>

static void on_client_capture_source_destroy(struct wl_listener *l,
											 void *data) {
	struct tp_client *c = wl_container_of(l, c, capture_source_destroy);
	wl_list_remove(&c->capture_source_destroy.link);
	c->capture_source_destroy.notify = NULL;
	c->capture_source = NULL;
}

void tp_capture_client_finish(struct tp_client *c) {
	if (!c)
		return;
	if (c->capture_source_destroy.notify) {
		wl_list_remove(&c->capture_source_destroy.link);
		c->capture_source_destroy.notify = NULL;
	}
	c->capture_source = NULL;
}

void tp_capture_reset_sources(struct tp_server *s) {
	if (!s)
		return;
	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) {
		tp_capture_client_finish(c);
	}
}

static void on_new_toplevel_capture_request(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, new_toplevel_capture_request);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req =
		data;
	if (!req || !req->toplevel_handle || s->locked || s->stopping)
		return;

	struct tp_client *c = req->toplevel_handle->data;
	if (!c || !c->mapped || !c->tree)
		return;

	if (!c->capture_source) {
		c->capture_source =
			wlr_ext_image_capture_source_v1_create_with_scene_node(
				&c->tree->node, s->loop, s->allocator, s->renderer);
		if (!c->capture_source) {
			tam_log_warn("capture: cannot create toplevel capture source");
			return;
		}
		c->capture_source_destroy.notify = on_client_capture_source_destroy;
		wl_signal_add(&c->capture_source->events.destroy,
					  &c->capture_source_destroy);
	}

	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
		req, c->capture_source);
}

bool tp_capture_init(struct tp_server *s) {
	s->screencopy_mgr = wlr_screencopy_manager_v1_create(s->display);
	if (!s->screencopy_mgr) {
		tam_log_error("cannot create zwlr_screencopy_manager_v1");
		return false;
	}
	s->image_copy_capture_mgr =
		wlr_ext_image_copy_capture_manager_v1_create(s->display, 1);
	if (!s->image_copy_capture_mgr) {
		tam_log_error("cannot create ext_image_copy_capture_manager_v1");
		return false;
	}
	s->output_capture_source_mgr =
		wlr_ext_output_image_capture_source_manager_v1_create(s->display, 1);
	if (!s->output_capture_source_mgr) {
		tam_log_error("cannot create ext_output_image_capture_source_manager_v1");
		return false;
	}
	s->toplevel_capture_source_mgr =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
			s->display, 1);
	if (!s->toplevel_capture_source_mgr) {
		tam_log_error(
			"cannot create ext_foreign_toplevel_image_capture_source_manager_v1");
		return false;
	}
	s->new_toplevel_capture_request.notify = on_new_toplevel_capture_request;
	wl_signal_add(&s->toplevel_capture_source_mgr->events.new_request,
				  &s->new_toplevel_capture_request);
	return true;
}

void tp_capture_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->new_toplevel_capture_request.notify) {
		wl_list_remove(&s->new_toplevel_capture_request.link);
		s->new_toplevel_capture_request.notify = NULL;
	}
}

void tp_capture_launch_argv(struct tp_server *s, const char *const argv[]) {
	if (!s || !s->display || !argv || !argv[0] || !*argv[0])
		return;
	int sv[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		tam_log_error("capture: socketpair failed: %s", strerror(errno));
		return;
	}
	struct wl_client *cl = wl_client_create(s->display, sv[0]);
	if (!cl) {
		tam_log_error("capture: wl_client_create failed");
		close(sv[0]);
		close(sv[1]);
		return;
	}
	if (!tp_security_add_capture_client(s, cl)) {
		tam_log_error("capture: cannot register authorized client");
		wl_client_destroy(cl);
		close(sv[1]);
		return;
	}
	tp_spawn_with_fd_argv(argv, sv[1]);
}

void tp_capture_launch(struct tp_server *s, const char *cmd) {
	if (!s || !cmd || !*cmd)
		return;
	const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
	tp_capture_launch_argv(s, argv);
}
