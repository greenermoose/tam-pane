/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Compositor dispatcher actions and key-binding bridge.
 *
 * Key bindings live in Lua (src/lua_bindings.c, seeded with defaults on
 * startup and customizable in init.lua). This module implements the C actions
 * that tp.dsp.* dispatchers invoke.
 */
#include "tam-pane/bindings.h"
#include "tam-pane/chord.h"
#include "tam-pane/chord_overlay.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/input.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/layout/layout.h"
#include "tam-pane/lua.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/spawn.h"
#include "tam-pane/workspace.h"

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
	}
}

bool tp_bindings_handle(struct tp_server *s, uint32_t mods, xkb_keysym_t sym) {
	if (!s || !s->lua)
		return false;
	return tp_lua_handle_key(s->lua, mods, sym, s->locked);
}
