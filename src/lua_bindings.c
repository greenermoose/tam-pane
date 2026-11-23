/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Lua key bindings, chord parser, and tp.dsp.* dispatcher values.
 *
 * Pure module (no wlroots): links only xkbcommon for keysym lookup and
 * canonicalization. Default bindings are loaded from an embedded Lua chunk
 * using the exact same tp.bind / tp.dsp.* API exposed to init.lua.
 */
#include "lua_internal.h"
#include "tam-pane/layout/layout.h"
#include "tam-pane/tags.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tam.h>

#define TP_DSP_MT "tp.dispatcher"

static const char default_bindings_lua[] =
	"tp.bind('Meta+Return', tp.dsp.terminal())\n"
	"tp.bind('Meta+w', tp.dsp.close())\n"
	"tp.bind('Meta+Shift+e', tp.dsp.quit())\n"
	"tp.bind('Meta+Shift+r', tp.dsp.reload())\n"
	"tp.bind('Meta+j', tp.dsp.focus('next'))\n"
	"tp.bind('Meta+k', tp.dsp.focus('prev'))\n"
	"tp.bind('Meta+v', tp.dsp.toggle_float())\n"
	"tp.bind('Meta+f', tp.dsp.toggle_fullscreen())\n"
	"tp.bind('Meta+Space', tp.dsp.layout('next'))\n"
	"tp.bind('Meta+Shift+Space', tp.dsp.layout('prev'))\n"
	"tp.bind('Meta+minus', tp.dsp.scratchpad('toggle'))\n"
	"tp.bind('Meta+Shift+minus', tp.dsp.scratchpad('move'))\n"
	"tp.bind('Meta+Tab', tp.dsp.tag('back'))\n"
	"tp.bind('Meta+comma', tp.dsp.monitor('focus', 'prev'))\n"
	"tp.bind('Meta+period', tp.dsp.monitor('focus', 'next'))\n"
	"tp.bind('Meta+Shift+comma', tp.dsp.monitor('move', 'prev'))\n"
	"tp.bind('Meta+Shift+period', tp.dsp.monitor('move', 'next'))\n"
	"for _, d in ipairs({'left', 'right', 'up', 'down'}) do\n"
	"  local key = d:sub(1, 1):upper() .. d:sub(2)\n"
	"  tp.bind('Meta+Shift+' .. key, tp.dsp.swap(d))\n"
	"  tp.bind('Meta+Ctrl+' .. key, tp.dsp.move(d))\n"
	"  tp.bind('Meta+Alt+' .. key, tp.dsp.resize(d))\n"
	"end\n"
	"for i = 1, 9 do\n"
	"  local k = tostring(i)\n"
	"  tp.bind('Meta+' .. k, tp.dsp.tag('view', i))\n"
	"  tp.bind('Meta+Shift+' .. k, tp.dsp.tag('move', i))\n"
	"  tp.bind('Meta+Ctrl+' .. k, tp.dsp.tag('toggle_view', i))\n"
	"  tp.bind('Meta+Shift+Ctrl+' .. k, tp.dsp.tag('toggle_client', i))\n"
	"end\n";

static bool ascii_ieq_n(const char *a, size_t len, const char *b) {
	for (size_t i = 0; i < len; i++) {
		if (b[i] == '\0')
			return false;
		if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
			return false;
	}
	return b[len] == '\0';
}

static void trim_span(const char **start, size_t *len) {
	const char *s = *start;
	size_t n = *len;
	while (n > 0 && isspace((unsigned char)*s)) {
		s++;
		n--;
	}
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		n--;
	*start = s;
	*len = n;
}

static bool parse_modifier(const char *tok, size_t len, uint32_t *mods) {
	if (ascii_ieq_n(tok, len, "meta") || ascii_ieq_n(tok, len, "super") ||
		ascii_ieq_n(tok, len, "logo") || ascii_ieq_n(tok, len, "mod4")) {
		*mods |= TP_MOD_META;
		return true;
	}
	if (ascii_ieq_n(tok, len, "shift")) {
		*mods |= TP_MOD_SHIFT;
		return true;
	}
	if (ascii_ieq_n(tok, len, "ctrl") || ascii_ieq_n(tok, len, "control")) {
		*mods |= TP_MOD_CTRL;
		return true;
	}
	if (ascii_ieq_n(tok, len, "alt") || ascii_ieq_n(tok, len, "mod1")) {
		*mods |= TP_MOD_ALT;
		return true;
	}
	return false;
}

