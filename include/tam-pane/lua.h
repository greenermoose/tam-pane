/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Sandboxed Lua 5.5 interpreter for tam-pane configuration and scripting.
 *
 * Pure module (no wlroots dependency), explicit state (no global variables),
 * safe standard libraries only (no io/os/package/debug), and every call
 * guarded by an instruction budget and a monotonic wall-clock deadline
 * (which also bounds pattern matching), with Lua memory capped at
 * TP_LUA_MEMORY_MAX.
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
	TP_LUA_MEMORY_MAX = 64 << 20, /* bytes, enforced inside guarded calls */
	TP_LUA_RULE_SUBJECT_MAX = 255, /* bytes of app_id/title rules match */
};

struct tp_lua_window_info {
	char app_id[256];
	char title[256];
	bool floating;
	bool fullscreen;
};

typedef void (*tp_lua_action_fn)(const struct tp_action *act, void *userdata);
typedef void (*tp_lua_lock_fn)(const char *cmd, void *userdata);
typedef void (*tp_lua_lock_argv_fn)(const char *const argv[], void *userdata);
typedef void (*tp_lua_capture_fn)(const char *cmd, void *userdata);
typedef void (*tp_lua_capture_argv_fn)(const char *const argv[], void *userdata);
typedef bool (*tp_lua_focused_fn)(struct tp_lua_window_info *out,
								  void *userdata);

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

/* Attach the locker launch handlers used by tp.lock(cmd) and tp.lock{...}. */
void tp_lua_set_lock_fn(struct tp_lua *lua, tp_lua_lock_fn fn, void *userdata);
void tp_lua_set_lock_argv_fn(struct tp_lua *lua, tp_lua_lock_argv_fn fn,
							 void *userdata);

/* Attach the capture launch handlers used by tp.capture(cmd) and tp.capture{...}. */
void tp_lua_set_capture_fn(struct tp_lua *lua, tp_lua_capture_fn fn,
						   void *userdata);
void tp_lua_set_capture_argv_fn(struct tp_lua *lua, tp_lua_capture_argv_fn fn,
								void *userdata);

/* Attach the focused-window query handler used by tp.focused_window(). */
void tp_lua_set_focused_fn(struct tp_lua *lua, tp_lua_focused_fn fn,
						   void *userdata);

/*
 * Match and execute the active binding for (mods, sym) using TP_MOD_* bits.
 * When `locked` is true, only bindings registered with `{ locked = true }`
 * match (R1 finding 17 binding gate). Dispatcher bindings invoke action_fn
 * directly; Lua callbacks run under the runtime watchdog. Returns true when
 * a binding matched.
 */
bool tp_lua_handle_key(struct tp_lua *lua, uint32_t mods, xkb_keysym_t sym,
					   bool locked);
bool tp_lua_handle_key_ex(struct tp_lua *lua, uint32_t mods, xkb_keysym_t sym,
						  uint32_t keycode, bool released, bool locked);
/* Read-only match using the same precedence as tp_lua_handle_key_ex.
 * Returns SIZE_MAX when no eligible binding matches. */
size_t tp_lua_find_key_binding(const struct tp_lua *lua, uint32_t mods,
							   xkb_keysym_t sym, uint32_t keycode,
							   bool released, bool locked);
bool tp_lua_handle_mouse(struct tp_lua *lua, uint32_t mods, uint32_t button,
						 bool locked, struct tp_action *out_grab);

/* Monitor configuration rule staged by tp.monitor{...}. */
struct tp_monitor_config {
	char output[64]; /* "" = wildcard fallback */
	bool has_mode;
	int32_t mode_w;
	int32_t mode_h;
	int32_t mode_refresh_mhz; /* 0 = best refresh for WxH */
	bool mode_preferred;
	bool has_pos;
	bool pos_auto;
	int32_t pos_x;
	int32_t pos_y;
	bool has_scale;
	float scale;
	bool has_transform;
	int32_t transform; /* 0..7 */
	bool has_enabled;
	bool enabled;
};

/* Resolve the effective monitor rule for output_name (exact match, else ""). */
bool tp_lua_monitor_rule(const struct tp_lua *lua, const char *output_name,
						 struct tp_monitor_config *out);
size_t tp_lua_monitor_count(const struct tp_lua *lua);

/* Number of currently active key bindings. */
size_t tp_lua_binding_count(const struct tp_lua *lua);

/* Read-only snapshot of one active key binding (for IPC `binds`). */
struct tp_lua_bind_info {
	uint32_t mods;
	xkb_keysym_t sym;
	uint32_t keycode;
	bool on_release;
	bool allow_locked;
	bool is_lua;
	struct tp_action action; /* valid when !is_lua */
	char desc[128];			 /* optional human-readable description */
};

typedef void (*tp_lua_bind_iter_fn)(const struct tp_lua_bind_info *info,
									void *userdata);

/* Invoke fn once per active key binding in registration order. */
void tp_lua_for_each_bind(const struct tp_lua *lua, tp_lua_bind_iter_fn fn,
						  void *userdata);

/* Execute the active binding at registration index idx (for chord menu). */
bool tp_lua_run_bind_index(struct tp_lua *lua, size_t idx, bool locked);

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
