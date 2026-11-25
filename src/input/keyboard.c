/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/bindings.h"
#include "tam-pane/chord.h"
#include "tam-pane/chord_overlay.h"
#include "tam-pane/idle.h"
#include "tam-pane/input.h"
#include "tam-pane/lua.h"
#include "tam-pane/server.h"
#include "tam-pane/server_events.h"
#include "tam-pane/text_input.h"
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <tam.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon.h>

static struct wlr_keyboard_modifiers sanitized_mods(struct tp_keyboard *k) {
	struct wlr_keyboard_modifiers m = k->wlr->modifiers;
	/* When one-handed Caps Lock chord mode is enabled, tam-pane manages
	 * Regular Caps Lock for A-Z in software so hardware Caps Lock presses
	 * in Pluripotent/Building mode never leave Lock stuck on clients. */
	if (k->server->chord.enabled &&
		k->server->chord.chord_sym == XKB_KEY_Caps_Lock) {
		m.locked &= ~(uint32_t)WLR_MODIFIER_CAPS;
	}
	return m;
}

static void on_modifiers(struct wl_listener *l, void *data) {
	struct tp_keyboard *k = wl_container_of(l, k, modifiers);
	tp_idle_notify_activity(k->server);
	if (tp_text_input_forward_modifiers(k->server, k->wlr))
		return;
	struct wlr_keyboard_modifiers m = sanitized_mods(k);
	wlr_seat_set_keyboard(k->server->seat, k->wlr);
	wlr_seat_keyboard_notify_modifiers(k->server->seat, &m);
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

static uint32_t tp_mods_to_wlr(uint32_t tp_mods) {
	uint32_t wlr = 0;
	if (tp_mods & TP_MOD_SHIFT)
		wlr |= WLR_MODIFIER_SHIFT;
	if (tp_mods & TP_MOD_CTRL)
		wlr |= WLR_MODIFIER_CTRL;
	if (tp_mods & TP_MOD_ALT)
		wlr |= WLR_MODIFIER_ALT;
	if (tp_mods & TP_MOD_META)
		wlr |= WLR_MODIFIER_LOGO;
	return wlr;
}

static void sync_caps_leds(struct tp_server *s) {
	uint32_t leds = tp_chord_is_caps_lock(&s->chord) ? WLR_LED_CAPS_LOCK : 0;
	struct tp_keyboard *k;
	wl_list_for_each(k, &s->keyboards, link) {
		uint32_t cur = 0;
		if (k->wlr->modifiers.locked & WLR_MODIFIER_MOD2)
			cur |= WLR_LED_NUM_LOCK;
		wlr_keyboard_led_update(k->wlr, cur | leds);
	}
}

/*
 * Synthesize a composed chord (e.g. Ctrl+X or shifted A-Z) to the focused
 * Wayland client. Emits BOTH modifier evdev key press/release events (needed
 * by tam-last's held-key bitmap) AND wl_keyboard.modifiers (needed by XKB
 * clients such as GTK, Qt, Chromium, and Foot).
 */
static void synthesize_chord(struct tp_keyboard *k, uint32_t mods,
							 uint32_t keycode, uint32_t time_msec) {
	struct wlr_seat *seat = k->server->seat;
	wlr_seat_set_keyboard(seat, k->wlr);

	static const struct {
		uint32_t bit;
		uint32_t evdev;
	} mod_keys[] = {
		{TP_MOD_META, KEY_LEFTMETA},
		{TP_MOD_CTRL, KEY_LEFTCTRL},
		{TP_MOD_ALT, KEY_LEFTALT},
		{TP_MOD_SHIFT, KEY_LEFTSHIFT},
	};

	for (size_t i = 0; i < sizeof(mod_keys) / sizeof(mod_keys[0]); i++) {
		if (mods & mod_keys[i].bit)
			wlr_seat_keyboard_notify_key(seat, time_msec, mod_keys[i].evdev,
										 WL_KEYBOARD_KEY_STATE_PRESSED);
	}

	struct wlr_keyboard_modifiers synth = sanitized_mods(k);
	synth.depressed |= tp_mods_to_wlr(mods);
	wlr_seat_keyboard_notify_modifiers(seat, &synth);

	wlr_seat_keyboard_notify_key(seat, time_msec, keycode,
								 WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(seat, time_msec, keycode,
								 WL_KEYBOARD_KEY_STATE_RELEASED);

	for (size_t i = sizeof(mod_keys) / sizeof(mod_keys[0]); i > 0; i--) {
		if (mods & mod_keys[i - 1].bit)
			wlr_seat_keyboard_notify_key(seat, time_msec, mod_keys[i - 1].evdev,
										 WL_KEYBOARD_KEY_STATE_RELEASED);
	}

	struct wlr_keyboard_modifiers real = sanitized_mods(k);
	wlr_seat_keyboard_notify_modifiers(seat, &real);
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
	struct tp_server *s = k->server;
	struct wlr_keyboard_key_event *ev = data;
	tp_idle_notify_activity(s);
	uint32_t word = ev->keycode / 64;
	uint64_t mask = UINT64_C(1) << (ev->keycode % 64);
	bool pressed = (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED);

	if (pressed && run_compositor_key(k, ev->keycode)) {
		if (word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]))
			k->consumed_keys[word] |= mask;
		return;
	}

	/* Route through one-handed chord composer when unlocked. */
	if (!s->locked && s->chord.enabled) {
		const xkb_keysym_t *syms = NULL;
		int n = level0_syms(k, ev->keycode, &syms);
		xkb_keysym_t raw_sym = (n > 0) ? syms[0] : XKB_KEY_NoSymbol;
		xkb_keysym_t sym = (raw_sym != XKB_KEY_NoSymbol)
							   ? xkb_keysym_to_lower(raw_sym)
							   : XKB_KEY_NoSymbol;
		uint32_t phys_mods = wlr_mods_to_tp(wlr_keyboard_get_modifiers(k->wlr));
		struct tp_chord_result cr = tp_chord_on_key(
			&s->chord, s->lua, raw_sym, sym, ev->keycode, pressed, phys_mods);

		if (cr.caps_changed)
			sync_caps_leds(s);
		if (cr.started) {
			tp_chord_overlay_update(s);
			tp_events_emit_chord(s, TP_EVENT_CHORD_START, NULL, NULL);
		} else if (cr.updated) {
			tp_chord_overlay_update(s);
			tp_events_emit_chord(s, TP_EVENT_CHORD_UPDATE, NULL, NULL);
		} else if (cr.cancelled) {
			tp_chord_overlay_update(s);
			tp_events_emit_chord(s, TP_EVENT_CHORD_CANCEL, NULL, NULL);
		}

		if (cr.kind == TP_CHORD_RES_CAPS_LETTER) {
			synthesize_chord(k, TP_MOD_SHIFT, cr.keycode, ev->time_msec);
			if (word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]))
				k->consumed_keys[word] |= mask;
			return;
		}
		if (cr.kind == TP_CHORD_RES_RUN_ITEM) {
			tp_chord_overlay_update(s);
			tp_events_emit_chord(s, TP_EVENT_CHORD_END, "menu", cr.item.desc);
			if (s->lua)
				tp_lua_run_bind_index(s->lua, cr.item.bind_index, s->locked);
			if (pressed &&
				word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]))
				k->consumed_keys[word] |= mask;
			return;
		}
		if (cr.kind == TP_CHORD_RES_FINISH_CHORD) {
			tp_chord_overlay_update(s);
			char cstr[64];
			tp_chord_format(cr.mods, cr.sym, cstr, sizeof(cstr));
			if (tp_bindings_handle(s, cr.mods, cr.sym)) {
				tp_events_emit_chord(s, TP_EVENT_CHORD_END, "dispatch", cstr);
			} else {
				synthesize_chord(k, cr.mods, cr.keycode, ev->time_msec);
				tp_events_emit_chord(s, TP_EVENT_CHORD_END, "forward", cstr);
			}
			if (pressed &&
				word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]))
				k->consumed_keys[word] |= mask;
			return;
		}
		if (cr.kind == TP_CHORD_RES_CONSUME) {
			if (pressed &&
				word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]))
				k->consumed_keys[word] |= mask;
			else if (!pressed &&
					 word < sizeof(k->consumed_keys) /
								sizeof(k->consumed_keys[0]))
				k->consumed_keys[word] &= ~mask;
			return;
		}
	}

	if (pressed) {
		if (run_binding(k, ev->keycode)) {
			if (word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]))
				k->consumed_keys[word] |= mask;
			return;
		}
	} else if (word < sizeof(k->consumed_keys) / sizeof(k->consumed_keys[0]) &&
			   (k->consumed_keys[word] & mask)) {
		k->consumed_keys[word] &= ~mask;
		return;
	}
	if (tp_text_input_forward_key(s, k->wlr, ev))
		return;
	wlr_seat_set_keyboard(s->seat, k->wlr);
	wlr_seat_keyboard_notify_key(s->seat, ev->time_msec, ev->keycode,
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
