/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure IPC wire helpers and the `tam-pane msg` CLI client (no wlroots).
 */
#include "tam-pane/ipc.h"
#include "tam-pane/layout/layout.h"
#include "tam-pane/tags.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static uint32_t all_subscribable_mask(void) {
	return (1u << TP_EVENT_WINDOW_OPEN) | (1u << TP_EVENT_WINDOW_CLOSE) |
		   (1u << TP_EVENT_WINDOW_FOCUS) | (1u << TP_EVENT_TAG_CHANGE) |
		   (1u << TP_EVENT_MONITOR_ADD) | (1u << TP_EVENT_MONITOR_REMOVE) |
		   (1u << TP_EVENT_MONITOR_FOCUS) | (1u << TP_EVENT_OUTPUT_POWER) |
		   (1u << TP_EVENT_KEYBOARD_LAYOUT) | (1u << TP_EVENT_CONFIG_RELOAD) |
		   (1u << TP_EVENT_CHORD_START) | (1u << TP_EVENT_CHORD_UPDATE) |
		   (1u << TP_EVENT_CHORD_END) | (1u << TP_EVENT_CHORD_CANCEL);
}

bool tp_ipc_socket_path(char *out, size_t out_size, const char *runtime_dir,
						const char *wayland_socket) {
	if (!out || out_size == 0 || !runtime_dir || !*runtime_dir ||
		!wayland_socket || !*wayland_socket)
		return false;
	if (strchr(wayland_socket, '/') != NULL ||
		strcmp(wayland_socket, ".") == 0 || strcmp(wayland_socket, "..") == 0)
		return false;

	size_t rlen = strlen(runtime_dir);
	while (rlen > 1 && runtime_dir[rlen - 1] == '/')
		rlen--;

	struct sockaddr_un probe;
	size_t max_len = sizeof(probe.sun_path);
	if (out_size < max_len)
		max_len = out_size;

	int n = snprintf(out, max_len, "%.*s/tam-pane/%s.sock", (int)rlen,
					 runtime_dir, wayland_socket);
	if (n < 0 || (size_t)n >= max_len) {
		out[0] = '\0';
		return false;
	}
	return true;
}

tam_str_t tp_ipc_escape_append(tam_str_t s, const char *val) {
	if (!s || !val)
		return s;
	for (const unsigned char *p = (const unsigned char *)val; *p; p++) {
		switch (*p) {
		case '\\':
			s = tam_str_cat_len(s, "\\\\", 2);
			break;
		case '\t':
			s = tam_str_cat_len(s, "\\t", 2);
			break;
		case '\n':
			s = tam_str_cat_len(s, "\\n", 2);
			break;
		case '\r':
			s = tam_str_cat_len(s, "\\r", 2);
			break;
		default:
			if (*p < 0x20 || *p == 0x7f) {
				char sp = ' ';
				s = tam_str_cat_len(s, &sp, 1);
			} else {
				s = tam_str_cat_len(s, (const char *)p, 1);
			}
			break;
		}
		if (!s)
			return NULL;
	}
	return s;
}