static xkb_keysym_t parse_keysym(const char *tok, size_t len) {
	if (len == 1) {
		switch (tok[0]) {
		case ',':
			return XKB_KEY_comma;
		case '.':
			return XKB_KEY_period;
		case '/':
			return XKB_KEY_slash;
		case '-':
			return XKB_KEY_minus;
		case '=':
			return XKB_KEY_equal;
		case '+':
			return XKB_KEY_plus;
		default:
			break;
		}
	}
	if (ascii_ieq_n(tok, len, "enter"))
		return XKB_KEY_Return;
	if (ascii_ieq_n(tok, len, "esc"))
		return XKB_KEY_Escape;
	if (ascii_ieq_n(tok, len, "space"))
		return XKB_KEY_space;

	if (len >= 64)
		return XKB_KEY_NoSymbol;
	char buf[64];
	memcpy(buf, tok, len);
	buf[len] = '\0';

	xkb_keysym_t sym = xkb_keysym_from_name(buf, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol)
		sym = xkb_keysym_from_name(buf, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol)
		return XKB_KEY_NoSymbol;
	return xkb_keysym_to_lower(sym);
}

bool tp_binding_parse_chord(const char *str, struct tp_key_spec *out, char *err,
							size_t err_size) {
	if (!str || !out)
		return false;

	const char *s = str;
	size_t len = strlen(s);
	trim_span(&s, &len);
	if (len == 0) {
		if (err && err_size > 0)
			snprintf(err, err_size, "empty key chord");
		return false;
	}

	const char *mod_str = NULL;
	size_t mod_len = 0;
	const char *key_str = NULL;
	size_t key_len = 0;

	if (s[len - 1] == '+') {
		if (len == 1) {
			key_str = "+";
			key_len = 1;
		} else if (s[len - 2] == '+') {
			mod_str = s;
			mod_len = len - 2;
			key_str = "+";
			key_len = 1;
		} else {
			if (err && err_size > 0)
				snprintf(err, err_size, "missing key after '+'");
			return false;
		}
	} else {
		const char *last_plus = NULL;
		for (size_t i = 0; i < len; i++) {
			if (s[i] == '+')
				last_plus = s + i;
		}
		if (!last_plus) {
			key_str = s;
			key_len = len;
		} else {
			mod_str = s;
			mod_len = (size_t)(last_plus - s);
			key_str = last_plus + 1;
			key_len = len - mod_len - 1;
		}
	}

	uint32_t mods = 0;
	if (mod_str) {
		if (mod_len == 0) {
			if (err && err_size > 0)
				snprintf(err, err_size, "empty modifier in chord");
			return false;
		}
		const char *p = mod_str;
		const char *end = mod_str + mod_len;
		for (;;) {
			const char *plus = memchr(p, '+', (size_t)(end - p));
			const char *tok = p;
			size_t tlen = plus ? (size_t)(plus - p) : (size_t)(end - p);
			trim_span(&tok, &tlen);
			if (tlen == 0) {
				if (err && err_size > 0)
					snprintf(err, err_size, "empty modifier in chord");
				return false;
			}
			if (!parse_modifier(tok, tlen, &mods)) {
				if (err && err_size > 0)
					snprintf(err, err_size, "unknown modifier '%.*s'",
							 (int)tlen, tok);
				return false;
			}
			if (!plus)
				break;
			p = plus + 1;
		}
	}

	trim_span(&key_str, &key_len);
	if (key_len == 0) {
		if (err && err_size > 0)
			snprintf(err, err_size, "missing key in chord");
		return false;
	}
	xkb_keysym_t sym = parse_keysym(key_str, key_len);
	if (sym == XKB_KEY_NoSymbol) {
		if (err && err_size > 0)
			snprintf(err, err_size, "unknown key '%.*s'", (int)key_len,
					 key_str);
		return false;
	}

	out->mods = mods;
	out->sym = sym;
	return true;
}

