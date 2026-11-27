/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Server-side Unix domain IPC listener, request dispatcher, and non-blocking
 * event fan-out.
 *
 * No blocking I/O on the compositor event loop: every socket is non-blocking,
 * outbound data is buffered per client with a hard TP_IPC_MAX_OUTBUF cap, and
 * slow subscribers that exceed the cap are disconnected cleanly.
 */
#define _GNU_SOURCE
#include "tam-pane/ipc.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/input.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/output_power.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/session_lock.h"
#include "tam-pane/workspace.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <tam.h>
#include <unistd.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>

struct tp_ipc_client {
	struct wl_list link;
	struct tp_ipc_server *ipc;
	int fd;
	struct wl_event_source *source;
	char rbuf[TP_IPC_MAX_LINE + 1];
	size_t rlen;
	tam_str_t wbuf;
	size_t woff;
	uint32_t sub_mask;
	bool close_after_write;
};

struct tp_ipc_server {
	struct tp_server *server;
	int listen_fd;
	struct wl_event_source *listen_source;
	char socket_path[PATH_MAX];
	bool socket_created;
	struct wl_list clients; /* tp_ipc_client.link */
	size_t client_count;
	uint32_t sub_counts[TP_EVENT_COUNT];
};

static void ipc_client_destroy(struct tp_ipc_client *c) {
	if (!c)
		return;
	struct tp_ipc_server *ipc = c->ipc;
	if (ipc) {
		for (int k = 0; k < TP_EVENT_COUNT; k++) {
			if ((c->sub_mask & (1u << k)) && ipc->sub_counts[k] > 0)
				ipc->sub_counts[k]--;
		}
		if (ipc->client_count > 0)
			ipc->client_count--;
	}
	if (c->source)
		wl_event_source_remove(c->source);
	if (c->fd >= 0)
		close(c->fd);
	tam_str_free(c->wbuf);
	wl_list_remove(&c->link);
	free(c);
}

static void ipc_client_update_events(struct tp_ipc_client *c) {
	if (!c || !c->source)
		return;
	uint32_t mask = WL_EVENT_HANGUP | WL_EVENT_ERROR;
	if (!c->close_after_write)
		mask |= WL_EVENT_READABLE;
	if (c->wbuf && c->woff < tam_str_len(c->wbuf))
		mask |= WL_EVENT_WRITABLE;
	wl_event_source_fd_update(c->source, mask);
}

static bool ipc_client_flush(struct tp_ipc_client *c) {
	if (!c->wbuf)
		return true;
	size_t len = tam_str_len(c->wbuf);
	while (c->woff < len) {
		ssize_t n = send(c->fd, c->wbuf + c->woff, len - c->woff,
						 MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return false;
		}
		if (n == 0)
			return false;
		c->woff += (size_t)n;
	}
	if (c->woff >= len) {
		tam_str_clear(c->wbuf);
		c->woff = 0;
	}
	return true;
}

static bool ipc_client_queue(struct tp_ipc_client *c, const char *data,
							 size_t len) {
	if (!c || !data || len == 0)
		return true;
	size_t pending = c->wbuf ? (tam_str_len(c->wbuf) - c->woff) : 0;
	if (pending + len > TP_IPC_MAX_OUTBUF)
		return false;
	if (!c->wbuf) {
		c->wbuf = tam_str_new_len(data, len);
		c->woff = 0;
	} else {
		if (c->woff > 0 && c->woff >= tam_str_len(c->wbuf)) {
			tam_str_clear(c->wbuf);
			c->woff = 0;
		}
		c->wbuf = tam_str_cat_len(c->wbuf, data, len);
	}
	if (!c->wbuf)
		return false;
	if (!ipc_client_flush(c))
		return false;
	ipc_client_update_events(c);
	return true;
}

static bool ipc_send_err(struct tp_ipc_client *c, const char *msg) {
	tam_str_t out = tam_str_new("ERR ");
	if (!out)
		return false;
	out = tp_ipc_escape_append(out, msg ? msg : "error");
	out = tam_str_cat_len(out, "\n", 1);
	if (!out)
		return false;
	bool ok = ipc_client_queue(c, out, tam_str_len(out));
	tam_str_free(out);
	return ok;
}

/* ---- Reply builders ---- */

