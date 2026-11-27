/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure unit tests for tam-pane's one-handed chord composer state machine.
 */
#include "tam-pane/chord.h"
#include "tam-pane/lua.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct tp_chord_result tap(struct tp_chord_state *st, struct tp_lua *lua,
								 xkb_keysym_t sym, uint32_t code) {
	struct tp_chord_result r = tp_chord_on_key(st, lua, sym, sym, code, true, 0);
	struct tp_chord_result release =
		tp_chord_on_key(st, lua, sym, sym, code, false, 0);
	assert(!release.ended && !release.cancelled);
	return r;
}

static void test_caps_lock_three_states(struct tp_lua *lua) {
	struct tp_chord_state st;
	tp_chord_init(&st);

	/* 1. Press Caps Lock -> Pluripotent */
	struct tp_chord_result r =
		tp_chord_on_key(&st, lua, XKB_KEY_Caps_Lock, XKB_KEY_Caps_Lock, 58,
						true, 0);
	assert(r.kind == TP_CHORD_RES_CONSUME && r.started);
	assert(st.mode == TP_CHORD_PLURIPOTENT);
	assert(tp_chord_is_composing(&st));
	assert(st.filtered_len > 0);

	/* Release Caps Lock -> consumed */
	r = tp_chord_on_key(&st, lua, XKB_KEY_Caps_Lock, XKB_KEY_Caps_Lock, 58,
						false, 0);
	assert(r.kind == TP_CHORD_RES_CONSUME);
	assert(st.mode == TP_CHORD_PLURIPOTENT);

	/* 2. Press 'h' -> transitions to Regular Caps Lock and sends shifted 'h' */
	r = tp_chord_on_key(&st, lua, XKB_KEY_h, XKB_KEY_h, 35, true, 0);
	assert(r.kind == TP_CHORD_RES_CAPS_LETTER);
	assert(r.mods == TP_MOD_SHIFT && r.sym == XKB_KEY_h && r.keycode == 35);
	assert(r.cancelled && r.caps_changed);
	assert(st.mode == TP_CHORD_CAPS_LOCK);
	assert(!tp_chord_is_composing(&st));
	assert(tp_chord_is_caps_lock(&st));

	/* 3. Subsequent A-Z sends shifted, digits pass through unchanged */
	r = tp_chord_on_key(&st, lua, XKB_KEY_i, XKB_KEY_i, 23, true, 0);
	assert(r.kind == TP_CHORD_RES_CAPS_LETTER && r.mods == TP_MOD_SHIFT);
	r = tp_chord_on_key(&st, lua, XKB_KEY_1, XKB_KEY_1, 2, true, 0);
	assert(r.kind == TP_CHORD_RES_PASSTHROUGH);

	/* 4. Press Caps Lock again -> turns Regular Caps Lock off -> IDLE */
	r = tp_chord_on_key(&st, lua, XKB_KEY_Caps_Lock, XKB_KEY_Caps_Lock, 58,
						true, 0);
	assert(r.kind == TP_CHORD_RES_CONSUME && r.caps_changed);
	assert(st.mode == TP_CHORD_IDLE);

	tp_chord_finish(&st);
}

