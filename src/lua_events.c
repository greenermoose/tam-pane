/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Lua event hook registry and dispatcher (tp.on, tp.off, tp.clear_hooks).
 *
 * Pure module (no wlroots, no Wayland). Hooks register during config load,
 * stage and roll back atomically alongside tp.config and tp.bind, and execute
 * under the runtime watchdog (1,000,000 instructions / 100 ms deadline).
 */
#include "lua_internal.h"
#include <stdlib.h>
#include <string.h>
#include <tam.h>

static const char *const event_names[TP_EVENT_COUNT] = {
	[TP_EVENT_WINDOW_OPEN] = "window.open",
	[TP_EVENT_WINDOW_CLOSE] = "window.close",
	[TP_EVENT_WINDOW_FOCUS] = "window.focus",
	[TP_EVENT_TAG_CHANGE] = "tag.change",
	[TP_EVENT_MONITOR_ADD] = "monitor.add",
	[TP_EVENT_MONITOR_REMOVE] = "monitor.remove",
	[TP_EVENT_STARTUP] = "startup",
};

const char *tp_event_kind_name(enum tp_event_kind kind) {
	if ((unsigned int)kind >= TP_EVENT_COUNT)
		return NULL;
	return event_names[kind];
}

bool tp_event_kind_from_name(const char *name, enum tp_event_kind *out) {
	if (!name || !out)
		return false;
	for (unsigned int i = 0; i < TP_EVENT_COUNT; i++) {
		if (strcmp(name, event_names[i]) == 0) {
			*out = (enum tp_event_kind)i;
			return true;
		}
	}
	return false;
}

static unsigned int first_tag_num(tp_tags_t t) {
	t &= TP_TAG_ALL;
	for (unsigned int i = 1; i <= TP_TAG_COUNT; i++) {
		if (t & (1u << (i - 1u)))
			return i;
	}
	return 0;
}

/* ---- Hook list helpers ---- */

static void hook_list_clear(lua_State *L, struct tp_lua_hook_list *list) {
	if (L && list->items) {
		for (size_t i = 0; i < list->len; i++) {
			if (list->items[i].lua_ref != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, list->items[i].lua_ref);
		}
	}
	free(list->items);
	list->items = NULL;
	list->len = 0;
	list->cap = 0;
	memset(list->counts, 0, sizeof(list->counts));
}

static bool hook_list_append(struct tp_lua_hook_list *list,
							 const struct tp_lua_hook *entry) {
	if (list->len == list->cap) {
		size_t next_cap = list->cap ? list->cap * 2 : 16;
		struct tp_lua_hook *grown =
			realloc(list->items, next_cap * sizeof(*grown));
		if (!grown)
			return false;
		list->items = grown;
		list->cap = next_cap;
	}
	list->items[list->len++] = *entry;
	list->counts[entry->kind]++;
	return true;
}

static bool hook_list_remove_id(lua_State *L, struct tp_lua_hook_list *list,
								uint64_t id) {
	for (size_t i = 0; i < list->len; i++) {
		if (list->items[i].id == id) {
			enum tp_event_kind kind = list->items[i].kind;
			if (L && list->items[i].lua_ref != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, list->items[i].lua_ref);
			if (i + 1 < list->len)
				memmove(&list->items[i], &list->items[i + 1],
						(list->len - i - 1) * sizeof(*list->items));
			list->len--;
			if (list->counts[kind] > 0)
				list->counts[kind]--;
			return true;
		}
	}
	return false;
}

static void hook_list_remove_kind(lua_State *L, struct tp_lua_hook_list *list,
								  enum tp_event_kind kind) {
	size_t dst = 0;
	for (size_t i = 0; i < list->len; i++) {
		if (list->items[i].kind == kind) {
			if (L && list->items[i].lua_ref != LUA_NOREF)
				luaL_unref(L, LUA_REGISTRYINDEX, list->items[i].lua_ref);
		} else {
			if (dst != i)
				list->items[dst] = list->items[i];
			dst++;
		}
	}
	list->len = dst;
	list->counts[kind] = 0;
}

/* ---- Lua payload construction ---- */