/* ---- Binding list helpers ---- */

static void bind_list_clear(lua_State *L, struct tp_lua_bind_list *list) {
	if (L && list->items) {
		for (size_t i = 0; i < list->len; i++) {
			if (list->items[i].is_lua && list->items[i].lua_ref != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, list->items[i].lua_ref);
		}
	}
	free(list->items);
	list->items = NULL;
	list->len = 0;
	list->cap = 0;
	list->has_unmodified = false;
}

static void bind_list_recalc_unmodified(struct tp_lua_bind_list *list) {
	list->has_unmodified = false;
	for (size_t i = 0; i < list->len; i++) {
		if (list->items[i].mods == 0) {
			list->has_unmodified = true;
			return;
		}
	}
}

static bool bind_list_clone(lua_State *L, const struct tp_lua_bind_list *src,
							struct tp_lua_bind_list *dst) {
	bind_list_clear(L, dst);
	if (src->len == 0)
		return true;

	dst->items = calloc(src->len, sizeof(*dst->items));
	if (!dst->items)
		return false;
	dst->cap = src->len;

	for (size_t i = 0; i < src->len; i++) {
		dst->items[i] = src->items[i];
		if (src->items[i].is_lua && src->items[i].lua_ref != LUA_NOREF) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, src->items[i].lua_ref);
			dst->items[i].lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		} else {
			dst->items[i].lua_ref = LUA_NOREF;
		}
		dst->len = i + 1;
	}
	dst->has_unmodified = src->has_unmodified;
	return true;
}

static bool bind_list_set(lua_State *L, struct tp_lua_bind_list *list,
						  const struct tp_lua_binding *entry) {
	for (size_t i = 0; i < list->len; i++) {
		if (list->items[i].mods == entry->mods &&
			list->items[i].sym == entry->sym) {
			if (list->items[i].is_lua && list->items[i].lua_ref != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, list->items[i].lua_ref);
			list->items[i] = *entry;
			if (entry->mods == 0)
				list->has_unmodified = true;
			return true;
		}
	}

	if (list->len == list->cap) {
		size_t next_cap = list->cap ? list->cap * 2 : 64;
		struct tp_lua_binding *grown =
			realloc(list->items, next_cap * sizeof(*grown));
		if (!grown)
			return false;
		list->items = grown;
		list->cap = next_cap;
	}
	list->items[list->len++] = *entry;
	if (entry->mods == 0)
		list->has_unmodified = true;
	return true;
}

static bool bind_list_remove(lua_State *L, struct tp_lua_bind_list *list,
							 uint32_t mods, xkb_keysym_t sym) {
	for (size_t i = 0; i < list->len; i++) {
		if (list->items[i].mods == mods && list->items[i].sym == sym) {
			if (list->items[i].is_lua && list->items[i].lua_ref != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, list->items[i].lua_ref);
			if (i + 1 < list->len)
				memmove(&list->items[i], &list->items[i + 1],
						(list->len - i - 1) * sizeof(*list->items));
			list->len--;
			if (mods == 0)
				bind_list_recalc_unmodified(list);
			return true;
		}
	}
	return false;
}

/* ---- Dispatcher userdata & constructors (tp.dsp.*) ---- */

static int push_dispatcher(lua_State *L, const struct tp_action *act) {
	struct tp_action *ud = lua_newuserdatauv(L, sizeof(*ud), 0);
	*ud = *act;
	luaL_setmetatable(L, TP_DSP_MT);
	return 1;
}

static enum tp_dir check_cardinal_dir(lua_State *L, int idx, const char *fn) {
	const char *s = luaL_checkstring(L, idx);
	if (strcmp(s, "left") == 0)
		return TP_DIR_LEFT;
	if (strcmp(s, "right") == 0)
		return TP_DIR_RIGHT;
	if (strcmp(s, "up") == 0)
		return TP_DIR_UP;
	if (strcmp(s, "down") == 0)
		return TP_DIR_DOWN;
	return (enum tp_dir)luaL_error(
		L, "tp.dsp.%s: expected 'left', 'right', 'up', or 'down'", fn);
}