static void test_compose_meta_and_ctrl_chords(struct tp_lua *lua) {
	struct tp_chord_state st;
	tp_chord_init(&st);

	/* Separate press/release pairs send exactly Meta+A on the last press. */
	struct tp_chord_result r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(r.started && st.mode == TP_CHORD_PLURIPOTENT);
	r = tap(&st, lua, XKB_KEY_Super_L, 125);
	assert(r.updated && st.mode == TP_CHORD_BUILDING && st.mods == TP_MOD_META);
	r = tap(&st, lua, XKB_KEY_a, 30);
	assert(r.updated && !r.ended && st.sym == XKB_KEY_a);
	r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(r.kind == TP_CHORD_RES_FINISH_CHORD && r.ended);
	assert(r.mods == TP_MOD_META && r.sym == XKB_KEY_a && r.keycode == 30);
	assert(st.mode == TP_CHORD_IDLE);

	/* Caps Lock -> Meta -> BackSpace -> Caps Lock => Meta+BackSpace */
	tp_chord_on_key(&st, lua, XKB_KEY_Caps_Lock, XKB_KEY_Caps_Lock, 58, true,
					0);
	tp_chord_on_key(&st, lua, XKB_KEY_Super_L, XKB_KEY_Super_L, 125, true, 0);
	tp_chord_on_key(&st, lua, XKB_KEY_BackSpace, XKB_KEY_BackSpace, 14, true,
					0);
	r = tp_chord_on_key(&st, lua, XKB_KEY_Caps_Lock, XKB_KEY_Caps_Lock, 58,
						true, 0);
	assert(r.kind == TP_CHORD_RES_FINISH_CHORD);
	assert(r.mods == TP_MOD_META && r.sym == XKB_KEY_BackSpace);

	/* Modifier toggle off and back on: Ctrl -> Ctrl -> Ctrl -> x -> Caps */
	tp_chord_on_key(&st, lua, XKB_KEY_Caps_Lock, XKB_KEY_Caps_Lock, 58, true,
					0);
	tp_chord_on_key(&st, lua, XKB_KEY_Control_L, XKB_KEY_Control_L, 29, true,
					0);
	assert(st.mods == TP_MOD_CTRL);
	tp_chord_on_key(&st, lua, XKB_KEY_Control_L, XKB_KEY_Control_L, 29, true,
					0);
	assert(st.mods == 0);
	tp_chord_on_key(&st, lua, XKB_KEY_Control_L, XKB_KEY_Control_L, 29, true,
					0);
	tp_chord_on_key(&st, lua, XKB_KEY_x, XKB_KEY_x, 45, true, 0);
	r = tp_chord_on_key(&st, lua, XKB_KEY_Caps_Lock, XKB_KEY_Caps_Lock, 58,
						true, 0);
	assert(r.kind == TP_CHORD_RES_FINISH_CHORD);
	assert(r.mods == TP_MOD_CTRL && r.sym == XKB_KEY_x && r.keycode == 45);

	tp_chord_finish(&st);
}

static void test_right_alt_solo_and_altgr(struct tp_lua *lua) {
	struct tp_chord_state st;
	tp_chord_init(&st);

	/* Holding Right Alt + 'e' (AltGr) must NOT trigger chord builder. */
	struct tp_chord_result r =
		tp_chord_on_key(&st, lua, XKB_KEY_Alt_R, XKB_KEY_Alt_R, 100, true, 0);
	assert(r.kind == TP_CHORD_RES_PASSTHROUGH);
	r = tp_chord_on_key(&st, lua, XKB_KEY_e, XKB_KEY_e, 18, true, TP_MOD_ALT);
	assert(r.kind == TP_CHORD_RES_PASSTHROUGH);
	r = tp_chord_on_key(&st, lua, XKB_KEY_Alt_R, XKB_KEY_Alt_R, 100, false, 0);
	assert(r.kind == TP_CHORD_RES_PASSTHROUGH && !r.started);
	assert(st.mode == TP_CHORD_IDLE);

	/* Solo Right Alt tap enters BUILDING directly (not Pluripotent). */
	tp_chord_on_key(&st, lua, XKB_KEY_Alt_R, XKB_KEY_Alt_R, 100, true, 0);
	r = tp_chord_on_key(&st, lua, XKB_KEY_Alt_R, XKB_KEY_Alt_R, 100, false, 0);
	assert(r.started && st.mode == TP_CHORD_BUILDING);

	/* Typing "float" filters the menu; a solo Right Alt tap confirms. */
	const char *q = "float";
	for (size_t i = 0; q[i]; i++) {
		r = tp_chord_on_key(&st, lua, (xkb_keysym_t)q[i], (xkb_keysym_t)q[i],
							0, true, 0);
		assert(r.kind == TP_CHORD_RES_CONSUME && r.updated);
	}
	assert(st.filtered_len >= 1);
	r = tp_chord_on_key(&st, lua, XKB_KEY_Return, XKB_KEY_Return, 28, true, 0);
	assert(r.kind == TP_CHORD_RES_CONSUME && !r.ended);
	assert(st.mode == TP_CHORD_BUILDING);
	r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(!r.ended && !r.cancelled && st.mode == TP_CHORD_BUILDING);
	tp_chord_on_key(&st, lua, XKB_KEY_Alt_R, XKB_KEY_Alt_R, 100, true, 0);
	r = tp_chord_on_key(&st, lua, XKB_KEY_Alt_R, XKB_KEY_Alt_R, 100, false, 0);
	assert(r.kind == TP_CHORD_RES_RUN_ITEM && r.ended);
	assert(r.item.action.kind == TP_ACT_TOGGLE_FLOAT);
	assert(st.mode == TP_CHORD_IDLE);

	tp_chord_finish(&st);
}