static void push_window_fields(lua_State *L, const struct tp_event_window *w) {
	lua_pushstring(L, w->app_id);
	lua_setfield(L, -2, "app_id");
	lua_pushstring(L, w->title);
	lua_setfield(L, -2, "title");
	lua_pushstring(L, w->monitor);
	lua_setfield(L, -2, "monitor");
	lua_pushinteger(L, (lua_Integer)w->tags);
	lua_setfield(L, -2, "tags");
	lua_pushinteger(L, (lua_Integer)first_tag_num(w->tags));
	lua_setfield(L, -2, "tag");
	lua_pushboolean(L, w->floating);
	lua_setfield(L, -2, "floating");
	lua_pushboolean(L, w->fullscreen);
	lua_setfield(L, -2, "fullscreen");
	lua_pushinteger(L, w->geom.x);
	lua_setfield(L, -2, "x");
	lua_pushinteger(L, w->geom.y);
	lua_setfield(L, -2, "y");
	lua_pushinteger(L, w->geom.w);
	lua_setfield(L, -2, "w");
	lua_pushinteger(L, w->geom.h);
	lua_setfield(L, -2, "h");
}

static void push_event_table(lua_State *L, const struct tp_event *ev) {
	const char *name = tp_event_kind_name(ev->kind);
	lua_createtable(L, 0, 12);
	lua_pushstring(L, name ? name : "");
	lua_setfield(L, -2, "event");

	switch (ev->kind) {
	case TP_EVENT_WINDOW_OPEN:
	case TP_EVENT_WINDOW_CLOSE:
		push_window_fields(L, &ev->window);
		break;
	case TP_EVENT_WINDOW_FOCUS:
		lua_pushboolean(L, ev->window.focused);
		lua_setfield(L, -2, "focused");
		push_window_fields(L, &ev->window);
		break;
	case TP_EVENT_TAG_CHANGE:
		lua_pushstring(L, ev->tag.monitor);
		lua_setfield(L, -2, "monitor");
		lua_pushinteger(L, (lua_Integer)ev->tag.tags);
		lua_setfield(L, -2, "tags");
		lua_pushinteger(L, (lua_Integer)ev->tag.prev_tags);
		lua_setfield(L, -2, "prev_tags");
		lua_pushinteger(L, (lua_Integer)first_tag_num(ev->tag.tags));
		lua_setfield(L, -2, "tag");
		break;
	case TP_EVENT_MONITOR_ADD:
	case TP_EVENT_MONITOR_REMOVE:
		lua_pushstring(L, ev->monitor.name);
		lua_setfield(L, -2, "name");
		lua_pushinteger(L, ev->monitor.area.x);
		lua_setfield(L, -2, "x");
		lua_pushinteger(L, ev->monitor.area.y);
		lua_setfield(L, -2, "y");
		lua_pushinteger(L, ev->monitor.area.w);
		lua_setfield(L, -2, "w");
		lua_pushinteger(L, ev->monitor.area.h);
		lua_setfield(L, -2, "h");
		lua_pushnumber(L, (lua_Number)ev->monitor.scale);
		lua_setfield(L, -2, "scale");
		break;
	case TP_EVENT_STARTUP:
	case TP_EVENT_COUNT:
		break;
	}
}

/* ---- tp.on / tp.off / tp.clear_hooks ---- */

static int l_tp_on(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_hooks)
		return luaL_error(L, "tp.on can only be called while loading config");

	const char *name = luaL_checkstring(L, 1);
	enum tp_event_kind kind;
	if (!tp_event_kind_from_name(name, &kind))
		return luaL_error(L, "tp.on: unknown event '%s'", name);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);

	struct tp_lua_hook entry = {
		.id = ++lua->next_hook_id,
		.kind = kind,
		.lua_ref = ref,
	};
	if (!hook_list_append(&lua->staged_hooks, &entry)) {
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		return luaL_error(L, "tp.on: out of memory");
	}
	lua_pushinteger(L, (lua_Integer)entry.id);
	return 1;
}