static int check_cycle_dir(lua_State *L, int idx, const char *fn) {
	const char *s = luaL_checkstring(L, idx);
	if (strcmp(s, "next") == 0)
		return +1;
	if (strcmp(s, "prev") == 0)
		return -1;
	return luaL_error(L, "tp.dsp.%s: expected 'next' or 'prev'", fn);
}

static int l_dsp_spawn(lua_State *L) {
	size_t len = 0;
	const char *cmd = luaL_checklstring(L, 1, &len);
	if (len == 0)
		return luaL_error(L, "tp.dsp.spawn: command must not be empty");
	struct tp_action act = {.kind = TP_ACT_SPAWN};
	if (len >= sizeof(act.cmd))
		return luaL_error(L, "tp.dsp.spawn: command too long (max %d bytes)",
						  (int)(sizeof(act.cmd) - 1));
	memcpy(act.cmd, cmd, len + 1);
	return push_dispatcher(L, &act);
}

static int l_dsp_terminal(lua_State *L) {
	struct tp_action act = {.kind = TP_ACT_TERMINAL};
	return push_dispatcher(L, &act);
}

static int l_dsp_close(lua_State *L) {
	struct tp_action act = {.kind = TP_ACT_CLOSE};
	return push_dispatcher(L, &act);
}

static int l_dsp_quit(lua_State *L) {
	struct tp_action act = {.kind = TP_ACT_QUIT};
	return push_dispatcher(L, &act);
}

static int l_dsp_focus(lua_State *L) {
	struct tp_action act = {
		.kind = TP_ACT_FOCUS,
		.dir = check_cycle_dir(L, 1, "focus"),
	};
	return push_dispatcher(L, &act);
}

static int l_dsp_swap(lua_State *L) {
	struct tp_action act = {
		.kind = TP_ACT_SWAP,
		.dir = (int)check_cardinal_dir(L, 1, "swap"),
	};
	return push_dispatcher(L, &act);
}

static int l_dsp_move(lua_State *L) {
	struct tp_action act = {
		.kind = TP_ACT_MOVE,
		.dir = (int)check_cardinal_dir(L, 1, "move"),
	};
	return push_dispatcher(L, &act);
}

static int l_dsp_resize(lua_State *L) {
	struct tp_action act = {
		.kind = TP_ACT_RESIZE,
		.dir = (int)check_cardinal_dir(L, 1, "resize"),
		.step = 0,
	};
	if (!lua_isnoneornil(L, 2)) {
		if (lua_type(L, 2) != LUA_TNUMBER || !lua_isinteger(L, 2))
			return luaL_error(L, "tp.dsp.resize: step must be an integer");
		lua_Integer step = lua_tointeger(L, 2);
		if (step < 1 || step > 500)
			return luaL_error(L, "tp.dsp.resize: step out of range [1, 500]");
		act.step = (int32_t)step;
	}
	return push_dispatcher(L, &act);
}

static int l_dsp_toggle_float(lua_State *L) {
	struct tp_action act = {.kind = TP_ACT_TOGGLE_FLOAT};
	return push_dispatcher(L, &act);
}

static int l_dsp_toggle_fullscreen(lua_State *L) {
	struct tp_action act = {.kind = TP_ACT_TOGGLE_FULLSCREEN};
	return push_dispatcher(L, &act);
}

static int l_dsp_tag(lua_State *L) {
	const char *op = luaL_checkstring(L, 1);
	struct tp_action act = {0};
	if (strcmp(op, "back") == 0) {
		act.kind = TP_ACT_TAG_BACK;
		return push_dispatcher(L, &act);
	}
	if (strcmp(op, "view") == 0) {
		act.kind = TP_ACT_TAG_VIEW;
	} else if (strcmp(op, "toggle_view") == 0) {
		act.kind = TP_ACT_TAG_TOGGLE_VIEW;
	} else if (strcmp(op, "move") == 0) {
		act.kind = TP_ACT_TAG_MOVE;
	} else if (strcmp(op, "toggle_client") == 0) {
		act.kind = TP_ACT_TAG_TOGGLE_CLIENT;
	} else {
		return luaL_error(
			L, "tp.dsp.tag: expected 'view', 'toggle_view', 'move', "
			   "'toggle_client', or 'back'");
	}
	act.tag = (unsigned int)tp_lua_check_int(L, 2, "tp.dsp.tag", 1,
											 (int32_t)TP_TAG_COUNT);
	return push_dispatcher(L, &act);
}

