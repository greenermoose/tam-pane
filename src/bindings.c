/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Compositor dispatcher actions and key-binding bridge.
 *
 * Key bindings live in Lua (src/lua_bindings.c, seeded with defaults on
 * startup and customizable in init.lua). This module implements the C actions
 * that tp.dsp.* dispatchers invoke.
 */
#include "tam-pane/bindings.h"
#include "tam-pane/capture.h"
#include "tam-pane/chord.h"
#include "tam-pane/chord_overlay.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/input.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/layout/layout.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/output_power.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/spawn.h"
#include "tam-pane/workspace.h"
#include <ctype.h>
#include <string.h>

static bool contains_icase(const char *haystack, const char *needle) {
	if (!haystack || !needle || !*needle)
		return false;
	size_t nlen = strlen(needle);
	for (const char *p = haystack; *p; p++) {
		size_t i = 0;
		while (i < nlen && p[i] &&
			   tolower((unsigned char)p[i]) ==
				   tolower((unsigned char)needle[i]))
			i++;
		if (i == nlen)
			return true;
	}
	return false;
}

static void focus_app_by_name(struct tp_server *s, const char *name) {
	if (!s || !name || !*name)
		return;
	struct tp_client *match = NULL;
	struct tp_client *agent_match = NULL;
	struct tp_client *c;
	wl_list_for_each(c, &s->focus_stack, flink) {
		if (!c->mapped)
			continue;
		const char *app_id = tp_client_app_id(c);
		const char *title = tp_client_title(c);
		if (app_id && contains_icase(app_id, name)) {
			match = c;
			break;
		}
		if (!agent_match && app_id &&
			strcmp(app_id, "org.tamlinux.agent") == 0 && title &&
			contains_icase(title, name)) {
			agent_match = c;
		}
	}
	if (!match)
		match = agent_match;
	if (!match)
		return;
	if (match->mon) {
		if (s->selmon != match->mon)
			tp_monitor_focus(s, match->mon);
		if (!tp_client_visible(match) && !match->scratchpad && match->tags) {
			tp_tags_t prev = tp_tagview_current(&match->mon->view);
			unsigned int tag = tp_tags_first(match->tags);
			if (tag && tp_tagview_show(&match->mon->view, tp_tag_mask(tag))) {
				tp_pointer_cancel_grab(s, NULL);
				tp_arrange(match->mon);
				tp_events_emit_tag(s, match->mon, prev,
								   tp_tagview_current(&match->mon->view));
				tp_workspace_sync_monitor(match->mon);
			}
		}
	}
	tp_focus(s, match);
}

static void view_changed(struct tp_server *s, tp_tags_t prev_tags) {
	tp_pointer_cancel_grab(s, NULL);
	tp_arrange(s->selmon);
	if (s->selmon) {
		tp_events_emit_tag(s, s->selmon, prev_tags,
						   tp_tagview_current(&s->selmon->view));
		tp_workspace_sync_monitor(s->selmon);
	}
	tp_focus_refresh(s);
}

