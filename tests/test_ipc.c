/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure unit tests for IPC protocol helpers (src/ipc_proto.c, no wlroots).
 */
#include "check.h"
#include "tam-pane/ipc.h"
#include <string.h>
#include <tam.h>

static void test_socket_path(void) {
	char out[256];
	CHECK(tp_ipc_socket_path(out, sizeof(out), "/run/user/1000", "wayland-0"));
	CHECK(strcmp(out, "/run/user/1000/tam-pane/wayland-0.sock") == 0);

	/* Trailing slash stripped */
	CHECK(tp_ipc_socket_path(out, sizeof(out), "/run/user/1000/", "wayland-1"));
	CHECK(strcmp(out, "/run/user/1000/tam-pane/wayland-1.sock") == 0);

	/* Invalid inputs rejected */
	CHECK(!tp_ipc_socket_path(out, sizeof(out), NULL, "wayland-0"));
	CHECK(!tp_ipc_socket_path(out, sizeof(out), "", "wayland-0"));
	CHECK(!tp_ipc_socket_path(out, sizeof(out), "/run/user/1000", NULL));
	CHECK(!tp_ipc_socket_path(out, sizeof(out), "/run/user/1000", ""));
	CHECK(!tp_ipc_socket_path(out, sizeof(out), "/run/user/1000", "../bad"));
	CHECK(!tp_ipc_socket_path(out, sizeof(out), "/run/user/1000", "a/b"));
	CHECK(!tp_ipc_socket_path(out, sizeof(out), "/run/user/1000", ".."));

	/* Buffer too small */
	char tiny[16];
	CHECK(!tp_ipc_socket_path(tiny, sizeof(tiny), "/run/user/1000",
							  "wayland-0"));

	/* Longer than sockaddr_un.sun_path (108 bytes) */
	char long_dir[120];
	memset(long_dir, 'a', sizeof(long_dir) - 1);
	long_dir[0] = '/';
	long_dir[sizeof(long_dir) - 1] = '\0';
	CHECK(!tp_ipc_socket_path(out, sizeof(out), long_dir, "wayland-0"));
}

static void test_escape_append(void) {
	tam_str_t s = tam_str_new("");
	s = tp_ipc_escape_append(s, "plain text");
	CHECK(strcmp(s, "plain text") == 0);
	tam_str_clear(s);

	s = tp_ipc_escape_append(s, "a\\b\tc\nd\re");
	CHECK(strcmp(s, "a\\\\b\\tc\\nd\\re") == 0);
	tam_str_clear(s);

	/* Control chars < 0x20 scrubbed to spaces; UTF-8 passes through */
	s = tp_ipc_escape_append(s, "hi\x01\x1f" "café");
	CHECK(strcmp(s, "hi  café") == 0);
	tam_str_free(s);
}

static void test_format_event(void) {
	struct tp_event ev = {.kind = TP_EVENT_WINDOW_OPEN};
	snprintf(ev.window.monitor, sizeof(ev.window.monitor), "DP-1");
	snprintf(ev.window.app_id, sizeof(ev.window.app_id), "foot");
	snprintf(ev.window.title, sizeof(ev.window.title), "hello\tworld\nline2");
	ev.window.tags = 3;
	ev.window.focused = true;
	ev.window.floating = false;
	ev.window.fullscreen = false;
	ev.window.geom = (struct tp_rect){10, 20, 800, 600};

	tam_str_t s = tam_str_new("");
	s = tp_ipc_format_event(s, &ev);
	CHECK(strcmp(s,
				 "event\twindow.open\tmonitor=DP-1\ttags=3\tfocused=1\t"
				 "floating=0\tfullscreen=0\tx=10\ty=20\tw=800\th=600\t"
				 "app_id=foot\ttitle=hello\\tworld\\nline2\n") == 0);
	tam_str_clear(s);

	struct tp_event tev = {.kind = TP_EVENT_TAG_CHANGE};
	snprintf(tev.tag.monitor, sizeof(tev.tag.monitor), "HEADLESS-1");
	tev.tag.tags = 2;
	tev.tag.prev_tags = 1;
	s = tp_ipc_format_event(s, &tev);
	CHECK(strcmp(s, "event\ttag.change\tmonitor=HEADLESS-1\ttags=2\t"
					"prev_tags=1\n") == 0);
	tam_str_clear(s);

	struct tp_event mev = {.kind = TP_EVENT_MONITOR_ADD};
	snprintf(mev.monitor.name, sizeof(mev.monitor.name), "HDMI-A-1");
	mev.monitor.area = (struct tp_rect){0, 0, 1920, 1080};
	mev.monitor.scale = 1.5f;
	s = tp_ipc_format_event(s, &mev);
	CHECK(strcmp(s, "event\tmonitor.add\tname=HDMI-A-1\tx=0\ty=0\tw=1920\t"
					"h=1080\tscale=1.50\n") == 0);
	tam_str_free(s);
}