static int l_dsp_monitor(lua_State *L) {
	const char *op = luaL_checkstring(L, 1);
	struct tp_action act = {0};
	if (strcmp(op, "focus") == 0)
		act.kind = TP_ACT_MONITOR_FOCUS;
	else if (strcmp(op, "move") == 0)
		act.kind = TP_ACT_MONITOR_MOVE;
	else
		return luaL_error(L, "tp.dsp.monitor: expected 'focus' or 'move'");
	act.dir = check_cycle_dir(L, 2, "monitor");
	return push_dispatcher(L, &act);
}

static int l_dsp_reload(lua_State *L) {
	struct tp_action act = {.kind = TP_ACT_RELOAD};
	return push_dispatcher(L, &act);
}

static int l_dsp_layout(lua_State *L) {
	const char *op = luaL_checkstring(L, 1);
	struct tp_action act = {0};
	if (strcmp(op, "set") == 0) {
		const char *name = luaL_checkstring(L, 2);
		enum tp_layout_kind kind;
		if (!tp_layout_kind_from_name(name, &kind))
			return luaL_error(
				L, "tp.dsp.layout: expected 'dwindle', 'master', 'scroller', "
				   "or 'monocle'");
		act.kind = TP_ACT_LAYOUT_SET;
		act.dir = (int)kind;
		return push_dispatcher(L, &act);
	}
	if (strcmp(op, "next") == 0) {
		act.kind = TP_ACT_LAYOUT_CYCLE;
		act.dir = 1;
		return push_dispatcher(L, &act);
	}
	if (strcmp(op, "prev") == 0) {
		act.kind = TP_ACT_LAYOUT_CYCLE;
		act.dir = -1;
		return push_dispatcher(L, &act);
	}
	if (strcmp(op, "nmaster") == 0) {
		int32_t d = tp_lua_check_int(L, 2, "tp.dsp.layout", -8, 8);
		if (d == 0)
			return luaL_error(L, "tp.dsp.layout: nmaster delta must be non-zero");
		act.kind = TP_ACT_LAYOUT_NMASTER;
		act.dir = (int)d;
		return push_dispatcher(L, &act);
	}
	return luaL_error(
		L, "tp.dsp.layout: expected 'set', 'next', 'prev', or 'nmaster'");
}

static int l_dsp_scratchpad(lua_State *L) {
	const char *op = luaL_checkstring(L, 1);
	struct tp_action act = {0};
	if (strcmp(op, "toggle") == 0)
		act.kind = TP_ACT_SCRATCHPAD_TOGGLE;
	else if (strcmp(op, "move") == 0)
		act.kind = TP_ACT_SCRATCHPAD_MOVE;
	else
		return luaL_error(L, "tp.dsp.scratchpad: expected 'toggle' or 'move'");
	return push_dispatcher(L, &act);
}

static int l_dsp_call(lua_State *L) {
	struct tp_action *act = luaL_checkudata(L, 1, TP_DSP_MT);
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || lua->staging || lua->staging_binds)
		return luaL_error(L, "dispatcher cannot run while loading config");
	if (!lua->action_fn)
		return luaL_error(L, "dispatcher has no active compositor target");
	lua->action_fn(act, lua->action_userdata);
	return 0;
}

static int l_dsp_tostring(lua_State *L) {
	struct tp_action *act = luaL_checkudata(L, 1, TP_DSP_MT);
	lua_pushfstring(L, "tp.dispatcher(%d)", (int)act->kind);
	return 1;
}

/* ---- tp.bind / tp.unbind / tp.clear_binds ---- */