static tam_str_t append_window_line(tam_str_t out, const struct tp_server *s,
									const struct tp_client *c) {
	const char *mon = (c->mon && c->mon->output && c->mon->output->name)
						  ? c->mon->output->name
						  : "";
	const char *app_id = tp_client_app_id(c);
	const char *title = tp_client_title(c);
	if (!app_id)
		app_id = "";
	if (!title)
		title = "";
	bool focused = (s->focused_client == c);

	out = tam_str_cat(out, "window\tmonitor=");
	out = tp_ipc_escape_append(out, mon);
	out = tam_str_cat_printf(
		out,
		"\tid=0x%llx\tpid=%d\ttags=%u\tfocused=%d\tfloating=%d\tfullscreen=%d"
		"\tscratchpad=%d\txwayland=%d\tx=%d\ty=%d\tw=%d\th=%d\tapp_id=",
		(unsigned long long)c->id, tp_client_pid(c), c->tags, focused ? 1 : 0,
		c->floating ? 1 : 0, c->fullscreen ? 1 : 0, c->scratchpad ? 1 : 0,
		tp_client_is_xwayland(c) ? 1 : 0, c->geom.x, c->geom.y, c->geom.w,
		c->geom.h);
	out = tp_ipc_escape_append(out, app_id);
	out = tam_str_cat(out, "\ttitle=");
	out = tp_ipc_escape_append(out, title);
	return tam_str_cat_len(out, "\n", 1);
}

static tam_str_t build_version(const struct tp_server *s) {
	tam_str_t out = tam_str_new("OK\nversion\tversion=");
	out = tp_ipc_escape_append(out, TP_VERSION);
	out = tam_str_cat(out, "\tsocket=");
	out = tp_ipc_escape_append(out, s->socket ? s->socket : "");
	out = tam_str_cat(out, "\tdisplay=");
	out = tp_ipc_escape_append(out,
							   s->xwayland_display ? s->xwayland_display : "");
	return tam_str_cat(out, "\nEND\n");
}

static tam_str_t build_monitors(const struct tp_server *s) {
	tam_str_t out = tam_str_new("OK\n");
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		const char *name =
			(m->output && m->output->name) ? m->output->name : "";
		const char *desc = "";
		if (m->output && m->output->description && *m->output->description)
			desc = m->output->description;
		else if (m->output && m->output->model && *m->output->model)
			desc = m->output->model;
		int dpms = (m->output && !m->output->enabled) ? 0 : 1;
		float scale = m->output ? m->output->scale : 1.0f;
		int phys_w = m->output ? m->output->width : m->area.w;
		int phys_h = m->output ? m->output->height : m->area.h;
		int refresh = m->output ? m->output->refresh : 0;
		int transform = m->output ? (int)m->output->transform : 0;
		tp_tags_t cur = tp_tagview_current(&m->view);
		tp_tags_t prev = m->view.slot[m->view.cur ^ 1u];
		out = tam_str_cat(out, "monitor\tname=");
		out = tp_ipc_escape_append(out, name);
		out = tam_str_cat_printf(
			out,
			"\tx=%d\ty=%d\tw=%d\th=%d\tphys_w=%d\tphys_h=%d\trefresh=%d\ttransform=%d"
			"\tusable_x=%d\tusable_y=%d\tusable_w=%d\tusable_h=%d"
			"\tscale=%.2f\ttags=%u\tprev_tags=%u\tfocused=%d\tlayout=%s\tdpms=%d\tdescription=",
			m->area.x, m->area.y, m->area.w, m->area.h, phys_w, phys_h, refresh,
			transform, m->usable.x, m->usable.y, m->usable.w, m->usable.h,
			scale, cur, prev, s->selmon == m ? 1 : 0,
			tp_layout_kind_name(tp_monitor_layout(m)->kind), dpms);
		out = tp_ipc_escape_append(out, desc);
		out = tam_str_cat_len(out, "\n", 1);
		if (!out)
			return NULL;
	}
	return tam_str_cat(out, "END\n");
}

static tam_str_t build_windows(const struct tp_server *s) {
	tam_str_t out = tam_str_new("OK\n");
	struct tp_client *c;
	wl_list_for_each(c, &s->clients, link) {
		if (!c->mapped)
			continue;
		out = append_window_line(out, s, c);
		if (!out)
			return NULL;
	}
	return tam_str_cat(out, "END\n");
}