tam_str_t tp_ipc_format_event(tam_str_t s, const struct tp_event *ev) {
	if (!s || !ev)
		return s;
	const char *name = tp_event_kind_name(ev->kind);
	if (!name)
		return s;

	switch (ev->kind) {
	case TP_EVENT_WINDOW_OPEN:
	case TP_EVENT_WINDOW_CLOSE:
	case TP_EVENT_WINDOW_FOCUS:
		s = tam_str_cat_printf(s, "event\t%s\tmonitor=", name);
		s = tp_ipc_escape_append(s, ev->window.monitor);
		s = tam_str_cat_printf(
			s,
			"\ttags=%u\tfocused=%d\tfloating=%d\tfullscreen=%d"
			"\tx=%d\ty=%d\tw=%d\th=%d\tapp_id=",
			ev->window.tags, ev->window.focused ? 1 : 0,
			ev->window.floating ? 1 : 0, ev->window.fullscreen ? 1 : 0,
			ev->window.geom.x, ev->window.geom.y, ev->window.geom.w,
			ev->window.geom.h);
		s = tp_ipc_escape_append(s, ev->window.app_id);
		s = tam_str_cat(s, "\ttitle=");
		s = tp_ipc_escape_append(s, ev->window.title);
		s = tam_str_cat_len(s, "\n", 1);
		return s;
	case TP_EVENT_TAG_CHANGE:
		s = tam_str_cat(s, "event\ttag.change\tmonitor=");
		s = tp_ipc_escape_append(s, ev->tag.monitor);
		s = tam_str_cat_printf(s, "\ttags=%u\tprev_tags=%u\n", ev->tag.tags,
							   ev->tag.prev_tags);
		return s;
	case TP_EVENT_MONITOR_ADD:
	case TP_EVENT_MONITOR_REMOVE:
		s = tam_str_cat_printf(s, "event\t%s\tname=", name);
		s = tp_ipc_escape_append(s, ev->monitor.name);
		s = tam_str_cat_printf(s, "\tx=%d\ty=%d\tw=%d\th=%d\tscale=%.2f\n",
							   ev->monitor.area.x, ev->monitor.area.y,
							   ev->monitor.area.w, ev->monitor.area.h,
							   ev->monitor.scale);
		return s;
	case TP_EVENT_MONITOR_FOCUS:
		s = tam_str_cat(s, "event\tmonitor.focus\tmonitor=");
		s = tp_ipc_escape_append(s, ev->monitor.name);
		return tam_str_cat_len(s, "\n", 1);
	case TP_EVENT_OUTPUT_POWER:
		s = tam_str_cat(s, "event\toutput.power\tmonitor=");
		s = tp_ipc_escape_append(s, ev->output_power.monitor);
		return tam_str_cat_printf(s, "\tdpms=%d\n",
								  ev->output_power.dpms ? 1 : 0);
	case TP_EVENT_KEYBOARD_LAYOUT:
		s = tam_str_cat(s, "event\tkeyboard.layout\tname=");
		s = tp_ipc_escape_append(s, ev->keyboard.name);
		s = tam_str_cat(s, "\tactive_keymap=");
		s = tp_ipc_escape_append(s, ev->keyboard.active_keymap);
		return tam_str_cat_printf(s, "\tactive_layout_index=%u\n",
								  ev->keyboard.active_layout_index);
	case TP_EVENT_CONFIG_RELOAD:
		return tam_str_cat(s, "event\tconfig.reload\n");
	case TP_EVENT_CHORD_START:
	case TP_EVENT_CHORD_UPDATE:
	case TP_EVENT_CHORD_END:
	case TP_EVENT_CHORD_CANCEL:
		s = tam_str_cat_printf(s, "event\t%s\tmode=", name);
		s = tp_ipc_escape_append(s, ev->chord.mode);
		s = tam_str_cat(s, "\tchord=");
		s = tp_ipc_escape_append(s, ev->chord.chord);
		s = tam_str_cat(s, "\tfilter=");
		s = tp_ipc_escape_append(s, ev->chord.filter);
		s = tam_str_cat_printf(s, "\tmatches=%zu\tselected=%zu\tresult=",
							   ev->chord.matches, ev->chord.selected);
		s = tp_ipc_escape_append(s, ev->chord.result);
		s = tam_str_cat(s, "\taction=");
		s = tp_ipc_escape_append(s, ev->chord.action);
		return tam_str_cat_len(s, "\n", 1);
	case TP_EVENT_STARTUP:
		return tam_str_cat(s, "event\tstartup\n");
	case TP_EVENT_COUNT:
		break;
	}
	return s;
}

static const char *skip_ws(const char *p) {
	while (p && (*p == ' ' || *p == '\t'))
		p++;
	return p;
}

static bool next_token(const char **cursor, char *buf, size_t buf_size) {
	const char *p = skip_ws(*cursor);
	if (!p || !*p) {
		*cursor = p;
		return false;
	}
	const char *start = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;
	size_t len = (size_t)(p - start);
	if (len >= buf_size)
		len = buf_size - 1;
	memcpy(buf, start, len);
	buf[len] = '\0';
	*cursor = p;
	return true;
}

static bool has_extra_tokens(const char *cursor) {
	const char *p = skip_ws(cursor);
	return p && *p != '\0';
}

bool tp_ipc_parse_subscribe(const char *args, uint32_t *out_mask, char *err,
							size_t err_size) {
	if (!out_mask)
		return false;
	const char *p = skip_ws(args ? args : "");
	if (!*p) {
		*out_mask = all_subscribable_mask();
		return true;
	}

	uint32_t mask = 0;
	char tok[64];
	while (next_token(&p, tok, sizeof(tok))) {
		if (strcmp(tok, "all") == 0) {
			mask |= all_subscribable_mask();
			continue;
		}
		if (strcmp(tok, "chord") == 0) {
			mask |= (1u << TP_EVENT_CHORD_START) |
					(1u << TP_EVENT_CHORD_UPDATE) |
					(1u << TP_EVENT_CHORD_END) | (1u << TP_EVENT_CHORD_CANCEL);
			continue;
		}
		enum tp_event_kind kind;
		if (!tp_event_kind_from_name(tok, &kind) ||
			kind == TP_EVENT_STARTUP || kind >= TP_EVENT_COUNT) {
			if (err && err_size > 0)
				snprintf(err, err_size, "unknown event '%s'", tok);
			return false;
		}
		mask |= (1u << kind);
	}
	if (mask == 0)
		mask = all_subscribable_mask();
	*out_mask = mask;
	return true;
}

