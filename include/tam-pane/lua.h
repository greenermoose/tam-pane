/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Sandboxed Lua 5.5 interpreter for tam-pane configuration and scripting.
 *
 * Pure module (no wlroots dependency), explicit state (no global variables),
 * safe standard libraries only (no io/os/package/debug), and every call
 * guarded by an instruction budget and a monotonic wall-clock deadline.
 */
#ifndef TAM_PANE_LUA_H
#define TAM_PANE_LUA_H

#include "tam-pane/bindings.h"
#include "tam-pane/events.h"
#include "tam-pane/rules.h"
#include "tam-pane/settings.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tp_lua;

enum {
	TP_LUA_HOOK_INTERVAL = 10000,
	TP_LUA_CONFIG_TIMEOUT_MS = 1500,
	TP_LUA_CONFIG_MAX_INSTRUCTIONS = 10000000,
	TP_LUA_CALLBACK_TIMEOUT_MS = 100,
	TP_LUA_CALLBACK_MAX_INSTRUCTIONS = 1000000,
	TP_LUA_READ_FILE_DEFAULT_CAP = 65536,
	TP_LUA_READ_FILE_MAX_CAP = 1048576,
	TP_LUA_SPAWN_ARGV_MAX = 64,
};

typedef void (*tp_lua_action_fn)(const struct tp_action *act, void *userdata);

struct tp_lua *tp_lua_create(void);
void tp_lua_destroy(struct tp_lua *lua);

/*
 * Resolve the default config path: $XDG_CONFIG_HOME/tam-pane/init.lua,
 * falling back to $HOME/.config/tam-pane/init.lua. Returns false if neither
 * environment variable is set or the path does not fit in out.
 */
bool tp_lua_default_config_path(char *out, size_t out_size);

/*
 * Load and execute a Lua configuration file under the watchdog.
 * On success, commits the resulting settings and bindings into *out / lua
 * and returns true. If the file does not exist (ENOENT) and warn_if_missing
 * is false, logs at debug level and returns false without modifying *out or
 * active bindings. On any read, syntax, runtime, or timeout error, logs an
 * error and returns false with atomic rollback.
 */
bool tp_lua_load_file(struct tp_lua *lua, const char *path,
					  bool warn_if_missing, const struct tp_settings *base,
					  struct tp_settings *out);

/*
 * Load path into a brand-new sandboxed Lua state starting from fresh defaults
 * (settings, default bindings, empty hooks and rules). On success, transfers
 * the existing spawn handlers and action_fn onto the new state, destroys *lua_slot,
 * installs the new state, writes *out, and returns true. On failure, destroys
 * only the candidate state, leaves *lua_slot and *out untouched, writes a
 * diagnostic into err_buf (if non-NULL), and returns false.
 */
bool tp_lua_reload_file(struct tp_lua **lua_slot, const char *path,
						struct tp_settings *out, char *err_buf,
						size_t err_buf_size);

/*
 * Execute a Lua source string under an explicit instruction budget and
 * monotonic wall-clock deadline. On success, commits staged settings (into
 * *out if non-NULL) and staged bindings, returning true. On failure, writes a
 * diagnostic into err_buf (if non-NULL) and leaves *out and active bindings
 * untouched.
 */
bool tp_lua_exec_string(struct tp_lua *lua, const char *code, size_t code_len,
						const char *chunk_name, uint64_t max_instructions,
						uint32_t timeout_ms, const struct tp_settings *base,
						struct tp_settings *out, char *err_buf,
						size_t err_buf_size);

/* Override the spawn handler used by tp.spawn (default: tp_spawn). */
void tp_lua_set_spawn_fn(struct tp_lua *lua, void (*fn)(const char *cmd));

/* Override the handler used by tp.spawn{...} (default: tp_spawn_argv). */
void tp_lua_set_spawn_argv_fn(struct tp_lua *lua,
							  void (*fn)(const char *const argv[]));

/* Attach the compositor action handler used by tp.dsp.* dispatchers. */
void tp_lua_set_action_fn(struct tp_lua *lua, tp_lua_action_fn fn,
						  void *userdata);

/*
 * Match and execute the active binding for (mods, sym) using TP_MOD_* bits.
 * Dispatcher bindings invoke action_fn directly; Lua callbacks run under the
 * runtime watchdog. Returns true when a binding matched.
 */
bool tp_lua_handle_key(struct tp_lua *lua, uint32_t mods, xkb_keysym_t sym);

/* Number of currently active key bindings. */
size_t tp_lua_binding_count(const struct tp_lua *lua);

/* True when at least one active binding has mods == 0. */
bool tp_lua_has_unmodified_bindings(const struct tp_lua *lua);

/* True when at least one active hook is registered for kind. */
bool tp_lua_has_hooks(const struct tp_lua *lua, enum tp_event_kind kind);

/* Number of active hooks for kind (or total hooks when kind == TP_EVENT_COUNT). */
size_t tp_lua_hook_count(const struct tp_lua *lua, enum tp_event_kind kind);

/*
 * Invoke every active Lua hook registered for ev->kind in registration order,
 * each with a fresh event table and guarded by the runtime watchdog. Errors or
 * timeouts are logged without stopping remaining hooks. Returns hooks invoked.
 */
size_t tp_lua_emit_event(struct tp_lua *lua, const struct tp_event *ev);

/* Number of currently active window rules. */
size_t tp_lua_rule_count(const struct tp_lua *lua);

/*
 * Evaluate active window rules in registration order against (app_id, title)
 * using Lua patterns under the runtime watchdog. Matching rules merge into
 * *out with later rules taking precedence. Returns true if any rule matched.
 */
bool tp_lua_eval_rules(struct tp_lua *lua, const char *app_id,
					   const char *title, struct tp_rule_result *out);

#endif