static tam_str_t build_focused(struct tp_server *s) {
	tam_str_t out = tam_str_new("OK\n");
	struct tp_client *c = tp_focused(s);
	if (c && c->mapped)
		out = append_window_line(out, s, c);
	return tam_str_cat(out, "END\n");
}

static tam_str_t build_tags(const struct tp_server *s) {
	tam_str_t out = tam_str_new("OK\n");
	struct tp_monitor *m;
	wl_list_for_each(m, &s->monitors, link) {
		tp_tags_t occupied = 0;
		struct tp_client *c;
		wl_list_for_each(c, &s->clients, link) {
			if (c->mapped && c->mon == m && !c->scratchpad)
				occupied |= c->tags;
		}
		const char *name =
			(m->output && m->output->name) ? m->output->name : "";
		out = tam_str_cat(out, "tag\tmonitor=");
		out = tp_ipc_escape_append(out, name);
		out = tam_str_cat_printf(
			out, "\tview=%u\tprev_view=%u\toccupied=%u\tfocused=%d\n",
			tp_tagview_current(&m->view), m->view.slot[m->view.cur ^ 1u],
			occupied, s->selmon == m ? 1 : 0);
		if (!out)
			return NULL;
	}
	return tam_str_cat(out, "END\n");
}

static tam_str_t build_cursor(struct tp_server *s) {
	int x = 0, y = 0;
	const char *mon_name = "";
	if (s->cursor) {
		x = (int)s->cursor->x;
		y = (int)s->cursor->y;
		struct tp_monitor *m = tp_monitor_at(s, s->cursor->x, s->cursor->y);
		if (m && m->output && m->output->name)
			mon_name = m->output->name;
	}
	tam_str_t out = tam_str_new("OK\ncursor\t");
	out = tam_str_cat_printf(out, "x=%d\ty=%d\tmonitor=", x, y);
	out = tp_ipc_escape_append(out, mon_name);
	return tam_str_cat(out, "\nEND\n");
}

struct bind_iter_ctx {
	tam_str_t out;
};

static void append_mods(tam_str_t *outp, uint32_t mods) {
	if (!*outp)
		return;
	if (mods == 0) {
		*outp = tam_str_cat(*outp, "none");
		return;
	}
	bool first = true;
	static const struct {
		uint32_t bit;
		const char *name;
	} table[] = {
		{TP_MOD_META, "Meta"},
		{TP_MOD_CTRL, "Ctrl"},
		{TP_MOD_ALT, "Alt"},
		{TP_MOD_SHIFT, "Shift"},
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (mods & table[i].bit) {
			if (!first)
				*outp = tam_str_cat_len(*outp, "+", 1);
			*outp = tam_str_cat(*outp, table[i].name);
			first = false;
			if (!*outp)
				return;
		}
	}
}

static void on_each_bind(const struct tp_lua_bind_info *info, void *userdata) {
	struct bind_iter_ctx *ctx = userdata;
	if (!ctx->out)
		return;
	char key_name[64];
	if (info->keycode > 0) {
		snprintf(key_name, sizeof(key_name), "code:%u", info->keycode);
	} else if (xkb_keysym_get_name(info->sym, key_name, sizeof(key_name)) < 0) {
		snprintf(key_name, sizeof(key_name), "0x%x", (unsigned)info->sym);
	}

	ctx->out = tam_str_cat(ctx->out, "bind\tmods=");
	append_mods(&ctx->out, info->mods);
	ctx->out = tam_str_cat(ctx->out, "\tkey=");
	ctx->out = tp_ipc_escape_append(ctx->out, key_name);
	ctx->out = tam_str_cat_printf(ctx->out, "\tlocked=%d\taction=",
								  info->allow_locked ? 1 : 0);
	if (info->is_lua)
		ctx->out = tam_str_cat(ctx->out, "lua");
	else
		ctx->out = tp_ipc_format_action(ctx->out, &info->action);
	if (info->desc[0]) {
		ctx->out = tam_str_cat(ctx->out, "\tdesc=");
		ctx->out = tp_ipc_escape_append(ctx->out, info->desc);
	}
	ctx->out = tam_str_cat_len(ctx->out, "\n", 1);
}

static tam_str_t build_binds(const struct tp_server *s) {
	struct bind_iter_ctx ctx = {.out = tam_str_new("OK\n")};
	if (s->lua)
		tp_lua_for_each_bind(s->lua, on_each_bind, &ctx);
	return tam_str_cat(ctx.out, "END\n");
}