static bool parse_cardinal_dir(const char *s, enum tp_dir *out) {
	if (strcmp(s, "left") == 0) {
		*out = TP_DIR_LEFT;
		return true;
	}
	if (strcmp(s, "right") == 0) {
		*out = TP_DIR_RIGHT;
		return true;
	}
	if (strcmp(s, "up") == 0) {
		*out = TP_DIR_UP;
		return true;
	}
	if (strcmp(s, "down") == 0) {
		*out = TP_DIR_DOWN;
		return true;
	}
	return false;
}

static const char *cardinal_dir_name(int dir) {
	switch ((enum tp_dir)dir) {
	case TP_DIR_LEFT:
		return "left";
	case TP_DIR_RIGHT:
		return "right";
	case TP_DIR_UP:
		return "up";
	case TP_DIR_DOWN:
		return "down";
	}
	return "?";
}

static bool parse_positive_int(const char *s, long min_v, long max_v,
							   long *out) {
	if (!s || !*s || isspace((unsigned char)s[0]) || s[0] == '+' || s[0] == '-')
		return false;
	char *end = NULL;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno != 0 || !end || *end != '\0')
		return false;
	if (v < min_v || v > max_v)
		return false;
	*out = v;
	return true;
}

bool tp_ipc_parse_dispatch(const char *args, struct tp_action *out,
						   char *mon_name, size_t mon_name_size, char *err,
						   size_t err_size) {
	if (!out)
		return false;
	memset(out, 0, sizeof(*out));
	if (mon_name && mon_name_size > 0)
		mon_name[0] = '\0';

	const char *p = args ? args : "";
	char verb[64];
	if (!next_token(&p, verb, sizeof(verb))) {
		if (err && err_size > 0)
			snprintf(err, err_size, "dispatch requires an action name");
		return false;
	}

	if (strcmp(verb, "terminal") == 0 || strcmp(verb, "close") == 0 ||
		strcmp(verb, "quit") == 0 || strcmp(verb, "reload") == 0 ||
		strcmp(verb, "toggle_float") == 0 ||
		strcmp(verb, "toggle_fullscreen") == 0 ||
		strcmp(verb, "chord_toggle") == 0 ||
		strcmp(verb, "chord_cancel") == 0) {
		if (has_extra_tokens(p)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "%s takes no arguments", verb);
			return false;
		}
		if (strcmp(verb, "terminal") == 0)
			out->kind = TP_ACT_TERMINAL;
		else if (strcmp(verb, "close") == 0)
			out->kind = TP_ACT_CLOSE;
		else if (strcmp(verb, "quit") == 0)
			out->kind = TP_ACT_QUIT;
		else if (strcmp(verb, "reload") == 0)
			out->kind = TP_ACT_RELOAD;
		else if (strcmp(verb, "toggle_float") == 0)
			out->kind = TP_ACT_TOGGLE_FLOAT;
		else if (strcmp(verb, "toggle_fullscreen") == 0)
			out->kind = TP_ACT_TOGGLE_FULLSCREEN;
		else if (strcmp(verb, "chord_toggle") == 0)
			out->kind = TP_ACT_CHORD_TOGGLE;
		else
			out->kind = TP_ACT_CHORD_CANCEL;
		return true;
	}

	if (strcmp(verb, "focus") == 0) {
		char arg[32];
		if (!next_token(&p, arg, sizeof(arg)) || has_extra_tokens(p)) {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "focus expects 'next', 'prev', 'left', 'right', 'up', or 'down'");
			return false;
		}
		if (strcmp(arg, "next") == 0) {
			out->kind = TP_ACT_FOCUS;
			out->dir = 1;
			return true;
		}
		if (strcmp(arg, "prev") == 0) {
			out->kind = TP_ACT_FOCUS;
			out->dir = -1;
			return true;
		}
		enum tp_dir cdir;
		if (parse_cardinal_dir(arg, &cdir)) {
			out->kind = TP_ACT_FOCUS_DIR;
			out->dir = (int)cdir;
			return true;
		}
		if (err && err_size > 0)
			snprintf(err, err_size,
					 "focus expects 'next', 'prev', 'left', 'right', 'up', or 'down'");
		return false;
	}

	if (strcmp(verb, "swap") == 0 || strcmp(verb, "move") == 0) {
		char arg[32];
		enum tp_dir dir;
		if (!next_token(&p, arg, sizeof(arg)) ||
			!parse_cardinal_dir(arg, &dir) || has_extra_tokens(p)) {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "%s expects 'left', 'right', 'up', or 'down'", verb);
			return false;
		}
		out->kind = strcmp(verb, "swap") == 0 ? TP_ACT_SWAP : TP_ACT_MOVE;
		out->dir = (int)dir;
		return true;
	}

	if (strcmp(verb, "resize") == 0) {
		char arg[32], step_tok[32];
		enum tp_dir dir;
		if (!next_token(&p, arg, sizeof(arg)) ||
			!parse_cardinal_dir(arg, &dir)) {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "resize expects 'left', 'right', 'up', or 'down'");
			return false;
		}
		out->kind = TP_ACT_RESIZE;
		out->dir = (int)dir;
		if (next_token(&p, step_tok, sizeof(step_tok))) {
			long step = 0;
			if (!parse_positive_int(step_tok, 1, 4096, &step) ||
				has_extra_tokens(p)) {
				if (err && err_size > 0)
					snprintf(err, err_size,
							 "resize step must be an integer 1..4096");
				return false;
			}
			out->step = (int32_t)step;
		}
		return true;
	}

	if (strcmp(verb, "tag") == 0) {
		char sub[32], num_tok[32];
		if (!next_token(&p, sub, sizeof(sub))) {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "tag expects view, toggle_view, move, toggle_client, "
						 "or back");
			return false;
		}
		if (strcmp(sub, "back") == 0) {
			if (has_extra_tokens(p)) {
				if (err && err_size > 0)
					snprintf(err, err_size, "tag back takes no arguments");
				return false;
			}
			out->kind = TP_ACT_TAG_BACK;
			return true;
		}
		if (strcmp(sub, "view") == 0)
			out->kind = TP_ACT_TAG_VIEW;
		else if (strcmp(sub, "toggle_view") == 0)
			out->kind = TP_ACT_TAG_TOGGLE_VIEW;
		else if (strcmp(sub, "move") == 0)
			out->kind = TP_ACT_TAG_MOVE;
		else if (strcmp(sub, "toggle_client") == 0)
			out->kind = TP_ACT_TAG_TOGGLE_CLIENT;
		else {
			if (err && err_size > 0)
				snprintf(err, err_size, "unknown tag action '%s'", sub);
			return false;
		}
		long tag_n = 0;
		if (!next_token(&p, num_tok, sizeof(num_tok)) ||
			!parse_positive_int(num_tok, 1, TP_TAG_COUNT, &tag_n)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "tag %s expects a tag number 1..%d",
						 sub, TP_TAG_COUNT);
			return false;
		}
		if (out->kind == TP_ACT_TAG_VIEW ||
			out->kind == TP_ACT_TAG_TOGGLE_VIEW) {
			char target[128];
			if (next_token(&p, target, sizeof(target))) {
				if (has_extra_tokens(p)) {
					if (err && err_size > 0)
						snprintf(err, err_size,
								 "tag %s takes at most a tag and monitor name",
								 sub);
					return false;
				}
				if (mon_name && mon_name_size > 0)
					snprintf(mon_name, mon_name_size, "%s", target);
			}
		} else if (has_extra_tokens(p)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "tag %s expects a tag number 1..%d",
						 sub, TP_TAG_COUNT);
			return false;
		}
		out->tag = (unsigned int)tag_n;
		return true;
	}

	if (strcmp(verb, "monitor") == 0) {
		char sub[32], target[128];
		if (!next_token(&p, sub, sizeof(sub))) {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "monitor expects 'focus', 'move', or 'scale'");
			return false;
		}
		if (strcmp(sub, "scale") == 0) {
			char scale_tok[32];
			if (!next_token(&p, target, sizeof(target)) ||
				!next_token(&p, scale_tok, sizeof(scale_tok)) ||
				has_extra_tokens(p)) {
				if (err && err_size > 0)
					snprintf(err, err_size,
							 "monitor scale expects <output|focused> <scale>");
				return false;
			}
			char *end = NULL;
			float sc = strtof(scale_tok, &end);
			if (!end || *end != '\0' || !(sc >= 0.5f && sc <= 8.0f)) {
				if (err && err_size > 0)
					snprintf(err, err_size, "scale out of range [0.5, 8.0]");
				return false;
			}
			out->kind = TP_ACT_MONITOR_SCALE;
			out->scale = sc;
			snprintf(out->cmd, sizeof(out->cmd), "%s", target);
			if (mon_name && mon_name_size > 0)
				snprintf(mon_name, mon_name_size, "%s", target);
			return true;
		}
		if (strcmp(sub, "focus") == 0)
			out->kind = TP_ACT_MONITOR_FOCUS;
		else if (strcmp(sub, "move") == 0)
			out->kind = TP_ACT_MONITOR_MOVE;
		else {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "monitor expects 'focus', 'move', or 'scale'");
			return false;
		}
		if (!next_token(&p, target, sizeof(target)) || has_extra_tokens(p)) {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "monitor %s expects 'next', 'prev', or an output name",
						 sub);
			return false;
		}
		if (strcmp(target, "next") == 0) {
			out->dir = 1;
		} else if (strcmp(target, "prev") == 0) {
			out->dir = -1;
		} else {
			out->dir = 0;
			if (mon_name && mon_name_size > 0)
				snprintf(mon_name, mon_name_size, "%s", target);
		}
		return true;
	}

	if (strcmp(verb, "layout") == 0) {
		char sub[32], arg[32];
		if (!next_token(&p, sub, sizeof(sub))) {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "layout expects 'set', 'next', 'prev', or 'nmaster'");
			return false;
		}
		if (strcmp(sub, "next") == 0 || strcmp(sub, "prev") == 0) {
			if (has_extra_tokens(p)) {
				if (err && err_size > 0)
					snprintf(err, err_size, "layout %s takes no arguments", sub);
				return false;
			}
			out->kind = TP_ACT_LAYOUT_CYCLE;
			out->dir = strcmp(sub, "next") == 0 ? 1 : -1;
			return true;
		}
		if (strcmp(sub, "set") == 0) {
			enum tp_layout_kind kind;
			if (!next_token(&p, arg, sizeof(arg)) ||
				!tp_layout_kind_from_name(arg, &kind) || has_extra_tokens(p)) {
				if (err && err_size > 0)
					snprintf(err, err_size,
							 "layout set expects 'dwindle', 'master', "
							 "'scroller', or 'monocle'");
				return false;
			}
			out->kind = TP_ACT_LAYOUT_SET;
			out->dir = (int)kind;
			return true;
		}
		if (strcmp(sub, "nmaster") == 0) {
			if (!next_token(&p, arg, sizeof(arg)) || has_extra_tokens(p)) {
				if (err && err_size > 0)
					snprintf(err, err_size,
							 "layout nmaster expects 'inc' or 'dec'");
				return false;
			}
			if (strcmp(arg, "inc") == 0 || strcmp(arg, "+1") == 0)
				out->dir = 1;
			else if (strcmp(arg, "dec") == 0 || strcmp(arg, "-1") == 0)
				out->dir = -1;
			else {
				if (err && err_size > 0)
					snprintf(err, err_size,
							 "layout nmaster expects 'inc' or 'dec'");
				return false;
			}
			out->kind = TP_ACT_LAYOUT_NMASTER;
			return true;
		}
		if (err && err_size > 0)
			snprintf(err, err_size, "unknown layout action '%s'", sub);
		return false;
	}

	if (strcmp(verb, "scratchpad") == 0) {
		char sub[32];
		if (!next_token(&p, sub, sizeof(sub)) || has_extra_tokens(p)) {
			if (err && err_size > 0)
				snprintf(err, err_size,
						 "scratchpad expects 'toggle' or 'move'");
			return false;
		}
		if (strcmp(sub, "toggle") == 0) {
			out->kind = TP_ACT_SCRATCHPAD_TOGGLE;
			return true;
		}
		if (strcmp(sub, "move") == 0) {
			out->kind = TP_ACT_SCRATCHPAD_MOVE;
			return true;
		}
		if (err && err_size > 0)
			snprintf(err, err_size, "scratchpad expects 'toggle' or 'move'");
		return false;
	}

	if (strcmp(verb, "dpms") == 0) {
		char mode[16], target[128];
		if (!next_token(&p, mode, sizeof(mode)) ||
			!next_token(&p, target, sizeof(target)) || has_extra_tokens(p)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "dpms expects 'on'|'off' and an output name");
			return false;
		}
		if (strcmp(mode, "on") == 0)
			out->dir = 1;
		else if (strcmp(mode, "off") == 0)
			out->dir = 0;
		else {
			if (err && err_size > 0)
				snprintf(err, err_size, "dpms expects 'on' or 'off'");
			return false;
		}
		out->kind = TP_ACT_DPMS;
		if (mon_name && mon_name_size > 0)
			snprintf(mon_name, mon_name_size, "%s", target);
		snprintf(out->cmd, sizeof(out->cmd), "%s", target);
		return true;
	}

	if (strcmp(verb, "focus_app") == 0) {
		const char *app = skip_ws(p);
		if (!app || !*app) {
			if (err && err_size > 0)
				snprintf(err, err_size, "focus_app requires an app name");
			return false;
		}
		size_t len = strlen(app);
		while (len > 0 && (app[len - 1] == ' ' || app[len - 1] == '\t'))
			len--;
		if (len == 0 || len >= sizeof(out->cmd)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "focus_app name too long");
			return false;
		}
		out->kind = TP_ACT_FOCUS_APP;
		memcpy(out->cmd, app, len);
		out->cmd[len] = '\0';
		return true;
	}

	if (strcmp(verb, "keyboard") == 0) {
		char sub[32], kbd[128];
		if (!next_token(&p, sub, sizeof(sub)) || strcmp(sub, "next") != 0) {
			if (err && err_size > 0)
				snprintf(err, err_size, "keyboard expects 'next' [name]");
			return false;
		}
		if (next_token(&p, kbd, sizeof(kbd))) {
			if (has_extra_tokens(p)) {
				if (err && err_size > 0)
					snprintf(err, err_size, "keyboard next takes at most one name");
				return false;
			}
			snprintf(out->cmd, sizeof(out->cmd), "%s", kbd);
		}
		out->kind = TP_ACT_KEYBOARD_NEXT;
		return true;
	}

	if (strcmp(verb, "send_key") == 0) {
		const char *chord = skip_ws(p);
		if (!chord || !*chord) {
			if (err && err_size > 0)
				snprintf(err, err_size, "send_key requires a chord");
			return false;
		}
		struct tp_key_spec spec;
		char perr[128] = {0};
		if (!tp_binding_parse_chord(chord, &spec, perr, sizeof(perr)) ||
			spec.sym == XKB_KEY_NoSymbol) {
			if (err && err_size > 0)
				snprintf(err, err_size, "send_key: %s",
						 perr[0] ? perr : "invalid chord");
			return false;
		}
		out->kind = TP_ACT_SEND_KEY;
		out->mods = spec.mods;
		out->sym = spec.sym;
		return true;
	}

	if (strcmp(verb, "capture") == 0) {
		const char *cmd = skip_ws(p);
		if (!cmd || !*cmd) {
			if (err && err_size > 0)
				snprintf(err, err_size, "capture requires a command");
			return false;
		}
		size_t len = strlen(cmd);
		while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t'))
			len--;
		if (len == 0 || len >= sizeof(out->cmd)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "capture command too long");
			return false;
		}
		out->kind = TP_ACT_CAPTURE;
		memcpy(out->cmd, cmd, len);
		out->cmd[len] = '\0';
		return true;
	}

	if (strcmp(verb, "spawn") == 0) {
		const char *cmd = skip_ws(p);
		if (!cmd || !*cmd) {
			if (err && err_size > 0)
				snprintf(err, err_size, "spawn requires a command");
			return false;
		}
		size_t len = strlen(cmd);
		while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t'))
			len--;
		if (len == 0 || len >= sizeof(out->cmd)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "spawn command too long");
			return false;
		}
		out->kind = TP_ACT_SPAWN;
		memcpy(out->cmd, cmd, len);
		out->cmd[len] = '\0';
		return true;
	}

	if (err && err_size > 0)
		snprintf(err, err_size, "unknown dispatch action '%s'", verb);
	return false;
}

