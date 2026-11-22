/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/bindings.h"
#include "tam-pane/idle.h"
#include "tam-pane/input.h"
#include "tam-pane/lua.h"
#include "tam-pane/server.h"
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon.h>

static void on_modifiers(struct wl_listener *l, void *data) {
	struct tp_keyboard *k = wl_container_of(l, k, modifiers);
	tp_idle_notify_activity(k->server);
	wlr_seat_set_keyboard(k->server->seat, k->wlr);
	wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr->modifiers);
}

static uint32_t wlr_mods_to_tp(uint32_t wlr_mods) {
	uint32_t mods = 0;
	if (wlr_mods & WLR_MODIFIER_SHIFT)
		mods |= TP_MOD_SHIFT;
	if (wlr_mods & WLR_MODIFIER_CTRL)
		mods |= TP_MOD_CTRL;
	if (wlr_mods & WLR_MODIFIER_ALT)
		mods |= TP_MOD_ALT;
	if (wlr_mods & WLR_MODIFIER_LOGO)
		mods |= TP_MOD_META;
	return mods;
}

/*
 * Bindings match the key's unshifted symbol (level 0), with Shift as an
 * ordinary modifier, so "Meta+Shift+1" is written with "1" on any layout
 * rather than with whatever symbol Shift+1 makes.
 */
static int level0_syms(struct tp_keyboard *k, uint32_t keycode,
					   const xkb_keysym_t **syms) {
	xkb_keycode_t code = keycode + 8; /* evdev to xkb */
	xkb_layout_index_t layout =
		xkb_state_key_get_layout(k->wlr->xkb_state, code);
	return xkb_keymap_key_get_syms_by_level(k->wlr->keymap, code, layout, 0,
											syms);
}

/*
 * Chords handled here, before Lua bindings and clients, so that no config
 * can remove them (0.1.9):
 *   Ctrl+Alt+F1..F12          switch VT, only when tam-pane owns a session
 *                             (the keymap turns these into XF86Switch_VT_n);
 *   Ctrl+Alt+Shift+Backspace  emergency quit.
 */
static bool run_compositor_key(struct tp_keyboard *k, uint32_t keycode) {
	struct tp_server *s = k->server;
	const xkb_keysym_t *syms;
	int n = xkb_state_key_get_syms(k->wlr->xkb_state, keycode + 8, &syms);
	for (int i = 0; i < n; i++) {
		if (syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
			syms[i] <= XKB_KEY_XF86Switch_VT_12) {
			if (!s->change_vt)
				return false; /* nested or headless: an ordinary key */
			s->change_vt(s, syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
			return true;
		}
	}

	if (s->locked)
		return false; /* gate emergency quit while session is locked */

	uint32_t mods = wlr_mods_to_tp(wlr_keyboard_get_modifiers(k->wlr));
	if (mods != (TP_MOD_CTRL | TP_MOD_ALT | TP_MOD_SHIFT))
		return false;
	n = level0_syms(k, keycode, &syms);
	for (int i = 0; i < n; i++) {
		if (syms[i] == XKB_KEY_BackSpace) {
			tam_log_warn("emergency quit (Ctrl+Alt+Shift+Backspace)");
			tp_server_quit(s);
			return true;
		}
	}
	return false;
}

static bool run_binding(struct tp_keyboard *k, uint32_t keycode) {
	uint32_t mods = wlr_mods_to_tp(wlr_keyboard_get_modifiers(k->wlr));
	if (!mods && !tp_lua_has_unmodified_bindings(k->server->lua))
		return false;
	const xkb_keysym_t *syms;
	int n = level0_syms(k, keycode, &syms);
	for (int i = 0; i < n; i++)
		if (tp_bindings_handle(k->server, mods, syms[i]))
			return true;
	return false;
}

static void on_key(struct wl_listener *l, void *data) {
	struct tp_keyboard *k = wl_container_of(l, k, key);
	struct wlr_keyboard_key_event *ev = data;
	tp_idle_notify_activity(k->server);
	uint32_t word = ev->keycode / 64;
	uint64_t mask = UINT64_C(1) << (ev->keycode % 64);

	if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (run_compositor_key(k, ev->keycode) || run_binding(k, ev->keycode)) {
			if (word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]))
				k->consumed_keys[word] |= mask;
			return;
		}
	} else if (word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]) &&
			   (k->consumed_keys[word] & mask)) {
		k->consumed_keys[word] &= ~mask;
		return;
	}
	wlr_seat_set_keyboard(k->server->seat, k->wlr);
	wlr_seat_keyboard_notify_key(k->server->seat, ev->time_msec, ev->keycode,
								 ev->state);
}

static void keyboard_free(struct tp_keyboard *k) {
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	free(k);
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct tp_keyboard *k = wl_container_of(l, k, destroy);
	struct tp_server *s = k->server;
	keyboard_free(k);
	if (s->display)
		tp_input_update_caps(s);
}

void tp_keyboard_add(struct tp_server *s, struct wlr_input_device *dev) {
	struct wlr_keyboard *wlr = wlr_keyboard_from_input_device(dev);
	struct tp_keyboard *k = calloc(1, sizeof(*k));
	if (!k)
		return;
	k->server = s;
	k->wlr = wlr;

	/* The keymap comes from XKB_DEFAULT_* until Lua config sets it. */
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap =
		ctx ? xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS)
			: NULL;
	if (!keymap) {
		tam_log_error("keyboard %s: cannot compile a keymap", dev->name);
		xkb_context_unref(ctx);
		free(k);
		return;
	}
	wlr_keyboard_set_keymap(wlr, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(wlr, s->settings.repeat_rate,
								 s->settings.repeat_delay);

	k->modifiers.notify = on_modifiers;
	wl_signal_add(&wlr->events.modifiers, &k->modifiers);
	k->key.notify = on_key;
	wl_signal_add(&wlr->events.key, &k->key);
	k->destroy.notify = on_destroy;
	wl_signal_add(&dev->events.destroy, &k->destroy);

	wlr_seat_set_keyboard(s->seat, wlr);
	wl_list_insert(&s->keyboards, &k->link);
}

void tp_keyboards_finish(struct tp_server *s) {
	struct tp_keyboard *k, *tmp;
	wl_list_for_each_safe(k, tmp, &s->keyboards, link) keyboard_free(k);
}

void tp_keyboards_apply_settings(struct tp_server *s) {
	struct tp_keyboard *k;
	wl_list_for_each(k, &s->keyboards, link) {
		wlr_keyboard_set_repeat_info(k->wlr, s->settings.repeat_rate,
									 s->settings.repeat_delay);
	}
}