static tam_str_t build_keyboards(const struct tp_server *s) {
	tam_str_t out = tam_str_new("OK\n");
	struct wlr_keyboard *seat_kb =
		s->seat ? wlr_seat_get_keyboard(s->seat) : NULL;
	struct tp_keyboard *k;
	wl_list_for_each(k, &s->keyboards, link) {
		if (!k->wlr)
			continue;
		const char *name = k->wlr->base.name ? k->wlr->base.name : "";
		uint32_t group = k->wlr->modifiers.group;
		const char *active_keymap = "";
		tam_str_t layouts = tam_str_new("");
		if (k->wlr->keymap) {
			xkb_layout_index_t num = xkb_keymap_num_layouts(k->wlr->keymap);
			if (num > 0) {
				if (group >= num)
					group = 0;
				const char *ln =
					xkb_keymap_layout_get_name(k->wlr->keymap, group);
				if (ln)
					active_keymap = ln;
				for (xkb_layout_index_t i = 0; i < num; i++) {
					if (i > 0)
						layouts = tam_str_cat_len(layouts, ",", 1);
					const char *item =
						xkb_keymap_layout_get_name(k->wlr->keymap, i);
					layouts = tam_str_cat(layouts, item ? item : "");
				}
			}
		}
		out = tam_str_cat(out, "keyboard\tname=");
		out = tp_ipc_escape_append(out, name);
		out = tam_str_cat(out, "\tlayout=");
		out = tp_ipc_escape_append(out, layouts ? layouts : "");
		out = tam_str_cat(out, "\tactive_keymap=");
		out = tp_ipc_escape_append(out, active_keymap);
		out = tam_str_cat_printf(out, "\tactive_layout_index=%u\tmain=%d\n",
								 group, k->wlr == seat_kb ? 1 : 0);
		tam_str_free(layouts);
		if (!out)
			return NULL;
	}
	return tam_str_cat(out, "END\n");
}

static tam_str_t build_chord(const struct tp_server *s) {
	const struct tp_chord_state *st = &s->chord;
	char cbuf[64];
	tp_chord_format(st->mods, st->sym, cbuf, sizeof(cbuf));
	tam_str_t out = tam_str_new("OK\nchord\tmode=");
	out = tp_ipc_escape_append(out, tp_chord_mode_name(st->mode));
	out = tam_str_cat(out, "\tchord=");
	out = tp_ipc_escape_append(out, cbuf);
	out = tam_str_cat(out, "\tfilter=");
	out = tp_ipc_escape_append(out, st->filter);
	out = tam_str_cat_printf(out, "\tmatches=%zu\tselected=%zu\n",
							 st->filtered_len, st->selected);
	return tam_str_cat(out, "END\n");
}

/* ---- Request handling ---- */

