/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/foreign_toplevel.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/workspace.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

struct tp_foreign_toplevel {
	struct tp_client *client;
	struct wlr_foreign_toplevel_handle_v1 *wlr_handle;
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;
	struct wlr_output *last_output;
	struct wl_listener request_activate;
	struct wl_listener request_fullscreen;
	struct wl_listener request_close;
};

static void on_request_activate(struct wl_listener *l, void *data) {
	struct tp_foreign_toplevel *ft = wl_container_of(l, ft, request_activate);
	struct tp_client *c = ft->client;
	struct tp_server *s = c->server;
	if (!c->mapped || s->locked || s->stopping)
		return;
	if (c->mon && !tp_client_visible(c)) {
		unsigned int first = tp_tags_first(c->tags);
		if (first > 0) {
			tp_tags_t prev = tp_tagview_current(&c->mon->view);
			if (tp_tagview_show(&c->mon->view, tp_tag_mask(first))) {
				tp_arrange(c->mon);
				tp_events_emit_tag(s, c->mon, prev,
								   tp_tagview_current(&c->mon->view));
				tp_workspace_sync_monitor(c->mon);
			}
		}
	}
	tp_focus(s, c);
}

static void on_request_fullscreen(struct wl_listener *l, void *data) {
	struct tp_foreign_toplevel *ft = wl_container_of(l, ft, request_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
	struct tp_client *c = ft->client;
	if (!c->mapped || c->server->locked || c->server->stopping)
		return;
	tp_client_set_fullscreen(c, ev->fullscreen);
}

static void on_request_close(struct wl_listener *l, void *data) {
	struct tp_foreign_toplevel *ft = wl_container_of(l, ft, request_close);
	struct tp_client *c = ft->client;
	if (!c->mapped || c->server->locked || c->server->stopping)
		return;
	tp_client_close(c);
}

bool tp_foreign_toplevel_init(struct tp_server *s) {
	s->foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(s->display);
	if (!s->foreign_toplevel_mgr) {
		tam_log_error("cannot create zwlr_foreign_toplevel_manager_v1");
		return false;
	}
	s->ext_foreign_toplevel_list =
		wlr_ext_foreign_toplevel_list_v1_create(s->display, 1);
	if (!s->ext_foreign_toplevel_list) {
		tam_log_error("cannot create ext_foreign_toplevel_list_v1");
		return false;
	}
	return true;
}

void tp_foreign_toplevel_finish(struct tp_server *s) {
	(void)s;
	/* Globals are owned by wl_display; per-client handles are destroyed on
	 * client unmap before wl_display_destroy_clients completes. */
}

void tp_foreign_toplevel_map(struct tp_client *c) {
	if (!c || c->foreign)
		return;
	struct tp_server *s = c->server;
	if (!s->foreign_toplevel_mgr || !s->ext_foreign_toplevel_list)
		return;

	struct tp_foreign_toplevel *ft = calloc(1, sizeof(*ft));
	if (!ft)
		return;
	ft->client = c;

	const char *title = c->toplevel->title ? c->toplevel->title : "";
	const char *app_id = c->toplevel->app_id ? c->toplevel->app_id : "";

	ft->wlr_handle =
		wlr_foreign_toplevel_handle_v1_create(s->foreign_toplevel_mgr);
	if (ft->wlr_handle) {
		wlr_foreign_toplevel_handle_v1_set_title(ft->wlr_handle, title);
		wlr_foreign_toplevel_handle_v1_set_app_id(ft->wlr_handle, app_id);
		wlr_foreign_toplevel_handle_v1_set_fullscreen(ft->wlr_handle,
													  c->fullscreen);
		wlr_foreign_toplevel_handle_v1_set_activated(ft->wlr_handle,
													 tp_focused(s) == c);
		if (c->mon && c->mon->output) {
			wlr_foreign_toplevel_handle_v1_output_enter(ft->wlr_handle,
														c->mon->output);
			ft->last_output = c->mon->output;
		}
		ft->request_activate.notify = on_request_activate;
		wl_signal_add(&ft->wlr_handle->events.request_activate,
					  &ft->request_activate);
		ft->request_fullscreen.notify = on_request_fullscreen;
		wl_signal_add(&ft->wlr_handle->events.request_fullscreen,
					  &ft->request_fullscreen);
		ft->request_close.notify = on_request_close;
		wl_signal_add(&ft->wlr_handle->events.request_close,
					  &ft->request_close);
	}

	struct wlr_ext_foreign_toplevel_handle_v1_state ext_state = {
		.title = title,
		.app_id = app_id,
	};
	ft->ext_handle = wlr_ext_foreign_toplevel_handle_v1_create(
		s->ext_foreign_toplevel_list, &ext_state);
	if (ft->ext_handle)
		ft->ext_handle->data = c;

	c->foreign = ft;
}

void tp_foreign_toplevel_unmap(struct tp_client *c) {
	if (!c || !c->foreign)
		return;
	struct tp_foreign_toplevel *ft = c->foreign;
	c->foreign = NULL;
	if (ft->wlr_handle) {
		wl_list_remove(&ft->request_activate.link);
		wl_list_remove(&ft->request_fullscreen.link);
		wl_list_remove(&ft->request_close.link);
		wlr_foreign_toplevel_handle_v1_destroy(ft->wlr_handle);
	}
	if (ft->ext_handle) {
		ft->ext_handle->data = NULL;
		wlr_ext_foreign_toplevel_handle_v1_destroy(ft->ext_handle);
	}
	free(ft);
}

void tp_foreign_toplevel_update_info(struct tp_client *c) {
	if (!c || !c->foreign)
		return;
	struct tp_foreign_toplevel *ft = c->foreign;
	const char *title = c->toplevel->title ? c->toplevel->title : "";
	const char *app_id = c->toplevel->app_id ? c->toplevel->app_id : "";

	if (ft->wlr_handle) {
		wlr_foreign_toplevel_handle_v1_set_title(ft->wlr_handle, title);
		wlr_foreign_toplevel_handle_v1_set_app_id(ft->wlr_handle, app_id);
	}
	if (ft->ext_handle) {
		struct wlr_ext_foreign_toplevel_handle_v1_state ext_state = {
			.title = title,
			.app_id = app_id,
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(ft->ext_handle,
														&ext_state);
	}
}

void tp_foreign_toplevel_set_activated(struct tp_client *c, bool activated) {
	if (!c || !c->foreign || !c->foreign->wlr_handle)
		return;
	wlr_foreign_toplevel_handle_v1_set_activated(c->foreign->wlr_handle,
												 activated);
}

void tp_foreign_toplevel_set_fullscreen(struct tp_client *c, bool fullscreen) {
	if (!c || !c->foreign || !c->foreign->wlr_handle)
		return;
	wlr_foreign_toplevel_handle_v1_set_fullscreen(c->foreign->wlr_handle,
												  fullscreen);
}

void tp_foreign_toplevel_set_output(struct tp_client *c,
									struct wlr_output *output) {
	if (!c || !c->foreign || !c->foreign->wlr_handle)
		return;
	struct tp_foreign_toplevel *ft = c->foreign;
	if (ft->last_output == output)
		return;
	if (ft->last_output)
		wlr_foreign_toplevel_handle_v1_output_leave(ft->wlr_handle,
													ft->last_output);
	ft->last_output = output;
	if (output)
		wlr_foreign_toplevel_handle_v1_output_enter(ft->wlr_handle, output);
}