static void test_parse_subscribe(void) {
	uint32_t mask = 0;
	char err[128] = {0};

	CHECK(tp_ipc_parse_subscribe("", &mask, err, sizeof(err)));
	CHECK((mask & (1u << TP_EVENT_WINDOW_OPEN)) != 0);
	CHECK((mask & (1u << TP_EVENT_WINDOW_CLOSE)) != 0);
	CHECK((mask & (1u << TP_EVENT_WINDOW_FOCUS)) != 0);
	CHECK((mask & (1u << TP_EVENT_TAG_CHANGE)) != 0);
	CHECK((mask & (1u << TP_EVENT_MONITOR_ADD)) != 0);
	CHECK((mask & (1u << TP_EVENT_MONITOR_REMOVE)) != 0);
	CHECK((mask & (1u << TP_EVENT_STARTUP)) == 0);

	CHECK(tp_ipc_parse_subscribe("window.focus tag.change", &mask, err,
								 sizeof(err)));
	CHECK(mask ==
		  ((1u << TP_EVENT_WINDOW_FOCUS) | (1u << TP_EVENT_TAG_CHANGE)));

	CHECK(tp_ipc_parse_subscribe("all", &mask, err, sizeof(err)));
	CHECK((mask & (1u << TP_EVENT_MONITOR_REMOVE)) != 0);

	CHECK(!tp_ipc_parse_subscribe("startup", &mask, err, sizeof(err)));
	CHECK(!tp_ipc_parse_subscribe("bogus.event", &mask, err, sizeof(err)));
	CHECK(err[0] != '\0');
}

static void test_parse_dispatch(void) {
	struct tp_action act;
	char mon[64];
	char err[128];

	CHECK(tp_ipc_parse_dispatch("terminal", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_TERMINAL);

	CHECK(tp_ipc_parse_dispatch("close", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_CLOSE);

	CHECK(tp_ipc_parse_dispatch("quit", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_QUIT);

	CHECK(tp_ipc_parse_dispatch("reload", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_RELOAD);

	CHECK(tp_ipc_parse_dispatch("toggle_float", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_TOGGLE_FLOAT);

	CHECK(tp_ipc_parse_dispatch("toggle_fullscreen", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_TOGGLE_FULLSCREEN);

	CHECK(tp_ipc_parse_dispatch("focus next", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_FOCUS && act.dir == 1);

	CHECK(tp_ipc_parse_dispatch("focus prev", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_FOCUS && act.dir == -1);

	CHECK(tp_ipc_parse_dispatch("swap left", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_SWAP && act.dir == TP_DIR_LEFT);

	CHECK(tp_ipc_parse_dispatch("move down", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_MOVE && act.dir == TP_DIR_DOWN);

	CHECK(tp_ipc_parse_dispatch("resize right 40", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_RESIZE && act.dir == TP_DIR_RIGHT &&
		  act.step == 40);

	CHECK(tp_ipc_parse_dispatch("tag view 3", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_TAG_VIEW && act.tag == 3);

	CHECK(tp_ipc_parse_dispatch("tag toggle_view 9", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_TAG_TOGGLE_VIEW && act.tag == 9);

	CHECK(tp_ipc_parse_dispatch("tag move 2", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_TAG_MOVE && act.tag == 2);

	CHECK(tp_ipc_parse_dispatch("tag toggle_client 5", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_TAG_TOGGLE_CLIENT && act.tag == 5);

	CHECK(tp_ipc_parse_dispatch("tag back", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_TAG_BACK);

	CHECK(tp_ipc_parse_dispatch("monitor focus next", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MONITOR_FOCUS && act.dir == 1 && mon[0] == '\0');

	CHECK(tp_ipc_parse_dispatch("monitor focus DP-2", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MONITOR_FOCUS && act.dir == 0 &&
		  strcmp(mon, "DP-2") == 0);

	CHECK(tp_ipc_parse_dispatch("monitor move HEADLESS-1", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MONITOR_MOVE && act.dir == 0 &&
		  strcmp(mon, "HEADLESS-1") == 0);

	CHECK(tp_ipc_parse_dispatch("spawn foot --title hello world", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_SPAWN &&
		  strcmp(act.cmd, "foot --title hello world") == 0);

	/* Action formatter round-check */
	tam_str_t fmt = tam_str_new("");
	fmt = tp_ipc_format_action(fmt, &act);
	CHECK(strcmp(fmt, "spawn foot --title hello world") == 0);
	tam_str_free(fmt);

	/* Invalid dispatches */
	CHECK(!tp_ipc_parse_dispatch("", &act, mon, sizeof(mon), err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("unknown", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("close extra", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("focus sideways", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("tag view 0", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("tag view 10", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("resize left 0", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("resize left -5", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("spawn   ", &act, mon, sizeof(mon), err,
								 sizeof(err)));
}

int main(void) {
	test_socket_path();
	test_escape_append();
	test_format_event();
	test_parse_subscribe();
	test_parse_dispatch();
	CHECK_DONE("test_ipc");
}
