/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Private interface shared between src/lua.c and src/lua_bindings.c. */
#ifndef TAM_PANE_LUA_INTERNAL_H
#define TAM_PANE_LUA_INTERNAL_H

#include "tam-pane/lua.h"
#include <lauxlib.h>
#include <lua.h>

struct tp_lua_binding {
	uint32_t mods;
	xkb_keysym_t sym;
	bool is_lua;
	struct tp_action action;
	int lua_ref; /* LUA_NOREF when !is_lua */
};

struct tp_lua_bind_list {
	struct tp_lua_binding *items;
	size_t len;
	size_t cap;
	bool has_unmodified;
};

struct tp_lua {
	lua_State *L;

	bool watchdog_active;
	bool watchdog_aborted;
	uint64_t deadline_ns;
	uint64_t max_instructions;
	uint64_t instructions_used;
	const char *context;
	const char *abort_reason;

	struct tp_settings *staging;
	void (*spawn_fn)(const char *cmd);

	tp_lua_action_fn action_fn;
	void *action_userdata;

	struct tp_lua_bind_list active_binds;
	struct tp_lua_bind_list staged_binds;
	bool staging_binds;
};

struct tp_lua *tp_lua_from_state(lua_State *L);
int tp_lua_guarded_pcall(struct tp_lua *lua, int nargs, int nresults,
						 uint64_t max_instructions, uint32_t timeout_ms,
						 const char *context);

/* Register tp.bind, tp.unbind, tp.clear_binds, and tp.dsp on table at -1. */
void tp_lua_bindings_open(lua_State *L);

/* Execute the embedded Lua default bindings into active_binds. */
bool tp_lua_bindings_load_defaults(struct tp_lua *lua);

/* Atomic staging for bindings during config load. */
bool tp_lua_bindings_stage_begin(struct tp_lua *lua);
void tp_lua_bindings_stage_commit(struct tp_lua *lua);
void tp_lua_bindings_stage_abort(struct tp_lua *lua);

/* Release all binding lists and Lua registry references before lua_close. */
void tp_lua_bindings_finish(struct tp_lua *lua);

#endif