static int l_tp_off(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_hooks)
		return luaL_error(L, "tp.off can only be called while loading config");

	if (lua_type(L, 1) != LUA_TNUMBER || !lua_isinteger(L, 1))
		return luaL_error(L, "tp.off: expected integer hook id");
	lua_Integer id = lua_tointeger(L, 1);
	bool removed =
		id > 0 && hook_list_remove_id(L, &lua->staged_hooks, (uint64_t)id);
	lua_pushboolean(L, removed);
	return 1;
}

static int l_tp_clear_hooks(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_hooks)
		return luaL_error(
			L, "tp.clear_hooks can only be called while loading config");

	if (lua_isnoneornil(L, 1)) {
		hook_list_clear(L, &lua->staged_hooks);
		return 0;
	}
	const char *name = luaL_checkstring(L, 1);
	enum tp_event_kind kind;
	if (!tp_event_kind_from_name(name, &kind))
		return luaL_error(L, "tp.clear_hooks: unknown event '%s'", name);
	hook_list_remove_kind(L, &lua->staged_hooks, kind);
	return 0;
}

void tp_lua_events_open(lua_State *L) {
	lua_pushcfunction(L, l_tp_on);
	lua_setfield(L, -2, "on");
	lua_pushcfunction(L, l_tp_off);
	lua_setfield(L, -2, "off");
	lua_pushcfunction(L, l_tp_clear_hooks);
	lua_setfield(L, -2, "clear_hooks");
}

void tp_lua_hooks_stage_begin(struct tp_lua *lua) {
	hook_list_clear(lua->L, &lua->staged_hooks);
	lua->staging_hooks = true;
}

void tp_lua_hooks_stage_commit(struct tp_lua *lua) {
	lua->staging_hooks = false;
	hook_list_clear(lua->L, &lua->active_hooks);
	lua->active_hooks = lua->staged_hooks;
	lua->staged_hooks = (struct tp_lua_hook_list){0};
}

void tp_lua_hooks_stage_abort(struct tp_lua *lua) {
	lua->staging_hooks = false;
	hook_list_clear(lua->L, &lua->staged_hooks);
}

void tp_lua_hooks_finish(struct tp_lua *lua) {
	if (!lua)
		return;
	hook_list_clear(lua->L, &lua->staged_hooks);
	hook_list_clear(lua->L, &lua->active_hooks);
	lua->staging_hooks = false;
}

bool tp_lua_has_hooks(const struct tp_lua *lua, enum tp_event_kind kind) {
	if (!lua || (unsigned int)kind >= TP_EVENT_COUNT)
		return false;
	return lua->active_hooks.counts[kind] > 0;
}

size_t tp_lua_hook_count(const struct tp_lua *lua, enum tp_event_kind kind) {
	if (!lua)
		return 0;
	if (kind == TP_EVENT_COUNT)
		return lua->active_hooks.len;
	if ((unsigned int)kind > TP_EVENT_COUNT)
		return 0;
	return lua->active_hooks.counts[kind];
}

size_t tp_lua_emit_event(struct tp_lua *lua, const struct tp_event *ev) {
	if (!lua || !lua->L || !ev || (unsigned int)ev->kind >= TP_EVENT_COUNT)
		return 0;
	if (lua->active_hooks.counts[ev->kind] == 0)
		return 0;

	lua_State *L = lua->L;
	const char *name = tp_event_kind_name(ev->kind);
	size_t ran = 0;

	for (size_t i = 0; i < lua->active_hooks.len; i++) {
		if (lua->active_hooks.items[i].kind != ev->kind)
			continue;
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, lua->active_hooks.items[i].lua_ref);
		push_event_table(L, ev);
		int rc = tp_lua_guarded_pcall(lua, 1, 0,
									  TP_LUA_CALLBACK_MAX_INSTRUCTIONS,
									  TP_LUA_CALLBACK_TIMEOUT_MS, name);
		if (rc != LUA_OK) {
			const char *msg = lua_tostring(L, -1);
			tam_log_error("lua hook %s failed: %s", name ? name : "event",
						  msg ? msg : "unknown error");
		}
		lua_settop(L, top);
		ran++;
	}
	return ran;
}