static bool handle_line(struct tp_ipc_client *c, char *line) {
	struct tp_ipc_server *ipc = c->ipc;
	struct tp_server *s = ipc->server;

	while (*line == ' ' || *line == '\t')
		line++;
	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
					   line[len - 1] == '\r'))
		line[--len] = '\0';
	if (len == 0)
		return ipc_send_err(c, "empty command");

	char *sp = line;
	while (*sp && *sp != ' ' && *sp != '\t')
		sp++;
	const char *args = "";
	if (*sp) {
		*sp = '\0';
		args = sp + 1;
		while (*args == ' ' || *args == '\t')
			args++;
	}

	if (strcmp(line, "version") == 0 || strcmp(line, "monitors") == 0 ||
		strcmp(line, "windows") == 0 || strcmp(line, "focused") == 0 ||
		strcmp(line, "tags") == 0 || strcmp(line, "cursor") == 0 ||
		strcmp(line, "binds") == 0 || strcmp(line, "keyboards") == 0 ||
		strcmp(line, "chord") == 0) {
		if (*args)
			return ipc_send_err(c, "command takes no arguments");
		tam_str_t reply = NULL;
		if (strcmp(line, "version") == 0)
			reply = build_version(s);
		else if (strcmp(line, "monitors") == 0)
			reply = build_monitors(s);
		else if (strcmp(line, "windows") == 0) {
			tp_arrange_flush(s);
			reply = build_windows(s);
		} else if (strcmp(line, "focused") == 0) {
			tp_arrange_flush(s);
			reply = build_focused(s);
		} else if (strcmp(line, "tags") == 0)
			reply = build_tags(s);
		else if (strcmp(line, "cursor") == 0)
			reply = build_cursor(s);
		else if (strcmp(line, "binds") == 0)
			reply = build_binds(s);
		else if (strcmp(line, "keyboards") == 0)
			reply = build_keyboards(s);
		else
			reply = build_chord(s);
		if (!reply)
			return ipc_send_err(c, "out of memory");
		bool ok = ipc_client_queue(c, reply, tam_str_len(reply));
		tam_str_free(reply);
		return ok;
	}

	if (strcmp(line, "lock") == 0) {
		if (*args)
			return ipc_send_err(c, "lock takes no arguments");
		if (s->locker_client)
			return ipc_send_err(c, "locker already running");
		if (!s->settings.locker[0])
			return ipc_send_err(c, "no locker configured");
		const char *argv[] = {s->settings.locker, NULL};
		if (access(argv[0], X_OK) != 0 || !tp_session_lock_launch_argv(s, argv))
			return ipc_send_err(c, "could not start configured locker");
		return ipc_client_queue(c, "OK\nEND\n", 7);
	}

	if (strcmp(line, "reload") == 0) {
		if (*args)
			return ipc_send_err(c, "reload takes no arguments");
		char err[512] = {0};
		if (!tp_server_reload(s, err, sizeof(err)))
			return ipc_send_err(c, err[0] ? err : "reload failed");
		return ipc_client_queue(c, "OK\nEND\n", 7);
	}

	if (strcmp(line, "dispatch") == 0) {
		if (s->locked)
			return ipc_send_err(c, "session is locked");
		struct tp_action act;
		char mon_name[128];
		char err[256] = {0};
		if (!tp_ipc_parse_dispatch(args, &act, mon_name, sizeof(mon_name), err,
								   sizeof(err)))
			return ipc_send_err(c, err[0] ? err : "invalid dispatch");

		if ((act.kind == TP_ACT_MONITOR_FOCUS ||
			 act.kind == TP_ACT_MONITOR_MOVE) &&
			act.dir == 0) {
			struct tp_monitor *m = tp_monitor_by_name(s, mon_name);
			if (!m)
				return ipc_send_err(c, "unknown monitor");
			if (act.kind == TP_ACT_MONITOR_FOCUS) {
				tp_monitor_focus(s, m);
			} else {
				struct tp_client *fc = tp_focused(s);
				if (fc)
					tp_client_set_monitor(fc, m, false);
			}
			return ipc_client_queue(c, "OK\nEND\n", 7);
		}

		if (act.kind == TP_ACT_MONITOR_SCALE) {
			if (!tp_monitor_set_scale(s, act.cmd, act.scale))
				return ipc_send_err(c, "monitor scale failed");
			return ipc_client_queue(c, "OK\nEND\n", 7);
		}

		if (act.kind == TP_ACT_MONITOR_MODE) {
			char why[128] = {0};
			if (!tp_monitor_set_mode(s, act.cmd, act.mode_w, act.mode_h,
									 act.mode_refresh, why, sizeof(why)))
				return ipc_send_err(c, why[0] ? why : "monitor mode failed");
			return ipc_client_queue(c, "OK\nEND\n", 7);
		}

		if (act.kind == TP_ACT_DPMS) {
			struct tp_monitor *m = tp_monitor_by_name(s, mon_name);
			if (!m || !m->output)
				return ipc_send_err(c, "unknown monitor");
			if (!tp_output_power_set(s, m->output, act.dir != 0))
				return ipc_send_err(c, "dpms failed");
			return ipc_client_queue(c, "OK\nEND\n", 7);
		}

		if ((act.kind == TP_ACT_TAG_VIEW ||
			 act.kind == TP_ACT_TAG_TOGGLE_VIEW) &&
			mon_name[0] != '\0') {
			struct tp_monitor *m = tp_monitor_by_name(s, mon_name);
			if (!m)
				return ipc_send_err(c, "unknown monitor");
			tp_tags_t prev = tp_tagview_current(&m->view);
			bool changed =
				act.kind == TP_ACT_TAG_VIEW
					? tp_tagview_show(&m->view, tp_tag_mask(act.tag))
					: tp_tagview_toggle(&m->view, tp_tag_mask(act.tag));
			if (changed) {
				tp_pointer_cancel_grab(s, NULL);
				tp_arrange(m);
				tp_events_emit_tag(s, m, prev, tp_tagview_current(&m->view));
				tp_workspace_sync_monitor(m);
				if (s->selmon == m)
					tp_focus_refresh(s);
			}
			return ipc_client_queue(c, "OK\nEND\n", 7);
		}

		if (act.kind == TP_ACT_QUIT) {
			/* Send reply before requesting termination. */
			ipc_client_queue(c, "OK\nEND\n", 7);
			tp_action_run(s, &act);
			return true;
		}
		tp_action_run(s, &act);
		return ipc_client_queue(c, "OK\nEND\n", 7);
	}

	if (strcmp(line, "subscribe") == 0) {
		uint32_t mask = 0;
		char err[128] = {0};
		if (!tp_ipc_parse_subscribe(args, &mask, err, sizeof(err)))
			return ipc_send_err(c, err[0] ? err : "invalid subscribe");

		for (int k = 0; k < TP_EVENT_COUNT; k++) {
			bool had = (c->sub_mask & (1u << k)) != 0;
			bool want = (mask & (1u << k)) != 0;
			if (!had && want)
				ipc->sub_counts[k]++;
			else if (had && !want && ipc->sub_counts[k] > 0)
				ipc->sub_counts[k]--;
		}
		c->sub_mask = mask;
		return ipc_client_queue(c, "OK\n", 3);
	}

	return ipc_send_err(c, "unknown command");
}