tam_str_t tp_ipc_format_action(tam_str_t s, const struct tp_action *act) {
	if (!s || !act)
		return s;
	switch (act->kind) {
	case TP_ACT_SPAWN:
		s = tam_str_cat(s, "spawn ");
		return tp_ipc_escape_append(s, act->cmd);
	case TP_ACT_TERMINAL:
		return tam_str_cat(s, "terminal");
	case TP_ACT_CLOSE:
		return tam_str_cat(s, "close");
	case TP_ACT_QUIT:
		return tam_str_cat(s, "quit");
	case TP_ACT_RELOAD:
		return tam_str_cat(s, "reload");
	case TP_ACT_FOCUS:
		return tam_str_cat(s, act->dir < 0 ? "focus prev" : "focus next");
	case TP_ACT_SWAP:
		return tam_str_cat_printf(s, "swap %s", cardinal_dir_name(act->dir));
	case TP_ACT_MOVE:
		return tam_str_cat_printf(s, "move %s", cardinal_dir_name(act->dir));
	case TP_ACT_RESIZE:
		if (act->step > 0)
			return tam_str_cat_printf(s, "resize %s %d",
									  cardinal_dir_name(act->dir), act->step);
		return tam_str_cat_printf(s, "resize %s", cardinal_dir_name(act->dir));
	case TP_ACT_TOGGLE_FLOAT:
		return tam_str_cat(s, "toggle_float");
	case TP_ACT_TOGGLE_FULLSCREEN:
		return tam_str_cat(s, "toggle_fullscreen");
	case TP_ACT_TAG_VIEW:
		return tam_str_cat_printf(s, "tag view %u", act->tag);
	case TP_ACT_TAG_TOGGLE_VIEW:
		return tam_str_cat_printf(s, "tag toggle_view %u", act->tag);
	case TP_ACT_TAG_MOVE:
		return tam_str_cat_printf(s, "tag move %u", act->tag);
	case TP_ACT_TAG_TOGGLE_CLIENT:
		return tam_str_cat_printf(s, "tag toggle_client %u", act->tag);
	case TP_ACT_TAG_BACK:
		return tam_str_cat(s, "tag back");
	case TP_ACT_MONITOR_FOCUS:
		return tam_str_cat(s, act->dir < 0 ? "monitor focus prev"
										   : "monitor focus next");
	case TP_ACT_MONITOR_MOVE:
		return tam_str_cat(s, act->dir < 0 ? "monitor move prev"
										   : "monitor move next");
	case TP_ACT_LAYOUT_SET:
		return tam_str_cat_printf(
			s, "layout set %s",
			tp_layout_kind_name((enum tp_layout_kind)act->dir));
	case TP_ACT_LAYOUT_CYCLE:
		return tam_str_cat(s, act->dir < 0 ? "layout prev" : "layout next");
	case TP_ACT_LAYOUT_NMASTER:
		return tam_str_cat(s, act->dir < 0 ? "layout nmaster dec"
										   : "layout nmaster inc");
	case TP_ACT_SCRATCHPAD_TOGGLE:
		return tam_str_cat(s, "scratchpad toggle");
	case TP_ACT_SCRATCHPAD_MOVE:
		return tam_str_cat(s, "scratchpad move");
	case TP_ACT_CHORD_TOGGLE:
		return tam_str_cat(s, "chord_toggle");
	case TP_ACT_CHORD_CANCEL:
		return tam_str_cat(s, "chord_cancel");
	case TP_ACT_DPMS:
		s = tam_str_cat(s, act->dir ? "dpms on " : "dpms off ");
		return tp_ipc_escape_append(s, act->cmd);
	case TP_ACT_FOCUS_APP:
		s = tam_str_cat(s, "focus_app ");
		return tp_ipc_escape_append(s, act->cmd);
	case TP_ACT_KEYBOARD_NEXT:
		if (act->cmd[0]) {
			s = tam_str_cat(s, "keyboard next ");
			return tp_ipc_escape_append(s, act->cmd);
		}
		return tam_str_cat(s, "keyboard next");
	case TP_ACT_FOCUS_DIR:
		return tam_str_cat_printf(s, "focus %s", cardinal_dir_name(act->dir));
	case TP_ACT_SEND_KEY:
		return tam_str_cat(s, "send_key");
	case TP_ACT_CAPTURE:
		s = tam_str_cat(s, "capture ");
		return tp_ipc_escape_append(s, act->cmd);
	case TP_ACT_MONITOR_SCALE:
		return tam_str_cat_printf(s, "monitor scale %s %.2f",
								  act->cmd[0] ? act->cmd : "focused",
								  act->scale);
	case TP_ACT_MOUSE_MOVE:
		return tam_str_cat(s, "mouse_move");
	case TP_ACT_MOUSE_RESIZE:
		return tam_str_cat(s, "mouse_resize");
	}
	return tam_str_cat(s, "unknown");
}

