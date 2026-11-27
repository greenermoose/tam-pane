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
	tam_str_clear(s);

	struct tp_event mfev = {.kind = TP_EVENT_MONITOR_FOCUS};
	snprintf(mfev.monitor.name, sizeof(mfev.monitor.name), "DP-2");
	s = tp_ipc_format_event(s, &mfev);
	CHECK(strcmp(s, "event\tmonitor.focus\tmonitor=DP-2\n") == 0);
	tam_str_clear(s);

	struct tp_event pev = {.kind = TP_EVENT_OUTPUT_POWER};
	snprintf(pev.output_power.monitor, sizeof(pev.output_power.monitor),
			 "HDMI-A-1");
	pev.output_power.dpms = false;
	s = tp_ipc_format_event(s, &pev);
	CHECK(strcmp(s, "event\toutput.power\tmonitor=HDMI-A-1\tdpms=0\n") == 0);
	tam_str_clear(s);

	struct tp_event kev = {.kind = TP_EVENT_KEYBOARD_LAYOUT};
	snprintf(kev.keyboard.name, sizeof(kev.keyboard.name), "kbd-0");
	snprintf(kev.keyboard.active_keymap, sizeof(kev.keyboard.active_keymap),
			 "English (US)");
	kev.keyboard.active_layout_index = 1;
	s = tp_ipc_format_event(s, &kev);
	CHECK(strcmp(s, "event\tkeyboard.layout\tname=kbd-0\t"
					"active_keymap=English (US)\tactive_layout_index=1\n") ==
		  0);
	tam_str_clear(s);

	struct tp_event rev = {.kind = TP_EVENT_CONFIG_RELOAD};
	s = tp_ipc_format_event(s, &rev);
	CHECK(strcmp(s, "event\tconfig.reload\n") == 0);
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
	CHECK((mask & (1u << TP_EVENT_MONITOR_FOCUS)) != 0);
	CHECK((mask & (1u << TP_EVENT_OUTPUT_POWER)) != 0);
	CHECK((mask & (1u << TP_EVENT_KEYBOARD_LAYOUT)) != 0);
	CHECK((mask & (1u << TP_EVENT_CONFIG_RELOAD)) != 0);

	CHECK(tp_ipc_parse_subscribe(
		"monitor.focus output.power keyboard.layout config.reload", &mask, err,
		sizeof(err)));
	CHECK(mask == ((1u << TP_EVENT_MONITOR_FOCUS) |
				   (1u << TP_EVENT_OUTPUT_POWER) |
				   (1u << TP_EVENT_KEYBOARD_LAYOUT) |
				   (1u << TP_EVENT_CONFIG_RELOAD)));

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
	CHECK(act.kind == TP_ACT_TAG_VIEW && act.tag == 3 && mon[0] == '\0');

	CHECK(tp_ipc_parse_dispatch("tag view 4 DP-2", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_TAG_VIEW && act.tag == 4 &&
		  strcmp(mon, "DP-2") == 0);

	CHECK(tp_ipc_parse_dispatch("tag toggle_view 9", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_TAG_TOGGLE_VIEW && act.tag == 9 && mon[0] == '\0');

	CHECK(tp_ipc_parse_dispatch("tag toggle_view 2 HDMI-A-1", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_TAG_TOGGLE_VIEW && act.tag == 2 &&
		  strcmp(mon, "HDMI-A-1") == 0);

	CHECK(tp_ipc_parse_dispatch("dpms off HDMI-A-1", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_DPMS && act.dir == 0 &&
		  strcmp(mon, "HDMI-A-1") == 0 && strcmp(act.cmd, "HDMI-A-1") == 0);

	CHECK(tp_ipc_parse_dispatch("dpms on DP-1", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_DPMS && act.dir == 1 && strcmp(mon, "DP-1") == 0);

	CHECK(tp_ipc_parse_dispatch("focus_app Google Chrome", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_FOCUS_APP &&
		  strcmp(act.cmd, "Google Chrome") == 0);

	CHECK(tp_ipc_parse_dispatch("keyboard next", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_KEYBOARD_NEXT && act.cmd[0] == '\0');

	CHECK(tp_ipc_parse_dispatch("keyboard next at-translated-set-2-keyboard",
								&act, mon, sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_KEYBOARD_NEXT &&
		  strcmp(act.cmd, "at-translated-set-2-keyboard") == 0);

	CHECK(!tp_ipc_parse_dispatch("dpms bogus DP-1", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("dpms on", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("focus_app   ", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("keyboard prev", &act, mon, sizeof(mon), err,
								 sizeof(err)));

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

	/* Layout and scratchpad dispatches (0.2.5) */
	CHECK(tp_ipc_parse_dispatch("layout set master", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_LAYOUT_SET && act.dir == 1);
	fmt = tp_ipc_format_action(tam_str_new(""), &act);
	CHECK(strcmp(fmt, "layout set master") == 0);
	tam_str_free(fmt);

	CHECK(tp_ipc_parse_dispatch("layout set scroller", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_LAYOUT_SET && act.dir == 2);
	CHECK(tp_ipc_parse_dispatch("layout set monocle", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_LAYOUT_SET && act.dir == 3);
	CHECK(tp_ipc_parse_dispatch("layout next", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_LAYOUT_CYCLE && act.dir == 1);
	CHECK(tp_ipc_parse_dispatch("layout prev", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_LAYOUT_CYCLE && act.dir == -1);
	CHECK(tp_ipc_parse_dispatch("layout nmaster inc", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_LAYOUT_NMASTER && act.dir == 1);
	CHECK(tp_ipc_parse_dispatch("layout nmaster dec", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_LAYOUT_NMASTER && act.dir == -1);
	CHECK(tp_ipc_parse_dispatch("scratchpad toggle", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_SCRATCHPAD_TOGGLE);
	fmt = tp_ipc_format_action(tam_str_new(""), &act);
	CHECK(strcmp(fmt, "scratchpad toggle") == 0);
	tam_str_free(fmt);
	CHECK(tp_ipc_parse_dispatch("scratchpad move", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_SCRATCHPAD_MOVE);
	fmt = tp_ipc_format_action(tam_str_new(""), &act);
	CHECK(strcmp(fmt, "scratchpad move") == 0);
	tam_str_free(fmt);

	/* Invalid layout / scratchpad dispatches */
	CHECK(!tp_ipc_parse_dispatch("layout", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("layout set unknown", &act, mon, sizeof(mon),
								 err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("layout next extra", &act, mon, sizeof(mon),
								 err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("layout nmaster 0", &act, mon, sizeof(mon),
								 err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("scratchpad", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("scratchpad drop", &act, mon, sizeof(mon), err,
								 sizeof(err)));

	/* 0.2.8 dispatches: focus dir, send_key, capture, monitor scale */
	CHECK(tp_ipc_parse_dispatch("focus left", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_FOCUS_DIR && act.dir == TP_DIR_LEFT);
	CHECK(tp_ipc_parse_dispatch("send_key Ctrl+Shift+v", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_SEND_KEY &&
		  act.mods == (TP_MOD_CTRL | TP_MOD_SHIFT) && act.sym == XKB_KEY_v);
	CHECK(tp_ipc_parse_dispatch("capture grim /tmp/shot.png", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_CAPTURE &&
		  strcmp(act.cmd, "grim /tmp/shot.png") == 0);
	CHECK(tp_ipc_parse_dispatch("monitor scale DP-1 1.25", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MONITOR_SCALE && strcmp(act.cmd, "DP-1") == 0 &&
		  act.scale > 1.24f && act.scale < 1.26f);
	CHECK(!tp_ipc_parse_dispatch("monitor scale DP-1 99", &act, mon,
								 sizeof(mon), err, sizeof(err)));

	/* 0.2.14: monitor mode, for retraining a display link. */
	CHECK(tp_ipc_parse_dispatch("monitor mode DP-2 1920x1080@59.94", &act,
								mon, sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MONITOR_MODE && strcmp(act.cmd, "DP-2") == 0 &&
		  strcmp(mon, "DP-2") == 0 && act.mode_w == 1920 &&
		  act.mode_h == 1080 && act.mode_refresh == 59940);
	CHECK(tp_ipc_parse_dispatch("monitor mode focused 2560x1440", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MONITOR_MODE && act.mode_w == 2560 &&
		  act.mode_h == 1440 && act.mode_refresh == 0);
	{
		tam_str_t f = tp_ipc_format_action(tam_str_new(""), &act);
		CHECK(f && strcmp(f, "monitor mode focused 2560x1440") == 0);
		tam_str_free(f);
	}
	CHECK(!tp_ipc_parse_dispatch("monitor mode DP-2 1920x1080@", &act, mon,
								 sizeof(mon), err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("monitor mode DP-2 1920x", &act, mon,
								 sizeof(mon), err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("monitor mode DP-2 0x1080@60", &act, mon,
								 sizeof(mon), err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("monitor mode DP-2 1920x1080@60Hz", &act,
								 mon, sizeof(mon), err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("monitor mode DP-2", &act, mon, sizeof(mon),
								 err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("monitor mode DP-2 1920x1080 extra", &act,
								 mon, sizeof(mon), err, sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("send_key   ", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("capture   ", &act, mon, sizeof(mon), err,
								 sizeof(err)));

	/* Gamma and freeze dispatches (0.2.16) */
	CHECK(tp_ipc_parse_dispatch("gamma 4000", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_GAMMA && act.gamma_temp == 4000);
	{
		tam_str_t f = tp_ipc_format_action(tam_str_new(""), &act);
		CHECK(f && strcmp(f, "gamma 4000") == 0);
		tam_str_free(f);
	}
	CHECK(tp_ipc_parse_dispatch("gamma reset", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_GAMMA && act.gamma_temp == 6500);
	{
		tam_str_t f = tp_ipc_format_action(tam_str_new(""), &act);
		CHECK(f && strcmp(f, "gamma reset") == 0);
		tam_str_free(f);
	}
	CHECK(tp_ipc_parse_dispatch("gamma off", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_GAMMA && act.gamma_temp == 6500);
	CHECK(!tp_ipc_parse_dispatch("gamma 500", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("gamma 30000", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("gamma -4000", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(!tp_ipc_parse_dispatch("gamma", &act, mon, sizeof(mon), err,
								 sizeof(err)));
	CHECK(tp_ipc_parse_dispatch("freeze on", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_FREEZE && act.dir == 1);
	{
		tam_str_t f = tp_ipc_format_action(tam_str_new(""), &act);
		CHECK(f && strcmp(f, "freeze on") == 0);
		tam_str_free(f);
	}
	CHECK(tp_ipc_parse_dispatch("freeze off", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_FREEZE && act.dir == 0);
	{
		tam_str_t f = tp_ipc_format_action(tam_str_new(""), &act);
		CHECK(f && strcmp(f, "freeze off") == 0);
		tam_str_free(f);
	}
	CHECK(!tp_ipc_parse_dispatch("freeze maybe", &act, mon, sizeof(mon), err,
								 sizeof(err)));

	/* session.lock / session.unlock / gamma.change subscribe & format */
	{
		uint32_t mask = 0;
		CHECK(tp_ipc_parse_subscribe("session.lock session.unlock gamma.change",
									 &mask, err, sizeof(err)));
		CHECK((mask & (1u << TP_EVENT_SESSION_LOCK)) != 0);
		CHECK((mask & (1u << TP_EVENT_SESSION_UNLOCK)) != 0);
		CHECK((mask & (1u << TP_EVENT_GAMMA_CHANGE)) != 0);

		struct tp_event ev = {.kind = TP_EVENT_SESSION_LOCK};
		ev.session_lock.locked = true;
		tam_str_t s = tp_ipc_format_event(tam_str_new(""), &ev);
		CHECK(s && strcmp(s, "event\tsession.lock\tlocked=1\n") == 0);
		tam_str_free(s);

		ev = (struct tp_event){.kind = TP_EVENT_SESSION_UNLOCK};
		ev.session_lock.locked = false;
		s = tp_ipc_format_event(tam_str_new(""), &ev);
		CHECK(s && strcmp(s, "event\tsession.unlock\tlocked=0\n") == 0);
		tam_str_free(s);

		ev = (struct tp_event){.kind = TP_EVENT_GAMMA_CHANGE};
		ev.gamma.temperature = 4000;
		ev.gamma.enabled = true;
		s = tp_ipc_format_event(tam_str_new(""), &ev);
		CHECK(s && strcmp(s, "event\tgamma.change\ttemperature=4000\tenabled=1\n") == 0);
		tam_str_free(s);
	}

	/* 0.2.17: monitor enable/disable and targeted window dispatches */
	CHECK(tp_ipc_parse_dispatch("monitor disable eDP-1", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MONITOR_ENABLE && act.dir == 0 &&
		  strcmp(act.cmd, "eDP-1") == 0);
	{
		tam_str_t s = tp_ipc_format_action(tam_str_new(""), &act);
		CHECK(s && strcmp(s, "monitor disable eDP-1") == 0);
		tam_str_free(s);
	}
	CHECK(tp_ipc_parse_dispatch("monitor enable focused", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MONITOR_ENABLE && act.dir == 1 &&
		  strcmp(act.cmd, "focused") == 0);
	CHECK(!tp_ipc_parse_dispatch("monitor enable", &act, mon, sizeof(mon), err,
								 sizeof(err)));

	CHECK(tp_ipc_parse_dispatch("close 0x2a", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_CLOSE && act.target_id == 0x2a);
	CHECK(!tp_ipc_parse_dispatch("close 0x0", &act, mon, sizeof(mon), err,
								 sizeof(err)));

	CHECK(tp_ipc_parse_dispatch("pin", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_PIN && act.dir == -1 && act.target_id == 0);
	CHECK(tp_ipc_parse_dispatch("pin on 0x7", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_PIN && act.dir == 1 && act.target_id == 7);
	CHECK(tp_ipc_parse_dispatch("pin 0x9", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_PIN && act.dir == -1 && act.target_id == 9);
	CHECK(!tp_ipc_parse_dispatch("pin maybe", &act, mon, sizeof(mon), err,
								 sizeof(err)));

	CHECK(tp_ipc_parse_dispatch("float off 0x10", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_FLOAT_SET && act.dir == 0 && act.target_id == 16);

	CHECK(tp_ipc_parse_dispatch("resize_exact 800 600 0x3", &act, mon,
								sizeof(mon), err, sizeof(err)));
	CHECK(act.kind == TP_ACT_RESIZE_EXACT && act.mode_w == 800 &&
		  act.mode_h == 600 && act.target_id == 3);
	CHECK(!tp_ipc_parse_dispatch("resize_exact 0 600", &act, mon, sizeof(mon),
								 err, sizeof(err)));

	CHECK(tp_ipc_parse_dispatch("move_exact -10 25 0x4", &act, mon, sizeof(mon),
								err, sizeof(err)));
	CHECK(act.kind == TP_ACT_MOVE_EXACT && act.pos_x == -10 &&
		  act.pos_y == 25 && act.target_id == 4);
	CHECK(!tp_ipc_parse_dispatch("move_exact 10", &act, mon, sizeof(mon), err,
								 sizeof(err)));

	CHECK(tp_ipc_parse_dispatch("center 0x5", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_CENTER && act.target_id == 5);
	CHECK(tp_ipc_parse_dispatch("raise 0x6", &act, mon, sizeof(mon), err,
								sizeof(err)));
	CHECK(act.kind == TP_ACT_RAISE && act.target_id == 6);

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
