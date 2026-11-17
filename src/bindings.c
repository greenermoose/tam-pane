/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Compiled-in key bindings. Lua configuration replaces this table in 0.1.1;
 * the actions stay as the dispatchers Lua calls.
 */
#include "tam-pane/bindings.h"
#include "tam-pane/client.h"
#include "tam-pane/focus.h"
#include "tam-pane/layout/arrange.h"
#include "tam-pane/monitor.h"
#include "tam-pane/server.h"
#include "tam-pane/spawn.h"
#include <wlr/types/wlr_keyboard.h>

union tp_arg {
	int i;
	unsigned int u;
};

struct tp_binding {
	uint32_t mods;
	xkb_keysym_t sym;
	void (*action)(struct tp_server *s, union tp_arg arg);
	union tp_arg arg;
};

static void quit(struct tp_server *s, union tp_arg a) {
	tp_server_quit(s);
}

static void terminal(struct tp_server *s, union tp_arg a) {
	tp_spawn(s->settings.terminal);
}

static void close_window(struct tp_server *s, union tp_arg a) {
	struct tp_client *c = tp_focused(s);
	if (c)
		tp_client_close(c);
}

static void focus_cycle(struct tp_server *s, union tp_arg a) {
	tp_focus_cycle(s, a.i);
}

static void toggle_floating(struct tp_server *s, union tp_arg a) {
	struct tp_client *c = tp_focused(s);
	if (c)
		tp_client_set_floating(c, !c->floating);
}

static void toggle_fullscreen(struct tp_server *s, union tp_arg a) {
	struct tp_client *c = tp_focused(s);
	if (c)
		tp_client_set_fullscreen(c, !c->fullscreen);
}

static void view_changed(struct tp_server *s) {
	tp_arrange(s->selmon);
	tp_focus_refresh(s);
}

static void view_tag(struct tp_server *s, union tp_arg a) {
	if (s->selmon && tp_tagview_show(&s->selmon->view, tp_tag_mask(a.u)))
		view_changed(s);
}

static void toggle_view_tag(struct tp_server *s, union tp_arg a) {
	if (s->selmon && tp_tagview_toggle(&s->selmon->view, tp_tag_mask(a.u)))
		view_changed(s);
}

static void view_back(struct tp_server *s, union tp_arg a) {
	if (s->selmon) {
		tp_tagview_back(&s->selmon->view);
		view_changed(s);
	}
}

static void move_to_tag(struct tp_server *s, union tp_arg a) {
	struct tp_client *c = tp_focused(s);
	if (c)
		tp_client_set_tags(c, tp_tag_mask(a.u));
}

#define META TP_MOD_META
#define SHIFT WLR_MODIFIER_SHIFT
#define CTRL WLR_MODIFIER_CTRL
#define TAG(key, n)                                                            \
	{META, key, view_tag, {.u = n}},                                           \
		{META | SHIFT, key, move_to_tag, {.u = n}},                            \
		{META | CTRL, key, toggle_view_tag, {.u = n}}

static const struct tp_binding bindings[] = {
	{META, XKB_KEY_Return, terminal, {0}},
	{META, XKB_KEY_w, close_window, {0}},
	{META | SHIFT, XKB_KEY_e, quit, {0}},
	{META, XKB_KEY_j, focus_cycle, {.i = +1}},
	{META, XKB_KEY_k, focus_cycle, {.i = -1}},
	{META, XKB_KEY_v, toggle_floating, {0}},
	{META, XKB_KEY_f, toggle_fullscreen, {0}},
	{META, XKB_KEY_Tab, view_back, {0}},
	TAG(XKB_KEY_1, 1),
	TAG(XKB_KEY_2, 2),
	TAG(XKB_KEY_3, 3),
	TAG(XKB_KEY_4, 4),
	TAG(XKB_KEY_5, 5),
	TAG(XKB_KEY_6, 6),
	TAG(XKB_KEY_7, 7),
	TAG(XKB_KEY_8, 8),
	TAG(XKB_KEY_9, 9),
};

bool tp_bindings_handle(struct tp_server *s, uint32_t mods, xkb_keysym_t sym) {
	/* Caps Lock and Num Lock never block a binding. */
	mods &= ~(uint32_t)(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
	for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
		if (bindings[i].mods == mods && bindings[i].sym == sym) {
			bindings[i].action(s, bindings[i].arg);
			return true;
		}
	}
	return false;
}
