/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Private interface shared between src/lua.c, src/lua_bindings.c,
 * src/lua_events.c, and src/lua_rules.c. */
#ifndef TAM_PANE_LUA_INTERNAL_H
#define TAM_PANE_LUA_INTERNAL_H

#include "tam-pane/lua.h"
#include <lauxlib.h>
#include <lua.h>

struct tp_lua_binding {
	uint32_t mods;
	xkb_keysym_t sym;
	bool is_lua;
	bool allow_locked;
	struct tp_action action;
	int lua_ref; /* LUA_NOREF when !is_lua */
	char desc[128];
};

struct tp_lua_bind_list {
	struct tp_lua_binding *items;
	size_t len;
	size_t cap;
	bool has_unmodified;
};

struct tp_lua_hook {
	uint64_t id;
	enum tp_event_kind kind;
	int lua_ref;
};

struct tp_lua_hook_list {
	struct tp_lua_hook *items;
	size_t len;
	size_t cap;
	size_t counts[TP_EVENT_COUNT];
};

struct tp_lua_rule {
	uint64_t id;
	bool has_app_id;
	char app_id[TP_RULE_PATTERN_MAX];
	bool has_title;
	char title[TP_RULE_PATTERN_MAX];

	bool has_tags;
	tp_tags_t tags;
	bool has_floating;
	bool floating;
	bool has_size;
	int32_t width;
	int32_t height;
	enum tp_rule_pos_mode pos_mode;
	int32_t x;
	int32_t y;
	bool has_monitor;
	char monitor[TP_RULE_MONITOR_MAX];
	bool has_scratchpad;
	bool scratchpad;
};

struct tp_lua_rule_list {
	struct tp_lua_rule *items;
	size_t len;
	size_t cap;
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
	size_t mem_used;   /* bytes the Lua state holds (tp_lua_alloc) */
	bool mem_refused;  /* tp_lua_alloc refused a growth in this call */

	struct tp_settings *staging;
	void (*spawn_fn)(const char *cmd);
	void (*spawn_argv_fn)(const char *const argv[]);
	tp_lua_lock_fn lock_fn;
	void *lock_userdata;
	tp_lua_lock_argv_fn lock_argv_fn;
	void *lock_argv_userdata;
	tp_lua_capture_fn capture_fn;
	void *capture_userdata;
	tp_lua_capture_argv_fn capture_argv_fn;
	void *capture_argv_userdata;

	tp_lua_action_fn action_fn;
	void *action_userdata;

	struct tp_lua_bind_list active_binds;
	struct tp_lua_bind_list staged_binds;
	bool staging_binds;

	struct tp_lua_hook_list active_hooks;
	struct tp_lua_hook_list staged_hooks;
	uint64_t next_hook_id;
	bool staging_hooks;

	struct tp_lua_rule_list active_rules;
	struct tp_lua_rule_list staged_rules;
	uint64_t next_rule_id;
	bool staging_rules;
	int string_find_ref;
};

struct tp_lua *tp_lua_from_state(lua_State *L);

/* The integer at idx, in [lo, hi], or a Lua error naming ctx: "<ctx>:
 * expected integer" or "<ctx>: <n> out of range [lo, hi]". */
int32_t tp_lua_check_int(lua_State *L, int idx, const char *ctx, int32_t lo,
						 int32_t hi);
/* Deadline check for the patched pattern matcher (LUAI_PATTERNCHECK). */
void tp_lua_pattern_check(lua_State *L);
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

/* Register tp.on, tp.off, and tp.clear_hooks on table at -1. */
void tp_lua_events_open(lua_State *L);

/* Atomic staging for event hooks during config load. */
void tp_lua_hooks_stage_begin(struct tp_lua *lua);
void tp_lua_hooks_stage_commit(struct tp_lua *lua);
void tp_lua_hooks_stage_abort(struct tp_lua *lua);

/* Release all hook lists and Lua registry references before lua_close. */
void tp_lua_hooks_finish(struct tp_lua *lua);

/* Register tp.rule, tp.remove_rule, and tp.clear_rules on table at -1. */
void tp_lua_rules_open(lua_State *L);

/* Atomic staging for window rules during config load. */
void tp_lua_rules_stage_begin(struct tp_lua *lua);
void tp_lua_rules_stage_commit(struct tp_lua *lua);
void tp_lua_rules_stage_abort(struct tp_lua *lua);

/* Release all rule lists and Lua registry references before lua_close. */
void tp_lua_rules_finish(struct tp_lua *lua);

#endif