/* ---- `tam-pane msg` CLI client ---- */

static void msg_usage(FILE *out) {
	fprintf(
		out,
		"usage: tam-pane msg [-S socket] <command> [args...]\n"
		"  -S socket   path to IPC socket (default: $TAM_PANE_SOCKET, else\n"
		"              $XDG_RUNTIME_DIR/tam-pane/$WAYLAND_DISPLAY.sock)\n"
		"  -h          show this help\n"
		"\n"
		"commands:\n"
		"  version                    compositor version and Wayland socket\n"
		"  monitors                   list connected outputs\n"
		"  windows                    list mapped toplevel windows\n"
		"  focused                    show the currently focused window\n"
		"  tags                       show tag views and occupancy per monitor\n"
		"  cursor                     show pointer coordinates and monitor\n"
		"  binds                      list active key bindings\n"
		"  keyboards                  list seat keyboards and active layouts\n"
		"  reload                     reload init.lua and report errors\n"
		"  dispatch <action> [args]   run a compositor dispatcher\n"
		"  subscribe [events...]      stream events until EOF or signal\n");
}

static bool write_all(int fd, const char *buf, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (n == 0)
			return false;
		off += (size_t)n;
	}
	return true;
}

int tp_ipc_client_main(int argc, char *argv[]) {
	const char *sock_override = NULL;
	int idx = 1;
	while (idx < argc && argv[idx][0] == '-' && argv[idx][1] != '\0') {
		if (strcmp(argv[idx], "--") == 0) {
			idx++;
			break;
		}
		if (strcmp(argv[idx], "-h") == 0 || strcmp(argv[idx], "--help") == 0) {
			msg_usage(stdout);
			return 0;
		}
		if (strcmp(argv[idx], "-S") == 0) {
			if (idx + 1 >= argc) {
				fprintf(stderr, "tam-pane msg: -S requires a socket path\n");
				return 2;
			}
			sock_override = argv[idx + 1];
			idx += 2;
			continue;
		}
		fprintf(stderr, "tam-pane msg: unknown option '%s'\n", argv[idx]);
		msg_usage(stderr);
		return 2;
	}
	if (idx >= argc) {
		msg_usage(stderr);
		return 2;
	}

	char sock_path[PATH_MAX];
	if (sock_override) {
		struct sockaddr_un probe;
		if (strlen(sock_override) >= sizeof(probe.sun_path)) {
			fprintf(stderr, "tam-pane msg: socket path too long\n");
			return 1;
		}
		snprintf(sock_path, sizeof(sock_path), "%s", sock_override);
	} else {
		const char *env_sock = getenv("TAM_PANE_SOCKET");
		if (env_sock && *env_sock) {
			struct sockaddr_un probe;
			if (strlen(env_sock) >= sizeof(probe.sun_path)) {
				fprintf(stderr, "tam-pane msg: TAM_PANE_SOCKET too long\n");
				return 1;
			}
			snprintf(sock_path, sizeof(sock_path), "%s", env_sock);
		} else {
			const char *rdir = getenv("XDG_RUNTIME_DIR");
			const char *wdisp = getenv("WAYLAND_DISPLAY");
			if (!tp_ipc_socket_path(sock_path, sizeof(sock_path), rdir,
									wdisp)) {
				fprintf(stderr,
						"tam-pane msg: cannot determine IPC socket "
						"(set TAM_PANE_SOCKET or pass -S)\n");
				return 1;
			}
		}
	}

	bool is_subscribe = strcmp(argv[idx], "subscribe") == 0;
	tam_str_t req = tam_str_new("");
	if (!req) {
		fprintf(stderr, "tam-pane msg: out of memory\n");
		return 1;
	}
	for (int i = idx; i < argc; i++) {
		if (strchr(argv[i], '\n') || strchr(argv[i], '\r')) {
			fprintf(stderr, "tam-pane msg: arguments cannot contain newlines\n");
			tam_str_free(req);
			return 2;
		}
		if (i > idx)
			req = tam_str_cat_len(req, " ", 1);
		req = tam_str_cat(req, argv[i]);
		if (!req) {
			fprintf(stderr, "tam-pane msg: out of memory\n");
			return 1;
		}
	}
	req = tam_str_cat_len(req, "\n", 1);
	if (!req || tam_str_len(req) > TP_IPC_MAX_LINE) {
		fprintf(stderr, "tam-pane msg: request line too long\n");
		tam_str_free(req);
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "tam-pane msg: socket: %s\n", strerror(errno));
		tam_str_free(req);
		return 1;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "tam-pane msg: connect %s: %s\n", sock_path,
				strerror(errno));
		close(fd);
		tam_str_free(req);
		return 1;
	}

	bool wrote = write_all(fd, req, tam_str_len(req));
	tam_str_free(req);
	if (!wrote) {
		fprintf(stderr, "tam-pane msg: write failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	FILE *fp = fdopen(fd, "r");
	if (!fp) {
		fprintf(stderr, "tam-pane msg: fdopen: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	char *line = NULL;
	size_t cap = 0;
	ssize_t nread = getline(&line, &cap, fp);
	if (nread < 0) {
		fprintf(stderr, "tam-pane msg: empty reply from compositor\n");
		free(line);
		fclose(fp);
		return 1;
	}
	while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r'))
		line[--nread] = '\0';

	int rc = 1;
	if (strncmp(line, "ERR ", 4) == 0) {
		fprintf(stderr, "tam-pane msg: %s\n", line + 4);
		rc = 1;
	} else if (strcmp(line, "ERR") == 0) {
		fprintf(stderr, "tam-pane msg: error\n");
		rc = 1;
	} else if (strcmp(line, "OK") == 0) {
		if (is_subscribe) {
			rc = 0;
			while ((nread = getline(&line, &cap, fp)) > 0) {
				fwrite(line, 1, (size_t)nread, stdout);
				fflush(stdout);
			}
		} else {
			bool got_end = false;
			while ((nread = getline(&line, &cap, fp)) > 0) {
				while (nread > 0 &&
					   (line[nread - 1] == '\n' || line[nread - 1] == '\r'))
					line[--nread] = '\0';
				if (strcmp(line, "END") == 0) {
					got_end = true;
					break;
				}
				fwrite(line, 1, (size_t)nread, stdout);
				fputc('\n', stdout);
			}
			if (!got_end) {
				fprintf(stderr, "tam-pane msg: truncated reply\n");
				rc = 1;
			} else {
				rc = 0;
			}
		}
	} else {
		fprintf(stderr, "tam-pane msg: unexpected reply: %s\n", line);
		rc = 1;
	}

	free(line);
	fclose(fp);
	return rc;
}
