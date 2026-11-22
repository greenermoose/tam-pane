/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure IPC wire helpers and the `tam-pane msg` CLI client (no wlroots).
 */
#include "tam-pane/ipc.h"
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
		   (1u << TP_EVENT_MONITOR_ADD) | (1u << TP_EVENT_MONITOR_REMOVE);
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
		strcmp(verb, "toggle_fullscreen") == 0) {
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
		else
			out->kind = TP_ACT_TOGGLE_FULLSCREEN;
		return true;
	}

	if (strcmp(verb, "focus") == 0) {
		char arg[32];
		if (!next_token(&p, arg, sizeof(arg)) || has_extra_tokens(p)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "focus expects 'next' or 'prev'");
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
		if (err && err_size > 0)
			snprintf(err, err_size, "focus expects 'next' or 'prev'");
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
			!parse_positive_int(num_tok, 1, TP_TAG_COUNT, &tag_n) ||
			has_extra_tokens(p)) {
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
				snprintf(err, err_size, "monitor expects 'focus' or 'move'");
			return false;
		}
		if (strcmp(sub, "focus") == 0)
			out->kind = TP_ACT_MONITOR_FOCUS;
		else if (strcmp(sub, "move") == 0)
			out->kind = TP_ACT_MONITOR_MOVE;
		else {
			if (err && err_size > 0)
				snprintf(err, err_size, "monitor expects 'focus' or 'move'");
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