static int on_client_event(int fd, uint32_t mask, void *data) {
	struct tp_ipc_client *c = data;

	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		ipc_client_destroy(c);
		return 0;
	}

	if (mask & WL_EVENT_WRITABLE) {
		if (!ipc_client_flush(c)) {
			ipc_client_destroy(c);
			return 0;
		}
		if (c->close_after_write && (!c->wbuf || c->woff >= tam_str_len(c->wbuf))) {
			ipc_client_destroy(c);
			return 0;
		}
		ipc_client_update_events(c);
	}

	if ((mask & WL_EVENT_READABLE) && !c->close_after_write) {
		for (;;) {
			size_t avail = TP_IPC_MAX_LINE - c->rlen;
			if (avail == 0) {
				ipc_send_err(c, "line too long");
				c->close_after_write = true;
				if (!c->wbuf || c->woff >= tam_str_len(c->wbuf)) {
					ipc_client_destroy(c);
					return 0;
				}
				ipc_client_update_events(c);
				return 0;
			}
			ssize_t n = read(fd, c->rbuf + c->rlen, avail);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				ipc_client_destroy(c);
				return 0;
			}
			if (n == 0) {
				ipc_client_destroy(c);
				return 0;
			}
			if (memchr(c->rbuf + c->rlen, '\0', (size_t)n) != NULL) {
				ipc_send_err(c, "NUL byte in request");
				c->close_after_write = true;
				if (!c->wbuf || c->woff >= tam_str_len(c->wbuf)) {
					ipc_client_destroy(c);
					return 0;
				}
				ipc_client_update_events(c);
				return 0;
			}
			c->rlen += (size_t)n;

			char *nl;
			while ((nl = memchr(c->rbuf, '\n', c->rlen)) != NULL) {
				size_t line_len = (size_t)(nl - c->rbuf);
				*nl = '\0';
				bool ok = handle_line(c, c->rbuf);
				size_t consumed = line_len + 1;
				c->rlen -= consumed;
				if (c->rlen > 0)
					memmove(c->rbuf, c->rbuf + consumed, c->rlen);
				if (!ok) {
					ipc_client_destroy(c);
					return 0;
				}
			}
		}
	}

	return 0;
}

static int on_accept(int fd, uint32_t mask, void *data) {
	struct tp_ipc_server *ipc = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP))
		return 0;

	for (;;) {
		int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			tam_log_warn("ipc accept: %s", strerror(errno));
			break;
		}

		uid_t peer;
		if (tam_sock_peer_uid(cfd, &peer) < 0 || peer != getuid()) {
			tam_log_warn("ipc: rejected peer (uid check failed)");
			close(cfd);
			continue;
		}

		if (ipc->client_count >= TP_IPC_MAX_CLIENTS) {
			tam_log_warn("ipc: client limit (%d) reached", TP_IPC_MAX_CLIENTS);
			close(cfd);
			continue;
		}

		struct tp_ipc_client *c = calloc(1, sizeof(*c));
		if (!c) {
			close(cfd);
			continue;
		}
		c->ipc = ipc;
		c->fd = cfd;
		c->source = wl_event_loop_add_fd(
			ipc->server->loop, cfd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
			on_client_event, c);
		if (!c->source) {
			close(cfd);
			free(c);
			continue;
		}
		wl_list_insert(&ipc->clients, &c->link);
		ipc->client_count++;
	}
	return 0;
}

