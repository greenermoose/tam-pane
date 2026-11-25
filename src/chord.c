/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tam-pane/chord.h"
#include "tam-pane/ipc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>

static xkb_keysym_t resolve_key_name(const char *name, xkb_keysym_t fallback) {
	if (!name || !*name)
		return fallback;
	if (strcmp(name, "none") == 0 || strcmp(name, "off") == 0)
		return XKB_KEY_NoSymbol;
	xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol)
		sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
	return sym != XKB_KEY_NoSymbol ? sym : fallback;
}

static uint32_t keysym_to_mod(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
		return TP_MOD_SHIFT;
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		return TP_MOD_CTRL;
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_ISO_Level3_Shift:
		return TP_MOD_ALT;
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
		return TP_MOD_META;
	default:
		return 0;
	}
}

static bool ci_strstr(const char *hay, const char *needle) {
	if (!needle || !*needle)
		return true;
	if (!hay)
		return false;
	size_t nlen = strlen(needle);
	for (const char *p = hay; *p; p++) {
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

static void clear_pending(struct tp_chord_state *st) {
	st->mods = 0;
	st->sym = XKB_KEY_NoSymbol;
	st->keycode = 0;
	st->filter[0] = '\0';
	st->filter_len = 0;
	st->selected = 0;
	st->scroll = 0;
}

static void refilter(struct tp_chord_state *st) {
	if (st->items_len > st->filtered_cap) {
		size_t ncap = st->items_len ? st->items_len : 16;
		size_t *grown = realloc(st->filtered, ncap * sizeof(*grown));
		if (!grown)
			return;
		st->filtered = grown;
		st->filtered_cap = ncap;
	}
	st->filtered_len = 0;

	bool by_chord = (st->mods != 0 || st->sym != XKB_KEY_NoSymbol);
	for (size_t i = 0; i < st->items_len; i++) {
		const struct tp_chord_item *it = &st->items[i];
		if (by_chord) {
			if ((it->mods & st->mods) != st->mods)
				continue;
			if (st->sym != XKB_KEY_NoSymbol && it->sym != st->sym)
				continue;
		} else if (st->filter_len > 0) {
			if (!ci_strstr(it->chord_str, st->filter) &&
				!ci_strstr(it->desc, st->filter))
				continue;
		}
		st->filtered[st->filtered_len++] = i;
	}

	if (st->filtered_len == 0) {
		st->selected = 0;
		st->scroll = 0;
	} else {
		if (st->selected >= st->filtered_len)
			st->selected = st->filtered_len - 1;
		if (st->selected < st->scroll)
			st->scroll = st->selected;
		else if (st->selected >= st->scroll + TP_CHORD_VISIBLE_ROWS)
			st->scroll = st->selected - TP_CHORD_VISIBLE_ROWS + 1;
	}
}

static void move_selection(struct tp_chord_state *st, int delta) {
	if (st->filtered_len == 0)
		return;
	if (delta < 0) {
		size_t step = (size_t)(-delta);
		if (st->selected >= step)
			st->selected -= step;
		else
			st->selected = st->filtered_len - 1;
	} else if (delta > 0) {
		st->selected = (st->selected + (size_t)delta) % st->filtered_len;
	}
	if (st->selected < st->scroll)
		st->scroll = st->selected;
	else if (st->selected >= st->scroll + TP_CHORD_VISIBLE_ROWS)
		st->scroll = st->selected - TP_CHORD_VISIBLE_ROWS + 1;
}

void tp_chord_format(uint32_t mods, xkb_keysym_t sym, char *buf,
					 size_t buf_size) {
	if (!buf || buf_size == 0)
		return;
	size_t pos = 0;
	buf[0] = '\0';

	static const struct {
		uint32_t bit;
		const char *name;
	} mtab[] = {
		{TP_MOD_META, "Meta"},
		{TP_MOD_CTRL, "Ctrl"},
		{TP_MOD_ALT, "Alt"},
		{TP_MOD_SHIFT, "Shift"},
	};
	for (size_t i = 0; i < sizeof(mtab) / sizeof(mtab[0]); i++) {
		if (mods & mtab[i].bit) {
			int n = snprintf(buf + pos, buf_size - pos, "%s+", mtab[i].name);
			if (n > 0 && pos + (size_t)n < buf_size)
				pos += (size_t)n;
		}
	}

	if (sym != XKB_KEY_NoSymbol) {
		char kname[64];
		if (xkb_keysym_get_name(sym, kname, sizeof(kname)) < 0)
			snprintf(kname, sizeof(kname), "0x%x", (unsigned)sym);
		snprintf(buf + pos, buf_size - pos, "%s", kname);
	}
}

struct catalog_ctx {
	struct tp_chord_state *st;
	size_t idx;
};

static void on_catalog_bind(const struct tp_lua_bind_info *info,
							void *userdata) {
	struct catalog_ctx *ctx = userdata;
	struct tp_chord_state *st = ctx->st;
	size_t idx = ctx->idx++;

	if (st->items_len == st->items_cap) {
		size_t ncap = st->items_cap ? st->items_cap * 2 : 64;
		struct tp_chord_item *grown =
			realloc(st->items, ncap * sizeof(*grown));
		if (!grown)
			return;
		st->items = grown;
		st->items_cap = ncap;
	}

	struct tp_chord_item *it = &st->items[st->items_len++];
	memset(it, 0, sizeof(*it));
	it->mods = info->mods;
	it->sym = info->sym;
	it->is_lua = info->is_lua;
	it->bind_index = idx;
	it->action = info->action;
	tp_chord_format(info->mods, info->sym, it->chord_str,
					sizeof(it->chord_str));
	if (info->desc[0]) {
		snprintf(it->desc, sizeof(it->desc), "%s", info->desc);
	} else if (info->is_lua) {
		snprintf(it->desc, sizeof(it->desc), "lua callback");
	} else {
		tam_str_t s = tam_str_new("");
		if (s) {
			s = tp_ipc_format_action(s, &info->action);
			if (s) {
				snprintf(it->desc, sizeof(it->desc), "%s", s);
				tam_str_free(s);
			}
		}
	}
}

void tp_chord_rebuild_catalog(struct tp_chord_state *st,
							  const struct tp_lua *lua) {
	if (!st)
		return;
	st->items_len = 0;
	if (lua) {
		struct catalog_ctx ctx = {.st = st, .idx = 0};
		tp_lua_for_each_bind(lua, on_catalog_bind, &ctx);
	}
	refilter(st);
}

void tp_chord_init(struct tp_chord_state *st) {
	if (!st)
		return;
	memset(st, 0, sizeof(*st));
	st->enabled = true;
	st->chord_sym = XKB_KEY_Caps_Lock;
	st->secondary_sym = XKB_KEY_Alt_R;
	st->cancel_sym = XKB_KEY_Escape;
	st->mode = TP_CHORD_IDLE;
}

void tp_chord_finish(struct tp_chord_state *st) {
	if (!st)
		return;
	free(st->items);
	free(st->filtered);
	memset(st, 0, sizeof(*st));
}

void tp_chord_configure(struct tp_chord_state *st, bool enabled,
						const char *chord_key, const char *secondary_key,
						const char *cancel_key) {
	if (!st)
		return;
	st->enabled = enabled;
	st->chord_sym = resolve_key_name(chord_key, XKB_KEY_Caps_Lock);
	st->secondary_sym = resolve_key_name(secondary_key, XKB_KEY_Alt_R);
	st->cancel_sym = resolve_key_name(cancel_key, XKB_KEY_Escape);
	if (!enabled) {
		st->mode = TP_CHORD_IDLE;
		st->return_to_caps_lock = false;
		clear_pending(st);
	}
}

bool tp_chord_is_composing(const struct tp_chord_state *st) {
	return st && (st->mode == TP_CHORD_PLURIPOTENT ||
				  st->mode == TP_CHORD_BUILDING);
}

bool tp_chord_is_caps_lock(const struct tp_chord_state *st) {
	return st && st->mode == TP_CHORD_CAPS_LOCK;
}

const char *tp_chord_mode_name(enum tp_chord_mode mode) {
	switch (mode) {
	case TP_CHORD_IDLE:
		return "idle";
	case TP_CHORD_PLURIPOTENT:
		return "pluripotent";
	case TP_CHORD_BUILDING:
		return "building";
	case TP_CHORD_CAPS_LOCK:
		return "caps_lock";
	}
	return "idle";
}

bool tp_chord_open(struct tp_chord_state *st, const struct tp_lua *lua,
				   bool pluripotent) {
	if (!st || !st->enabled)
		return false;
	st->return_to_caps_lock = (st->mode == TP_CHORD_CAPS_LOCK);
	clear_pending(st);
	st->mode = pluripotent ? TP_CHORD_PLURIPOTENT : TP_CHORD_BUILDING;
	tp_chord_rebuild_catalog(st, lua);
	return true;
}

bool tp_chord_cancel(struct tp_chord_state *st) {
	if (!st || !tp_chord_is_composing(st))
		return false;
	clear_pending(st);
	st->mode = st->return_to_caps_lock ? TP_CHORD_CAPS_LOCK : TP_CHORD_IDLE;
	st->return_to_caps_lock = false;
	return true;
}

static struct tp_chord_result finish_or_cancel(struct tp_chord_state *st) {
	struct tp_chord_result res = {.kind = TP_CHORD_RES_CONSUME};
	if (st->mode == TP_CHORD_BUILDING && st->sym != XKB_KEY_NoSymbol) {
		res.kind = TP_CHORD_RES_FINISH_CHORD;
		res.mods = st->mods;
		res.sym = st->sym;
		res.keycode = st->keycode;
		res.ended = true;
	} else {
		res.cancelled = true;
	}
	clear_pending(st);
	st->mode = st->return_to_caps_lock ? TP_CHORD_CAPS_LOCK : TP_CHORD_IDLE;
	st->return_to_caps_lock = false;
	return res;
}

struct tp_chord_result tp_chord_on_key(struct tp_chord_state *st,
									   const struct tp_lua *lua,
									   xkb_keysym_t raw_sym, xkb_keysym_t sym,
									   uint32_t keycode, bool pressed,
									   uint32_t phys_mods) {
	struct tp_chord_result res = {.kind = TP_CHORD_RES_PASSTHROUGH};
	if (!st || !st->enabled)
		return res;

	/* Track solo secondary chord key (default: Right Alt tap). */
	if (st->secondary_sym != XKB_KEY_NoSymbol && raw_sym == st->secondary_sym) {
		if (pressed) {
			st->secondary_down = true;
			st->secondary_solo = true;
			if (tp_chord_is_composing(st))
				res.kind = TP_CHORD_RES_CONSUME;
			return res;
		}
		bool was_solo = st->secondary_down && st->secondary_solo;
		st->secondary_down = false;
		st->secondary_solo = false;
		if (was_solo) {
			if (!tp_chord_is_composing(st)) {
				tp_chord_open(st, lua, false);
				/* Pass through the release so client never sees a stuck key. */
				res.kind = TP_CHORD_RES_PASSTHROUGH;
				res.started = true;
				return res;
			}
			return finish_or_cancel(st);
		}
		if (tp_chord_is_composing(st))
			res.kind = TP_CHORD_RES_CONSUME;
		return res;
	}

	/* Any other key press cancels a pending solo Right Alt tap (preserves AltGr). */
	if (pressed && st->secondary_down)
		st->secondary_solo = false;

	/* Primary chord key (default: Caps Lock). */
	if (st->chord_sym != XKB_KEY_NoSymbol && raw_sym == st->chord_sym) {
		if (!pressed) {
			res.kind = TP_CHORD_RES_CONSUME;
			return res;
		}
		if (st->mode == TP_CHORD_CAPS_LOCK) {
			st->mode = TP_CHORD_IDLE;
			res.kind = TP_CHORD_RES_CONSUME;
			res.caps_changed = true;
			return res;
		}
		if (st->mode == TP_CHORD_IDLE) {
			bool pluri = (st->chord_sym == XKB_KEY_Caps_Lock);
			tp_chord_open(st, lua, pluri);
			res.kind = TP_CHORD_RES_CONSUME;
			res.started = true;
			return res;
		}
		return finish_or_cancel(st);
	}

	/* Regular Caps Lock mode: A-Z behave as shifted, all other keys pass through. */
	if (st->mode == TP_CHORD_CAPS_LOCK) {
		if (pressed && phys_mods == 0 && sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
			res.kind = TP_CHORD_RES_CAPS_LETTER;
			res.mods = TP_MOD_SHIFT;
			res.sym = sym;
			res.keycode = keycode;
		}
		return res;
	}

	if (st->mode == TP_CHORD_IDLE)
		return res;

	/* Inside PLURIPOTENT or BUILDING: consume releases and handle presses. */
	if (!pressed) {
		res.kind = TP_CHORD_RES_CONSUME;
		return res;
	}

	/* Cancel key (default: Escape). */
	if (st->cancel_sym != XKB_KEY_NoSymbol && raw_sym == st->cancel_sym) {
		tp_chord_cancel(st);
		res.kind = TP_CHORD_RES_CONSUME;
		res.cancelled = true;
		return res;
	}

	uint32_t mod_bit = keysym_to_mod(raw_sym);

	if (st->mode == TP_CHORD_PLURIPOTENT) {
		if (mod_bit != 0) {
			st->mode = TP_CHORD_BUILDING;
			st->mods |= mod_bit;
			refilter(st);
			res.kind = TP_CHORD_RES_CONSUME;
			res.updated = true;
			return res;
		}
		if (sym == XKB_KEY_Up || sym == XKB_KEY_Down ||
			sym == XKB_KEY_Page_Up || sym == XKB_KEY_Page_Down) {
			st->mode = TP_CHORD_BUILDING;
			int delta = (sym == XKB_KEY_Up) ? -1 :
						(sym == XKB_KEY_Down) ? 1 :
						(sym == XKB_KEY_Page_Up) ? -TP_CHORD_VISIBLE_ROWS :
						TP_CHORD_VISIBLE_ROWS;
			move_selection(st, delta);
			res.kind = TP_CHORD_RES_CONSUME;
			res.updated = true;
			return res;
		}
		if (sym == XKB_KEY_BackSpace) {
			tp_chord_cancel(st);
			res.kind = TP_CHORD_RES_CONSUME;
			res.cancelled = true;
			return res;
		}
		if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) &&
			st->filtered_len > 0) {
			res.kind = TP_CHORD_RES_RUN_ITEM;
			res.item = st->items[st->filtered[st->selected]];
			res.ended = true;
			clear_pending(st);
			st->mode = TP_CHORD_IDLE;
			st->return_to_caps_lock = false;
			return res;
		}
		/* Plain letter A-Z transitions from Pluripotent to Regular Caps Lock
		 * and immediately sends that first letter uppercase. */
		if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
			clear_pending(st);
			st->mode = TP_CHORD_CAPS_LOCK;
			st->return_to_caps_lock = false;
			res.kind = TP_CHORD_RES_CAPS_LETTER;
			res.mods = TP_MOD_SHIFT;
			res.sym = sym;
			res.keycode = keycode;
			res.cancelled = true; /* dismiss composer overlay */
			res.caps_changed = true;
			return res;
		}
		/* Any other non-letter key enters Chord Builder with that key. */
		st->mode = TP_CHORD_BUILDING;
		st->sym = sym;
		st->keycode = keycode;
		refilter(st);
		res.kind = TP_CHORD_RES_CONSUME;
		res.updated = true;
		return res;
	}

	/* TP_CHORD_BUILDING */
	if (mod_bit != 0) {
		st->mods ^= mod_bit;
		st->filter[0] = '\0';
		st->filter_len = 0;
		refilter(st);
		res.kind = TP_CHORD_RES_CONSUME;
		res.updated = true;
		return res;
	}

	if (st->mods == 0) {
		if (sym == XKB_KEY_Up || sym == XKB_KEY_Down ||
			sym == XKB_KEY_Page_Up || sym == XKB_KEY_Page_Down) {
			int delta = (sym == XKB_KEY_Up) ? -1 :
						(sym == XKB_KEY_Down) ? 1 :
						(sym == XKB_KEY_Page_Up) ? -TP_CHORD_VISIBLE_ROWS :
						TP_CHORD_VISIBLE_ROWS;
			move_selection(st, delta);
			res.kind = TP_CHORD_RES_CONSUME;
			res.updated = true;
			return res;
		}
		if (sym == XKB_KEY_BackSpace) {
			if (st->filter_len > 0) {
				st->filter[--st->filter_len] = '\0';
				refilter(st);
				res.kind = TP_CHORD_RES_CONSUME;
				res.updated = true;
				return res;
			}
			if (st->sym != XKB_KEY_NoSymbol) {
				st->sym = XKB_KEY_NoSymbol;
				st->keycode = 0;
				refilter(st);
				res.kind = TP_CHORD_RES_CONSUME;
				res.updated = true;
				return res;
			}
			tp_chord_cancel(st);
			res.kind = TP_CHORD_RES_CONSUME;
			res.cancelled = true;
			return res;
		}
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
			if (st->sym != XKB_KEY_NoSymbol)
				return finish_or_cancel(st);
			if (st->filtered_len > 0) {
				res.kind = TP_CHORD_RES_RUN_ITEM;
				res.item = st->items[st->filtered[st->selected]];
				res.ended = true;
				clear_pending(st);
				st->mode = st->return_to_caps_lock ? TP_CHORD_CAPS_LOCK :
													 TP_CHORD_IDLE;
				st->return_to_caps_lock = false;
				return res;
			}
			res.kind = TP_CHORD_RES_CONSUME;
			return res;
		}
		/* Printable ASCII filters the command menu by name/binding. */
		if (sym >= 0x20 && sym <= 0x7e) {
			if (st->filter_len + 1 < TP_CHORD_FILTER_MAX) {
				st->filter[st->filter_len++] = (char)sym;
				st->filter[st->filter_len] = '\0';
				refilter(st);
				res.kind = TP_CHORD_RES_CONSUME;
				res.updated = true;
			}
			return res;
		}
		st->sym = sym;
		st->keycode = keycode;
		refilter(st);
		res.kind = TP_CHORD_RES_CONSUME;
		res.updated = true;
		return res;
	}

	/* One or more modifiers latched: any non-modifier sets the chord's key. */
	st->sym = sym;
	st->keycode = keycode;
	refilter(st);
	res.kind = TP_CHORD_RES_CONSUME;
	res.updated = true;
	return res;
}