void tp_action_run(struct tp_server *s, const struct tp_action *act) {
	if (!s || !act)
		return;

	switch (act->kind) {
	case TP_ACT_SPAWN:
		tp_spawn(act->cmd);
		break;
	case TP_ACT_TERMINAL:
		tp_spawn(s->settings.terminal);
		break;
	case TP_ACT_CLOSE: {
		struct tp_client *c = tp_focused(s);
		if (c)
			tp_client_close(c);
		break;
	}
	case TP_ACT_QUIT:
		tp_server_quit(s);
		break;
	case TP_ACT_FOCUS:
		tp_focus_cycle(s, act->dir);
		break;
	case TP_ACT_SWAP: {
		struct tp_client *c = tp_focused(s);
		if (!c || !c->mon || c->floating || c->fullscreen)
			break;
		if (c->mon->arrange_dirty)
			tp_arrange_flush(s);
		if (tp_layout_swap_dir(tp_monitor_layout(c->mon), c,
							   (enum tp_dir)act->dir))
			tp_arrange(c->mon);
		break;
	}
	case TP_ACT_MOVE: {
		struct tp_client *c = tp_focused(s);
		if (!c || !c->mon || c->floating || c->fullscreen)
			break;
		if (c->mon->arrange_dirty)
			tp_arrange_flush(s);
		if (tp_layout_move_dir(tp_monitor_layout(c->mon), c,
							   (enum tp_dir)act->dir, s->settings.split_ratio))
			tp_arrange(c->mon);
		break;
	}
	case TP_ACT_RESIZE: {
		struct tp_client *c = tp_focused(s);
		if (!c || !c->mon || c->fullscreen)
			break;
		enum tp_dir dir = (enum tp_dir)act->dir;
		int32_t step = act->step > 0 ? act->step : s->settings.resize_step;
		if (c->floating) {
			tp_client_resize_floating(c, dir, step);
			break;
		}
		if (c->mon->arrange_dirty)
			tp_arrange_flush(s);
		if (tp_layout_resize(tp_monitor_layout(c->mon), c, dir, step))
			tp_arrange(c->mon);
		break;
	}
	case TP_ACT_TOGGLE_FLOAT: {
		struct tp_client *c = tp_focused(s);
		if (c)
			tp_client_set_floating(c, !c->floating);
		break;
	}
	case TP_ACT_TOGGLE_FULLSCREEN: {
		struct tp_client *c = tp_focused(s);
		if (c)
			tp_client_set_fullscreen(c, !c->fullscreen);
		break;
	}
	case TP_ACT_TAG_VIEW:
		if (s->selmon) {
			tp_tags_t prev = tp_tagview_current(&s->selmon->view);
			if (tp_tagview_show(&s->selmon->view, tp_tag_mask(act->tag)))
				view_changed(s, prev);
		}
		break;
	case TP_ACT_TAG_TOGGLE_VIEW:
		if (s->selmon) {
			tp_tags_t prev = tp_tagview_current(&s->selmon->view);
			if (tp_tagview_toggle(&s->selmon->view, tp_tag_mask(act->tag)))
				view_changed(s, prev);
		}
		break;
	case TP_ACT_TAG_MOVE: {
		struct tp_client *c = tp_focused(s);
		if (c)
			tp_client_set_tags(c, tp_tag_mask(act->tag));
		break;
	}
	case TP_ACT_TAG_TOGGLE_CLIENT: {
		struct tp_client *c = tp_focused(s);
		if (c)
			tp_client_toggle_tags(c, tp_tag_mask(act->tag));
		break;
	}
	case TP_ACT_TAG_BACK:
		if (s->selmon) {
			tp_tags_t prev = tp_tagview_current(&s->selmon->view);
			tp_tagview_back(&s->selmon->view);
			if (tp_tagview_current(&s->selmon->view) != prev)
				view_changed(s, prev);
		}
		break;
	case TP_ACT_MONITOR_FOCUS: {
		struct tp_monitor *m = tp_monitor_cycle(s, act->dir);
		if (m)
			tp_monitor_focus(s, m);
		break;
	}
	case TP_ACT_MONITOR_MOVE: {
		struct tp_client *c = tp_focused(s);
		struct tp_monitor *m = tp_monitor_cycle(s, act->dir);
		if (c && m)
			tp_client_set_monitor(c, m, false);
		break;
	}
	case TP_ACT_RELOAD:
		tp_server_request_reload(s);
		break;
	case TP_ACT_LAYOUT_SET:
		if (s->selmon) {
			tp_pointer_cancel_grab(s, NULL);
			if (tp_layout_set_kind(tp_monitor_layout(s->selmon),
								   (enum tp_layout_kind)act->dir))
				tp_arrange(s->selmon);
		}
		break;
	case TP_ACT_LAYOUT_CYCLE:
		if (s->selmon) {
			tp_pointer_cancel_grab(s, NULL);
			tp_layout_cycle(tp_monitor_layout(s->selmon), act->dir);
			tp_arrange(s->selmon);
		}
		break;
	case TP_ACT_LAYOUT_NMASTER:
		if (s->selmon) {
			if (tp_layout_adjust_nmaster(tp_monitor_layout(s->selmon),
										 act->dir))
				tp_arrange(s->selmon);
		}
		break;
	case TP_ACT_SCRATCHPAD_TOGGLE:
		tp_scratchpad_toggle(s);
		break;
	case TP_ACT_SCRATCHPAD_MOVE: {
		struct tp_client *c = tp_focused(s);
		if (c)
			tp_client_scratchpad_move(c);
		break;
	}
	case TP_ACT_CHORD_TOGGLE:
		if (tp_chord_is_composing(&s->chord)) {
			if (tp_chord_cancel(&s->chord)) {
				tp_chord_overlay_update(s);
				tp_events_emit_chord(s, TP_EVENT_CHORD_CANCEL, NULL, NULL);
			}
		} else if (!s->locked && tp_chord_open(&s->chord, s->lua, false)) {
			tp_chord_overlay_update(s);
			tp_events_emit_chord(s, TP_EVENT_CHORD_START, NULL, NULL);
		}
		break;
	case TP_ACT_CHORD_CANCEL:
		if (tp_chord_cancel(&s->chord)) {
			tp_chord_overlay_update(s);
			tp_events_emit_chord(s, TP_EVENT_CHORD_CANCEL, NULL, NULL);
		}
		break;
	case TP_ACT_DPMS: {
		struct tp_monitor *m = tp_monitor_by_name(s, act->cmd);
		if (m && m->output)
			tp_output_power_set(s, m->output, act->dir != 0);
		break;
	}
	case TP_ACT_FOCUS_APP:
		focus_app_by_name(s, act->cmd);
		break;
	case TP_ACT_KEYBOARD_NEXT:
		tp_keyboard_next_layout(s, act->cmd[0] ? act->cmd : NULL);
		break;
	case TP_ACT_FOCUS_DIR:
		tp_focus_dir(s, (enum tp_dir)act->dir);
		break;
	case TP_ACT_SEND_KEY:
		tp_keyboard_send_key(s, act->mods, act->sym);
		break;
	case TP_ACT_CAPTURE:
		tp_capture_launch(s, act->cmd);
		break;
	case TP_ACT_MONITOR_SCALE:
		tp_monitor_set_scale(s, act->cmd[0] ? act->cmd : NULL, act->scale);
		break;
	case TP_ACT_MONITOR_MODE:
		tp_monitor_set_mode(s, act->cmd[0] ? act->cmd : NULL, act->mode_w,
							act->mode_h, act->mode_refresh, NULL, 0);
		break;
	case TP_ACT_MOUSE_MOVE:
	case TP_ACT_MOUSE_RESIZE:
		break;
	}
}

bool tp_bindings_handle(struct tp_server *s, uint32_t mods, xkb_keysym_t sym) {
	if (!s || !s->lua)
		return false;
	return tp_lua_handle_key(s->lua, mods, sym, s->locked);
}

bool tp_bindings_handle_key(struct tp_server *s, uint32_t mods,
							xkb_keysym_t sym, uint32_t keycode, bool released) {
	if (!s || !s->lua)
		return false;
	return tp_lua_handle_key_ex(s->lua, mods, sym, keycode, released,
								s->locked);
}

bool tp_bindings_handle_mouse(struct tp_server *s, uint32_t mods,
							  uint32_t button, struct tp_action *out_grab) {
	if (!s || !s->lua)
		return false;
	return tp_lua_handle_mouse(s->lua, mods, button, s->locked, out_grab);
}