static void test_cancel_paths(struct tp_lua *lua) {
	struct tp_chord_state st;
	tp_chord_init(&st);

	tp_chord_configure(&st, true, "Caps_Lock", "Alt_R", "Escape");

	tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	struct tp_chord_result r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(r.cancelled && !r.ended && st.mode == TP_CHORD_IDLE);

	tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	r = tap(&st, lua, XKB_KEY_BackSpace, 14);
	assert(!r.cancelled && st.mode == TP_CHORD_PLURIPOTENT);
	r = tap(&st, lua, XKB_KEY_Return, 28);
	assert(!r.ended && st.mode == TP_CHORD_PLURIPOTENT);
	tap(&st, lua, XKB_KEY_Control_L, 29);
	r = tap(&st, lua, XKB_KEY_Escape, 1);
	assert(!r.cancelled && !r.ended && st.mode == TP_CHORD_BUILDING);
	r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(r.kind == TP_CHORD_RES_FINISH_CHORD && r.mods == TP_MOD_CTRL);
	assert(r.sym == XKB_KEY_Escape && st.mode == TP_CHORD_IDLE);

	tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	r = tap(&st, lua, XKB_KEY_Alt_R, 100);
	assert(r.updated && st.mode == TP_CHORD_BUILDING && st.mods == TP_MOD_ALT);
	r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(r.cancelled && st.mode == TP_CHORD_IDLE);

	tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	tap(&st, lua, XKB_KEY_Escape, 1);
	r = tap(&st, lua, XKB_KEY_BackSpace, 14);
	assert(r.updated && !r.cancelled && st.sym == XKB_KEY_NoSymbol);
	r = tap(&st, lua, XKB_KEY_BackSpace, 14);
	assert(!r.cancelled && st.mode == TP_CHORD_BUILDING);
	r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(r.cancelled && st.mode == TP_CHORD_IDLE);

	tp_chord_finish(&st);
}

static void test_menu_confirmation(struct tp_lua *lua) {
	struct tp_chord_state st;
	tp_chord_init(&st);
	tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	tap(&st, lua, XKB_KEY_Down, 108);
	assert(tp_chord_menu_ready(&st));
	size_t bind = st.items[st.filtered[st.selected]].bind_index;
	struct tp_chord_result r = tap(&st, lua, XKB_KEY_Return, 28);
	assert(!r.ended && st.mode == TP_CHORD_BUILDING);
	r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(r.kind == TP_CHORD_RES_RUN_ITEM && r.item.bind_index == bind);
	assert(st.mode == TP_CHORD_IDLE);

	tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	tap(&st, lua, XKB_KEY_Down, 108);
	const char *q = "zzzznoresult";
	for (size_t i = 0; q[i]; i++)
		tap(&st, lua, (xkb_keysym_t)q[i], 0);
	assert(st.filtered_len == 0);
	r = tap(&st, lua, XKB_KEY_Caps_Lock, 58);
	assert(r.cancelled && !r.ended && st.mode == TP_CHORD_IDLE);
	tp_chord_finish(&st);
}

int main(void) {
	struct tp_lua *lua = tp_lua_create();
	assert(lua != NULL);

	test_caps_lock_three_states(lua);
	test_compose_meta_and_ctrl_chords(lua);
	test_right_alt_solo_and_altgr(lua);
	test_cancel_paths(lua);
	test_menu_confirmation(lua);

	tp_lua_destroy(lua);
	puts("test_chord: ok");
	return 0;
}