bool tp_ipc_start(struct tp_server *s) {
	if (!s || !s->loop || !s->socket)
		return false;

	const char *rdir = getenv("XDG_RUNTIME_DIR");
	char path[PATH_MAX];
	if (!tp_ipc_socket_path(path, sizeof(path), rdir, s->socket)) {
		tam_log_error("ipc: cannot build socket path for '%s'", s->socket);
		return false;
	}

	/* $XDG_RUNTIME_DIR/tam-pane: created 0700, and refused unless it and
	 * the runtime directory are real directories owned by us and closed
	 * to everyone else. */
	char *dir = tam_runtime_dir("tam-pane");
	if (!dir) {
		tam_log_error("ipc: runtime directory: %s", strerror(errno));
		return false;
	}
	free(dir);

	/* A socket left by a compositor that died is replaced; a live one, or
	 * anything that is not a socket, is never removed. */
	int fd = tam_sock_listen(path, 0600, 16);
	if (fd < 0) {
		tam_log_error("ipc: listen %s: %s", path,
					  errno == EADDRINUSE ? "another compositor is using it"
										  : strerror(errno));
		return false;
	}
	int fl = fcntl(fd, F_GETFL);
	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
		tam_log_error("ipc: %s: %s", path, strerror(errno));
		close(fd);
		unlink(path);
		return false;
	}

	struct tp_ipc_server *ipc = calloc(1, sizeof(*ipc));
	if (!ipc) {
		close(fd);
		unlink(path);
		return false;
	}
	ipc->server = s;
	ipc->listen_fd = fd;
	snprintf(ipc->socket_path, sizeof(ipc->socket_path), "%s", path);
	ipc->socket_created = true;
	wl_list_init(&ipc->clients);

	ipc->listen_source = wl_event_loop_add_fd(
		s->loop, fd, WL_EVENT_READABLE | WL_EVENT_ERROR, on_accept, ipc);
	if (!ipc->listen_source) {
		close(fd);
		unlink(path);
		free(ipc);
		return false;
	}

	s->ipc = ipc;
	setenv("TAM_PANE_SOCKET", ipc->socket_path, 1);
	return true;
}

void tp_ipc_finish(struct tp_server *s) {
	if (!s || !s->ipc)
		return;
	struct tp_ipc_server *ipc = s->ipc;
	s->ipc = NULL;

	struct tp_ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &ipc->clients, link) {
		ipc_client_destroy(c);
	}
	if (ipc->listen_source)
		wl_event_source_remove(ipc->listen_source);
	if (ipc->listen_fd >= 0)
		close(ipc->listen_fd);
	if (ipc->socket_created && ipc->socket_path[0])
		unlink(ipc->socket_path);
	free(ipc);
}

bool tp_ipc_has_subscribers(const struct tp_server *s,
							enum tp_event_kind kind) {
	if (!s || !s->ipc || kind >= TP_EVENT_COUNT)
		return false;
	return s->ipc->sub_counts[kind] > 0;
}

void tp_ipc_emit_event(struct tp_server *s, const struct tp_event *ev) {
	if (!s || !s->ipc || !ev || ev->kind >= TP_EVENT_COUNT)
		return;
	struct tp_ipc_server *ipc = s->ipc;
	if (ipc->sub_counts[ev->kind] == 0)
		return;

	tam_str_t line = tam_str_new("");
	if (!line)
		return;
	line = tp_ipc_format_event(line, ev);
	if (!line || tam_str_len(line) == 0) {
		tam_str_free(line);
		return;
	}

	uint32_t bit = 1u << ev->kind;
	struct tp_ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &ipc->clients, link) {
		if ((c->sub_mask & bit) == 0)
			continue;
		if (!ipc_client_queue(c, line, tam_str_len(line))) {
			tam_log_warn("ipc: disconnecting slow subscriber");
			ipc_client_destroy(c);
		}
	}
	tam_str_free(line);
}