static int l_tp_bind(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_binds)
		return luaL_error(L, "tp.bind can only be called while loading config");

	const char *chord = luaL_checkstring(L, 1);
	struct tp_key_spec spec;
	char err[128] = {0};
	if (!tp_binding_parse_chord(chord, &spec, err, sizeof(err)))
		return luaL_error(L, "tp.bind: %s", err);

	struct tp_lua_binding entry = {
		.mods = spec.mods,
		.sym = spec.sym,
		.lua_ref = LUA_NOREF,
	};

	if (!lua_isnoneornil(L, 3)) {
		if (lua_type(L, 3) != LUA_TTABLE)
			return luaL_error(L, "tp.bind: options (argument 3) must be a table");
		lua_pushnil(L);
		while (lua_next(L, 3) != 0) {
			if (lua_type(L, -2) != LUA_TSTRING)
				return luaL_error(L, "tp.bind: option keys must be strings");
			const char *k = lua_tostring(L, -2);
			if (strcmp(k, "locked") == 0) {
				if (lua_type(L, -1) != LUA_TBOOLEAN)
					return luaL_error(L, "tp.bind: locked must be a boolean");
				entry.allow_locked = lua_toboolean(L, -1) != 0;
			} else {
				tam_log_warn("tp.bind: unknown option '%s' ignored", k);
			}
			lua_pop(L, 1);
		}
	}

	struct tp_action *dsp = luaL_testudata(L, 2, TP_DSP_MT);
	if (dsp) {
		entry.is_lua = false;
		entry.action = *dsp;
	} else if (lua_isfunction(L, 2)) {
		entry.is_lua = true;
		lua_pushvalue(L, 2);
		entry.lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		return luaL_error(
			L, "tp.bind: expected a tp.dispatcher or function as argument 2");
	}

	if (!bind_list_set(L, &lua->staged_binds, &entry)) {
		if (entry.is_lua && entry.lua_ref != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, entry.lua_ref);
		return luaL_error(L, "tp.bind: out of memory");
	}
	return 0;
}

static int l_tp_unbind(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_binds)
		return luaL_error(L, "tp.unbind can only be called while loading config");

	const char *chord = luaL_checkstring(L, 1);
	struct tp_key_spec spec;
	char err[128] = {0};
	if (!tp_binding_parse_chord(chord, &spec, err, sizeof(err)))
		return luaL_error(L, "tp.unbind: %s", err);

	bool removed =
		bind_list_remove(L, &lua->staged_binds, spec.mods, spec.sym);
	lua_pushboolean(L, removed);
	return 1;
}

static int l_tp_clear_binds(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_binds)
		return luaL_error(
			L, "tp.clear_binds can only be called while loading config");
	bind_list_clear(L, &lua->staged_binds);
	return 0;
}

static const luaL_Reg dsp_funcs[] = {
	{"spawn", l_dsp_spawn},
	{"terminal", l_dsp_terminal},
	{"close", l_dsp_close},
	{"quit", l_dsp_quit},
	{"focus", l_dsp_focus},
	{"swap", l_dsp_swap},
	{"move", l_dsp_move},
	{"resize", l_dsp_resize},
	{"toggle_float", l_dsp_toggle_float},
	{"toggle_fullscreen", l_dsp_toggle_fullscreen},
	{"tag", l_dsp_tag},
	{"monitor", l_dsp_monitor},
	{"reload", l_dsp_reload},
	{"layout", l_dsp_layout},
	{"scratchpad", l_dsp_scratchpad},
	{NULL, NULL},
};

void tp_lua_bindings_open(lua_State *L) {
	/* Dispatcher metatable. */
	luaL_newmetatable(L, TP_DSP_MT);
	lua_pushcfunction(L, l_dsp_call);
	lua_setfield(L, -2, "__call");
	lua_pushcfunction(L, l_dsp_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);

	/* tp.bind, tp.unbind, tp.clear_binds, tp.dsp (tp table is at -1). */
	lua_pushcfunction(L, l_tp_bind);
	lua_setfield(L, -2, "bind");
	lua_pushcfunction(L, l_tp_unbind);
	lua_setfield(L, -2, "unbind");
	lua_pushcfunction(L, l_tp_clear_binds);
	lua_setfield(L, -2, "clear_binds");

	luaL_newlib(L, dsp_funcs);
	lua_setfield(L, -2, "dsp");
}

