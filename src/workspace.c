/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/workspace.h"
#include "tam-pane/focus.h"
#include "tam-pane/input.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/tags.h"
#include <stdio.h>
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output.h>

struct tp_workspace_entry {
	struct tp_monitor *mon;
	unsigned int tag;
	struct wlr_ext_workspace_handle_v1 *handle;
};

struct tp_monitor_workspaces {
	struct wlr_ext_workspace_group_handle_v1 *group;
	struct tp_workspace_entry entries[TP_TAG_COUNT];
};

static void on_commit(struct wl_listener *l, void *data) {
	struct tp_server *s = wl_container_of(l, s, workspace_commit);
	struct wlr_ext_workspace_v1_commit_event *ev = data;
	if (s->locked || s->stopping || !ev || !ev->requests)
		return;

	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, ev->requests, link) {
		struct wlr_ext_workspace_handle_v1 *ws = NULL;
		bool activate = false;
		if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) {
			ws = req->activate.workspace;
			activate = true;
		} else if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE) {
			ws = req->deactivate.workspace;
			activate = false;
		} else {
			continue;
		}
		if (!ws || !ws->data)
			continue;

		struct tp_workspace_entry *e = ws->data;
		struct tp_monitor *m = e->mon;
		if (!m)
			continue;
		tp_tags_t prev = tp_tagview_current(&m->view);
		bool changed = activate ? tp_tagview_show(&m->view, tp_tag_mask(e->tag))
								: tp_tagview_toggle(&m->view, tp_tag_mask(e->tag));
		if (changed) {
			tp_pointer_cancel_grab(s, NULL);
			tp_arrange(m);
			tp_events_emit_tag(s, m, prev, tp_tagview_current(&m->view));
			tp_workspace_sync_monitor(m);
			if (s->selmon == m)
				tp_focus_refresh(s);
		}
	}
}

bool tp_workspace_init(struct tp_server *s) {
	s->workspace_mgr = wlr_ext_workspace_manager_v1_create(s->display, 1);
	if (!s->workspace_mgr) {
		tam_log_error("cannot create ext_workspace_manager_v1");
		return false;
	}
	s->workspace_commit.notify = on_commit;
	wl_signal_add(&s->workspace_mgr->events.commit, &s->workspace_commit);
	return true;
}

void tp_workspace_finish(struct tp_server *s) {
	if (!s)
		return;
	if (s->workspace_commit.notify)
		wl_list_remove(&s->workspace_commit.link);
	s->workspace_commit.notify = NULL;
}

void tp_workspace_monitor_init(struct tp_monitor *m) {
	if (!m || !m->server || !m->server->workspace_mgr || m->workspaces)
		return;
	struct tp_server *s = m->server;

	struct tp_monitor_workspaces *mw = calloc(1, sizeof(*mw));
	if (!mw)
		return;
	mw->group = wlr_ext_workspace_group_handle_v1_create(s->workspace_mgr, 0);
	if (!mw->group) {
		free(mw);
		return;
	}
	wlr_ext_workspace_group_handle_v1_output_enter(mw->group, m->output);

	const char *out_name =
		(m->output && m->output->name) ? m->output->name : "output";
	uint32_t caps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE |
					EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE;

	for (unsigned int i = 0; i < TP_TAG_COUNT; i++) {
		unsigned int tag = i + 1;
		char id[96], name[16];
		snprintf(id, sizeof(id), "%s:%u", out_name, tag);
		snprintf(name, sizeof(name), "%u", tag);

		struct wlr_ext_workspace_handle_v1 *ws =
			wlr_ext_workspace_handle_v1_create(s->workspace_mgr, id, caps);
		if (!ws)
			continue;
		uint32_t coord = i;
		wlr_ext_workspace_handle_v1_set_name(ws, name);
		wlr_ext_workspace_handle_v1_set_coordinates(ws, &coord, 1);
		wlr_ext_workspace_handle_v1_set_group(ws, mw->group);
		mw->entries[i].mon = m;
		mw->entries[i].tag = tag;
		mw->entries[i].handle = ws;
		ws->data = &mw->entries[i];
	}

	m->workspaces = mw;
	tp_workspace_sync_monitor(m);
}

void tp_workspace_monitor_finish(struct tp_monitor *m) {
	if (!m || !m->workspaces)
		return;
	struct tp_monitor_workspaces *mw = m->workspaces;
	m->workspaces = NULL;
	for (unsigned int i = 0; i < TP_TAG_COUNT; i++) {
		if (mw->entries[i].handle) {
			mw->entries[i].handle->data = NULL;
			wlr_ext_workspace_handle_v1_destroy(mw->entries[i].handle);
		}
	}
	if (mw->group)
		wlr_ext_workspace_group_handle_v1_destroy(mw->group);
	free(mw);
}

void tp_workspace_sync_monitor(struct tp_monitor *m) {
	if (!m || !m->workspaces)
		return;
	tp_tags_t cur = tp_tagview_current(&m->view);
	for (unsigned int i = 0; i < TP_TAG_COUNT; i++) {
		struct wlr_ext_workspace_handle_v1 *ws =
			m->workspaces->entries[i].handle;
		if (!ws)
			continue;
		bool active = (cur & tp_tag_mask(i + 1)) != 0;
		wlr_ext_workspace_handle_v1_set_active(ws, active);
	}
}
