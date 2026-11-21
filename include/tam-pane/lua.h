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

#include "tam-pane/settings.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tp_lua;

enum {
	TP_LUA_HOOK_INTERVAL = 10000,
	TP_LUA_CONFIG_TIMEOUT_MS = 1500,
	TP_LUA_CONFIG_MAX_INSTRUCTIONS = 10000000,
	TP_LUA_READ_FILE_DEFAULT_CAP = 65536,
	TP_LUA_READ_FILE_MAX_CAP = 1048576,
};

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
 * On success, commits the resulting settings into *out and returns true.
 * If the file does not exist (ENOENT) and warn_if_missing is false, logs at
 * debug level and returns false without modifying *out. On any read, syntax,
 * runtime, or timeout error, logs an error and returns false without
 * modifying *out.
 */
bool tp_lua_load_file(struct tp_lua *lua, const char *path,
					  bool warn_if_missing, const struct tp_settings *base,
					  struct tp_settings *out);

/*
 * Execute a Lua source string under an explicit instruction budget and
 * monotonic wall-clock deadline. On success, commits staged settings into
 * *out (if out is non-NULL) and returns true. On failure, writes a diagnostic
 * into err_buf (if non-NULL) and leaves *out untouched.
 */
bool tp_lua_exec_string(struct tp_lua *lua, const char *code, size_t code_len,
						const char *chunk_name, uint64_t max_instructions,
						uint32_t timeout_ms, const struct tp_settings *base,
						struct tp_settings *out, char *err_buf,
						size_t err_buf_size);

/* Override the spawn handler used by tp.spawn (default: tp_spawn). */
void tp_lua_set_spawn_fn(struct tp_lua *lua, void (*fn)(const char *cmd));

#endif