bool tp_lua_bindings_stage_begin(struct tp_lua *lua) {
	if (!bind_list_clone(lua->L, &lua->active_binds, &lua->staged_binds))
		return false;
	lua->staging_binds = true;
	return true;
}

void tp_lua_bindings_stage_commit(struct tp_lua *lua) {
	lua->staging_binds = false;
	bind_list_clear(lua->L, &lua->active_binds);
	lua->active_binds = lua->staged_binds;
	lua->staged_binds = (struct tp_lua_bind_list){0};
}

void tp_lua_bindings_stage_abort(struct tp_lua *lua) {
	lua->staging_binds = false;
	bind_list_clear(lua->L, &lua->staged_binds);
}

void tp_lua_bindings_finish(struct tp_lua *lua) {
	if (!lua)
		return;
	bind_list_clear(lua->L, &lua->staged_binds);
	bind_list_clear(lua->L, &lua->active_binds);
	lua->staging_binds = false;
}

bool tp_lua_bindings_load_defaults(struct tp_lua *lua) {
	lua_State *L = lua->L;
	int top = lua_gettop(L);

	if (!tp_lua_bindings_stage_begin(lua))
		return false;

	int rc = luaL_loadbufferx(L, default_bindings_lua,
							  sizeof(default_bindings_lua) - 1,
							  "default_bindings", "t");
	if (rc == LUA_OK) {
		rc = tp_lua_guarded_pcall(lua, 0, 0, TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, "default_bindings");
	}
	if (rc != LUA_OK) {
		tam_log_error("failed to load default Lua bindings: %s",
					  lua_tostring(L, -1));
		tp_lua_bindings_stage_abort(lua);
		lua_settop(L, top);
		return false;
	}

	tp_lua_bindings_stage_commit(lua);
	lua_settop(L, top);
	return true;
}

void tp_lua_set_action_fn(struct tp_lua *lua, tp_lua_action_fn fn,
						  void *userdata) {
	if (!lua)
		return;
	lua->action_fn = fn;
	lua->action_userdata = userdata;
}

bool tp_lua_handle_key(struct tp_lua *lua, uint32_t mods, xkb_keysym_t sym,
					   bool locked) {
	if (!lua || !lua->L)
		return false;

	xkb_keysym_t key = xkb_keysym_to_lower(sym);
	const struct tp_lua_binding *found = NULL;
	for (size_t i = 0; i < lua->active_binds.len; i++) {
		if (lua->active_binds.items[i].mods == mods &&
			lua->active_binds.items[i].sym == key) {
			found = &lua->active_binds.items[i];
			break;
		}
	}
	if (!found || (locked && !found->allow_locked))
		return false;

	if (!found->is_lua) {
		if (lua->action_fn)
			lua->action_fn(&found->action, lua->action_userdata);
		return true;
	}

	lua_State *L = lua->L;
	int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, found->lua_ref);
	int rc = tp_lua_guarded_pcall(lua, 0, 0, TP_LUA_CALLBACK_MAX_INSTRUCTIONS,
								  TP_LUA_CALLBACK_TIMEOUT_MS, "binding");
	if (rc != LUA_OK) {
		const char *msg = lua_tostring(L, -1);
		tam_log_error("lua binding failed: %s", msg ? msg : "unknown error");
	}
	lua_settop(L, top);
	return true;
}

size_t tp_lua_binding_count(const struct tp_lua *lua) {
	return lua ? lua->active_binds.len : 0;
}

void tp_lua_for_each_bind(const struct tp_lua *lua, tp_lua_bind_iter_fn fn,
						  void *userdata) {
	if (!lua || !fn)
		return;
	for (size_t i = 0; i < lua->active_binds.len; i++) {
		const struct tp_lua_binding *b = &lua->active_binds.items[i];
		struct tp_lua_bind_info info = {
			.mods = b->mods,
			.sym = b->sym,
			.allow_locked = b->allow_locked,
			.is_lua = b->is_lua,
			.action = b->action,
		};
		fn(&info, userdata);
	}
}

bool tp_lua_has_unmodified_bindings(const struct tp_lua *lua) {
	return lua ? lua->active_binds.has_unmodified : false;
}
