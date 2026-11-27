/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lua_internal.h"
#include "tam-pane/layout/layout.h"
#include "tam-pane/spawn.h"
#include <errno.h>
#include <fcntl.h>
#include <lualib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <tam.h>
#include <time.h>
#include <unistd.h>

/* Unique address used as a lightuserdata key in LUA_REGISTRYINDEX. */
static char tp_lua_registry_key;

static uint64_t now_monotonic_ns(void) {
	return (uint64_t)tam_monotonic_ns();
}

struct tp_lua *tp_lua_from_state(lua_State *L) {
	lua_pushlightuserdata(L, &tp_lua_registry_key);
	lua_rawget(L, LUA_REGISTRYINDEX);
	struct tp_lua *lua = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return lua;
}

static void watchdog_hook(lua_State *L, lua_Debug *ar);

/* Stop the running call: raise the error, and switch to a 1-instruction
 * hook so a nested pcall/xpcall cannot trap it. */
static void watchdog_abort(lua_State *L, struct tp_lua *lua,
						   const char *reason) {
	if (reason) {
		lua->watchdog_aborted = true;
		lua->abort_reason = reason;
	}
	lua_sethook(L, watchdog_hook, LUA_MASKCOUNT, 1);
	luaL_error(L, "%s in %s",
			   lua->abort_reason ? lua->abort_reason : "execution aborted",
			   lua->context ? lua->context : "lua");
}

static void watchdog_check_deadline(lua_State *L, struct tp_lua *lua) {
	if (lua->watchdog_aborted)
		watchdog_abort(L, lua, NULL);
	if (lua->deadline_ns > 0 && now_monotonic_ns() >= lua->deadline_ns)
		watchdog_abort(L, lua, "execution timed out");
}

static void watchdog_hook(lua_State *L, lua_Debug *ar) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->watchdog_active)
		return;
	if (lua->watchdog_aborted)
		watchdog_abort(L, lua, NULL);

	lua->instructions_used += TP_LUA_HOOK_INTERVAL;
	if (lua->max_instructions > 0 &&
		lua->instructions_used > lua->max_instructions)
		watchdog_abort(L, lua, "execution exceeded instruction budget");
	watchdog_check_deadline(L, lua);
}

/*
 * Called by the patched Lua pattern matcher (third_party/lua/lstrlib.c,
 * LUAI_PATTERNCHECK) every 4096 steps. Matching runs in C, where the count
 * hook never fires, so this applies the same wall-clock deadline to it.
 */
void tp_lua_pattern_check(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (lua && lua->watchdog_active)
		watchdog_check_deadline(L, lua);
}

/*
 * Lua's allocator, counting what the state holds. Inside a guarded call a
 * growth past TP_LUA_MEMORY_MAX is refused, which Lua turns into a "not
 * enough memory" error that the call's pcall catches. Outside guarded
 * calls nothing is refused, because the host's own small pushes there are
 * unprotected, and a memory error would make Lua panic and abort.
 */
static void *tp_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	struct tp_lua *lua = ud;
	size_t old = ptr ? osize : 0;
	if (old > lua->mem_used)
		old = lua->mem_used; /* never wrap, even if counts ever disagree */
	if (nsize == 0) {
		free(ptr);
		lua->mem_used -= old;
		return NULL;
	}
	if (nsize > old && lua->watchdog_active &&
		lua->mem_used - old + nsize > (size_t)TP_LUA_MEMORY_MAX) {
		lua->mem_refused = true;
		return NULL;
	}
	void *p = realloc(ptr, nsize);
	if (p)
		lua->mem_used = lua->mem_used - old + nsize;
	return p;
}

int tp_lua_guarded_pcall(struct tp_lua *lua, int nargs, int nresults,
						 uint64_t max_instructions, uint32_t timeout_ms,
						 const char *context) {
	lua_State *L = lua->L;

	lua_Hook prev_hook = lua_gethook(L);
	int prev_mask = lua_gethookmask(L);
	int prev_count = lua_gethookcount(L);
	bool prev_active = lua->watchdog_active;
	bool prev_aborted = lua->watchdog_aborted;
	uint64_t prev_deadline = lua->deadline_ns;
	uint64_t prev_max_instr = lua->max_instructions;
	uint64_t prev_used = lua->instructions_used;
	const char *prev_ctx = lua->context;
	const char *prev_reason = lua->abort_reason;

	lua->watchdog_active = (max_instructions > 0 || timeout_ms > 0);
	lua->watchdog_aborted = false;
	lua->abort_reason = NULL;
	lua->max_instructions = max_instructions;
	lua->instructions_used = 0;
	lua->deadline_ns =
		timeout_ms > 0
			? now_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL
			: 0;
	lua->context = context;

	if (lua->watchdog_active)
		lua_sethook(L, watchdog_hook, LUA_MASKCOUNT, TP_LUA_HOOK_INTERVAL);
	else
		lua_sethook(L, NULL, 0, 0);

	lua->mem_refused = false;
	int rc = lua_pcall(L, nargs, nresults, 0);
	if (rc == LUA_ERRMEM && lua->mem_refused)
		tam_log_warn("lua: %s reached the %d MiB memory limit",
					 context ? context : "lua", TP_LUA_MEMORY_MAX >> 20);

	lua_sethook(L, prev_hook, prev_mask, prev_count);
	lua->watchdog_active = prev_active;
	lua->watchdog_aborted = prev_aborted;
	lua->deadline_ns = prev_deadline;
	lua->max_instructions = prev_max_instr;
	lua->instructions_used = prev_used;
	lua->context = prev_ctx;
	lua->abort_reason = prev_reason;

	return rc;
}

/*
 * Non-blocking, bounded regular-file reader. Opens with O_NONBLOCK and
 * verifies S_ISREG so FIFOs, sockets, and device nodes can never stall the
 * compositor event loop. If reject_oversize is true and the file exceeds
 * max_bytes, fails with EFBIG instead of truncating.
 */
static bool read_regular_file(const char *path, size_t max_bytes,
							  bool reject_oversize, char **out_buf,
							  size_t *out_len, int *out_errno) {
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		*out_errno = errno;
		return false;
	}

	struct stat st;
	if (fstat(fd, &st) != 0) {
		*out_errno = errno;
		close(fd);
		return false;
	}
	if (!S_ISREG(st.st_mode)) {
		*out_errno = EINVAL;
		close(fd);
		return false;
	}
	if (reject_oversize && st.st_size > 0 && (uint64_t)st.st_size > max_bytes) {
		*out_errno = EFBIG;
		close(fd);
		return false;
	}

	size_t cap = st.st_size > 0 ? (size_t)st.st_size : 256;
	if (cap > max_bytes)
		cap = max_bytes;
	char *buf = malloc(cap + 1);
	if (!buf) {
		*out_errno = ENOMEM;
		close(fd);
		return false;
	}

	size_t total = 0;
	for (;;) {
		if (total == cap) {
			if (cap == max_bytes) {
				if (reject_oversize) {
					char extra;
					ssize_t probe = read(fd, &extra, 1);
					if (probe > 0) {
						free(buf);
						close(fd);
						*out_errno = EFBIG;
						return false;
					}
				}
				break;
			}
			size_t next_cap = cap * 2;
			if (next_cap > max_bytes)
				next_cap = max_bytes;
			char *grown = realloc(buf, next_cap + 1);
			if (!grown) {
				free(buf);
				close(fd);
				*out_errno = ENOMEM;
				return false;
			}
			buf = grown;
			cap = next_cap;
		}
		ssize_t n = read(fd, buf + total, cap - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			*out_errno = errno;
			free(buf);
			close(fd);
			return false;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}

	close(fd);
	buf[total] = '\0';
	*out_buf = buf;
	*out_len = total;
	*out_errno = 0;
	return true;
}

static int l_print(lua_State *L) {
	int n = lua_gettop(L);
	char buf[1024];
	size_t pos = 0;

	for (int i = 1; i <= n; i++) {
		size_t len = 0;
		const char *s = luaL_tolstring(L, i, &len);
		if (i > 1 && pos + 1 < sizeof(buf))
			buf[pos++] = '\t';
		size_t avail = sizeof(buf) - 1 - pos;
		size_t copy = len < avail ? len : avail;
		if (copy > 0) {
			memcpy(buf + pos, s, copy);
			pos += copy;
		}
		lua_pop(L, 1); /* tolstring result */
	}
	buf[pos] = '\0';
	tam_log_info("lua: %s", buf);
	return 0;
}

int32_t tp_lua_check_int(lua_State *L, int idx, const char *ctx, int32_t lo,
						 int32_t hi) {
	if (lua_type(L, idx) != LUA_TNUMBER || !lua_isinteger(L, idx))
		luaL_error(L, "%s: expected integer", ctx);
	lua_Integer v = lua_tointeger(L, idx);
	/* lua_pushfstring knows %I (lua_Integer), not %lld. */
	if (v < lo || v > hi)
		luaL_error(L, "%s: %I out of range [%d, %d]", ctx, v, (int)lo,
				   (int)hi);
	return (int32_t)v;
}

static int32_t check_int_range(lua_State *L, int idx, const char *key,
							   int32_t lo, int32_t hi) {
	char ctx[64];
	snprintf(ctx, sizeof(ctx), "tp.config.%s", key);
	return tp_lua_check_int(L, idx, ctx, lo, hi);
}

static float check_float_range(lua_State *L, int idx, const char *key,
							   float lo, float hi) {
	if (lua_type(L, idx) != LUA_TNUMBER)
		luaL_error(L, "tp.config.%s: expected number", key);
	lua_Number v = lua_tonumber(L, idx);
	if (isnan(v) || v < (lua_Number)lo || v > (lua_Number)hi) {
		/* lua_pushfstring has no precision; format the bounds here. */
		char range[64];
		snprintf(range, sizeof(range), "[%.2f, %.2f]", (double)lo, (double)hi);
		luaL_error(L, "tp.config.%s: value out of range %s", key, range);
	}
	return (float)v;
}

static bool check_bool(lua_State *L, int idx, const char *key) {
	if (lua_type(L, idx) != LUA_TBOOLEAN)
		luaL_error(L, "tp.config.%s: expected boolean", key);
	return lua_toboolean(L, idx) != 0;
}

static void check_string_buf(lua_State *L, int idx, const char *key,
							 bool allow_empty, char *dst, size_t dst_size) {
	if (lua_type(L, idx) != LUA_TSTRING)
		luaL_error(L, "tp.config.%s: expected string", key);
	size_t len = 0;
	const char *s = lua_tolstring(L, idx, &len);
	if (!allow_empty && len == 0)
		luaL_error(L, "tp.config.%s: must not be empty", key);
	if (len >= dst_size)
		luaL_error(L, "tp.config.%s: string too long (max %d bytes)", key,
				   (int)(dst_size - 1));
	memcpy(dst, s, len + 1);
}

static int l_tp_config(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging)
		return luaL_error(L, "tp.config can only be called while loading config");

	luaL_checktype(L, 1, LUA_TTABLE);
	struct tp_settings *s = lua->staging;

	/* Apply shorthand 'gaps' first so gap_inner / gap_outer can override it. */
	lua_getfield(L, 1, "gaps");
	if (!lua_isnil(L, -1)) {
		int32_t g = check_int_range(L, -1, "gaps", 0, 256);
		s->gap_inner = g;
		s->gap_outer = g;
	}
	lua_pop(L, 1);

	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		if (lua_type(L, -2) != LUA_TSTRING)
			return luaL_error(L, "tp.config: keys must be strings");
		const char *key = lua_tostring(L, -2);

		if (strcmp(key, "gaps") == 0) {
			/* already handled */
		} else if (strcmp(key, "gap_inner") == 0) {
			s->gap_inner = check_int_range(L, -1, key, 0, 256);
		} else if (strcmp(key, "gap_outer") == 0) {
			s->gap_outer = check_int_range(L, -1, key, 0, 256);
		} else if (strcmp(key, "focus_follows_mouse") == 0) {
			luaL_checktype(L, -1, LUA_TBOOLEAN);
			s->focus_follows_mouse = lua_toboolean(L, -1);
		} else if (strcmp(key, "border_width") == 0) {
			s->border_width = check_int_range(L, -1, key, 0, 16);
		} else if (strcmp(key, "border_color_focused") == 0) {
			if (lua_type(L, -1) != LUA_TSTRING)
				return luaL_error(L, "tp.config.border_color_focused: expected string");
			if (!tp_settings_parse_color(lua_tostring(L, -1),
										 s->border_color_focused))
				return luaL_error(
					L, "tp.config.border_color_focused: expected '#RRGGBB[AA]', "
					   "'0xRRGGBB[AA]', or 'none'");
		} else if (strcmp(key, "border_color_unfocused") == 0) {
			if (lua_type(L, -1) != LUA_TSTRING)
				return luaL_error(L, "tp.config.border_color_unfocused: expected string");
			if (!tp_settings_parse_color(lua_tostring(L, -1),
										 s->border_color_unfocused))
				return luaL_error(
					L, "tp.config.border_color_unfocused: expected '#RRGGBB[AA]', "
					   "'0xRRGGBB[AA]', or 'none'");
		} else if (strcmp(key, "locker") == 0) {
			check_string_buf(L, -1, key, true, s->locker, sizeof(s->locker));
			if (s->locker[0] && s->locker[0] != '/')
				return luaL_error(L, "tp.config.locker: expected an absolute path");
		} else if (strcmp(key, "split_ratio") == 0) {
			s->split_ratio = check_float_range(L, -1, key, 0.05f, 0.95f);
		} else if (strcmp(key, "width_bias") == 0) {
			s->width_bias = check_float_range(L, -1, key, 0.1f, 10.0f);
		} else if (strcmp(key, "preserve_split") == 0) {
			s->preserve_split = check_bool(L, -1, key);
		} else if (strcmp(key, "smart_split") == 0) {
			s->smart_split = check_bool(L, -1, key);
		} else if (strcmp(key, "proportional_split") == 0) {
			s->proportional_split = check_bool(L, -1, key);
		} else if (strcmp(key, "layout") == 0) {
			if (lua_type(L, -1) != LUA_TSTRING)
				return luaL_error(L, "tp.config.layout: expected string");
			enum tp_layout_kind kind;
			if (!tp_layout_kind_from_name(lua_tostring(L, -1), &kind))
				return luaL_error(
					L, "tp.config.layout: expected 'dwindle', 'master', "
					   "'scroller', or 'monocle'");
			s->layout = (int)kind;
		} else if (strcmp(key, "master_ratio") == 0) {
			s->master_ratio = check_float_range(L, -1, key, 0.10f, 0.90f);
		} else if (strcmp(key, "master_count") == 0) {
			s->master_count = check_int_range(L, -1, key, 1, 8);
		} else if (strcmp(key, "scroller_width") == 0) {
			s->scroller_width = check_float_range(L, -1, key, 0.15f, 1.0f);
		} else if (strcmp(key, "resize_step") == 0) {
			s->resize_step = check_int_range(L, -1, key, 1, 500);
		} else if (strcmp(key, "terminal") == 0) {
			check_string_buf(L, -1, key, false, s->terminal,
							 sizeof(s->terminal));
		} else if (strcmp(key, "cursor_theme") == 0) {
			check_string_buf(L, -1, key, true, s->cursor_theme,
							 sizeof(s->cursor_theme));
		} else if (strcmp(key, "cursor_size") == 0) {
			s->cursor_size = (uint32_t)check_int_range(L, -1, key, 1, 256);
		} else if (strcmp(key, "repeat_rate") == 0) {
			s->repeat_rate = check_int_range(L, -1, key, 1, 200);
		} else if (strcmp(key, "repeat_delay") == 0) {
			s->repeat_delay = check_int_range(L, -1, key, 1, 5000);
		} else if (strcmp(key, "kb_layout") == 0) {
			check_string_buf(L, -1, key, true, s->kb_layout,
							 sizeof(s->kb_layout));
		} else if (strcmp(key, "kb_variant") == 0) {
			check_string_buf(L, -1, key, true, s->kb_variant,
							 sizeof(s->kb_variant));
		} else if (strcmp(key, "kb_model") == 0) {
			check_string_buf(L, -1, key, true, s->kb_model,
							 sizeof(s->kb_model));
		} else if (strcmp(key, "kb_options") == 0) {
			check_string_buf(L, -1, key, true, s->kb_options,
							 sizeof(s->kb_options));
		} else if (strcmp(key, "kb_rules") == 0) {
			check_string_buf(L, -1, key, true, s->kb_rules,
							 sizeof(s->kb_rules));
		} else if (strcmp(key, "numlock_by_default") == 0) {
			s->numlock_by_default = check_bool(L, -1, key);
		} else if (strcmp(key, "natural_scroll") == 0) {
			s->natural_scroll = check_bool(L, -1, key);
		} else if (strcmp(key, "scroll_factor") == 0) {
			s->scroll_factor = check_float_range(L, -1, key, 0.05f, 10.0f);
		} else if (strcmp(key, "chord_enabled") == 0) {
			s->chord_enabled = check_bool(L, -1, key);
		} else if (strcmp(key, "chord_overlay_builtin") == 0) {
			s->chord_overlay_builtin = check_bool(L, -1, key);
		} else if (strcmp(key, "chord_key") == 0) {
			check_string_buf(L, -1, key, false, s->chord_key,
							 sizeof(s->chord_key));
		} else if (strcmp(key, "chord_secondary_key") == 0) {
			check_string_buf(L, -1, key, false, s->chord_secondary_key,
							 sizeof(s->chord_secondary_key));
		} else if (strcmp(key, "chord_cancel_key") == 0) {
			check_string_buf(L, -1, key, false, s->chord_cancel_key,
							 sizeof(s->chord_cancel_key));
		} else {
			tam_log_warn("lua: unknown setting '%s' ignored", key);
		}

		lua_pop(L, 1); /* pop value, keep key for lua_next */
	}

	return 0;
}

static int collect_argv_and_call(lua_State *L, const char *ctx,
								 void (*fn)(const char *const argv[],
											void *userdata),
								 void *userdata) {
	lua_Integer n = luaL_len(L, 1);
	if (n < 1)
		return luaL_error(L, "%s: argument list must not be empty", ctx);
	if (n > TP_LUA_SPAWN_ARGV_MAX)
		return luaL_error(L, "%s: more than %d arguments", ctx,
						  TP_LUA_SPAWN_ARGV_MAX);
	luaL_checkstack(L, (int)n, ctx);
	const char *argv[TP_LUA_SPAWN_ARGV_MAX + 1];
	for (lua_Integer i = 1; i <= n; i++) {
		if (lua_geti(L, 1, i) != LUA_TSTRING)
			return luaL_error(L, "%s: argument %d must be a string", ctx,
							  (int)i);
		size_t len = 0;
		const char *arg = lua_tolstring(L, -1, &len);
		if (strlen(arg) != len)
			return luaL_error(L, "%s: argument %d contains a NUL byte", ctx,
							  (int)i);
		if (i == 1 && len == 0)
			return luaL_error(L, "%s: program name must not be empty", ctx);
		argv[i - 1] = arg;
	}
	argv[n] = NULL;
	if (fn)
		fn(argv, userdata);
	return 0;
}

static void call_spawn_argv(const char *const argv[], void *userdata) {
	struct tp_lua *lua = userdata;
	if (lua && lua->spawn_argv_fn)
		lua->spawn_argv_fn(argv);
}

static int l_tp_spawn(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	/* A spawn while loading would run again on every reload, before the
	 * compositor's own WAYLAND_DISPLAY exists, and even when the rest of
	 * the config then fails. */
	if (lua && lua->staging)
		return luaL_error(
			L, "tp.spawn cannot run while the config loads; put autostart "
			   "in tp.on('startup', function() tp.spawn(...) end)");
	if (lua_type(L, 1) == LUA_TTABLE)
		return collect_argv_and_call(L, "tp.spawn", call_spawn_argv, lua);
	size_t len = 0;
	const char *cmd = luaL_checklstring(L, 1, &len);
	if (len == 0)
		return luaL_error(L, "tp.spawn: command must not be empty");
	if (lua && lua->spawn_fn)
		lua->spawn_fn(cmd);
	return 0;
}

static void call_lock_argv(const char *const argv[], void *userdata) {
	struct tp_lua *lua = userdata;
	if (lua && lua->lock_argv_fn)
		lua->lock_argv_fn(argv, lua->lock_argv_userdata);
}

static int l_tp_lock(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (lua && lua->staging)
		return luaL_error(L, "tp.lock cannot run while the config loads");
	if (lua_type(L, 1) == LUA_TTABLE)
		return collect_argv_and_call(L, "tp.lock", call_lock_argv, lua);
	size_t len = 0;
	const char *cmd = luaL_checklstring(L, 1, &len);
	if (len == 0)
		return luaL_error(L, "tp.lock: command must not be empty");
	if (lua && lua->lock_fn)
		lua->lock_fn(cmd, lua->lock_userdata);
	return 0;
}

static void call_capture_argv(const char *const argv[], void *userdata) {
	struct tp_lua *lua = userdata;
	if (lua && lua->capture_argv_fn)
		lua->capture_argv_fn(argv, lua->capture_argv_userdata);
}

static int l_tp_capture(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (lua && lua->staging)
		return luaL_error(L, "tp.capture cannot run while the config loads");
	if (lua_type(L, 1) == LUA_TTABLE)
		return collect_argv_and_call(L, "tp.capture", call_capture_argv, lua);
	size_t len = 0;
	const char *cmd = luaL_checklstring(L, 1, &len);
	if (len == 0)
		return luaL_error(L, "tp.capture: command must not be empty");
	if (lua && lua->capture_fn)
		lua->capture_fn(cmd, lua->capture_userdata);
	return 0;
}

static int l_tp_read_file(lua_State *L) {
	const char *path = luaL_checkstring(L, 1);
	size_t cap = TP_LUA_READ_FILE_DEFAULT_CAP;
	if (!lua_isnoneornil(L, 2)) {
		if (lua_type(L, 2) != LUA_TNUMBER || !lua_isinteger(L, 2))
			return luaL_error(L, "tp.read_file: max_bytes must be an integer");
		lua_Integer req = lua_tointeger(L, 2);
		if (req < 1 || req > TP_LUA_READ_FILE_MAX_CAP)
			return luaL_error(L, "tp.read_file: max_bytes out of range [1, %d]",
							  TP_LUA_READ_FILE_MAX_CAP);
		cap = (size_t)req;
	}

	char *buf = NULL;
	size_t len = 0;
	int err = 0;
	if (!read_regular_file(path, cap, false, &buf, &len, &err)) {
		lua_pushnil(L);
		if (err == EINVAL)
			lua_pushliteral(L, "not a regular file");
		else
			lua_pushstring(L, strerror(err));
		return 2;
	}

	lua_pushlstring(L, buf, len);
	free(buf);
	return 1;
}

/* ---- tp.include(path[, optional]) ---- */

enum { TP_LUA_INCLUDE_MAX_DEPTH = 8 };

static int l_tp_include(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging)
		return luaL_error(L, "tp.include can only be called while loading config");
	const char *path = luaL_checkstring(L, 1);
	bool optional = false;
	if (!lua_isnoneornil(L, 2)) {
		if (lua_type(L, 2) != LUA_TBOOLEAN)
			return luaL_error(L, "tp.include: optional must be a boolean");
		optional = lua_toboolean(L, 2) != 0;
	}
	if (lua->include_depth >= TP_LUA_INCLUDE_MAX_DEPTH)
		return luaL_error(L, "tp.include: maximum include depth (%d) exceeded",
						  TP_LUA_INCLUDE_MAX_DEPTH);

	char *buf = NULL;
	size_t len = 0;
	int err = 0;
	if (!read_regular_file(path, TP_LUA_READ_FILE_MAX_CAP, true, &buf, &len,
						   &err)) {
		if (optional && err == ENOENT) {
			lua_pushboolean(L, 0);
			return 1;
		}
		if (err == EINVAL)
			return luaL_error(L, "tp.include: %s is not a regular file", path);
		return luaL_error(L, "tp.include: cannot read %s: %s", path,
						  strerror(err));
	}

	char chunk_name[512];
	snprintf(chunk_name, sizeof(chunk_name), "@%s", path);
	int rc = luaL_loadbufferx(L, buf, len, chunk_name, "t");
	free(buf);
	if (rc != LUA_OK)
		return lua_error(L);

	lua->include_depth++;
	int call_rc = lua_pcall(L, 0, 0, 0);
	lua->include_depth--;
	if (call_rc != LUA_OK)
		return lua_error(L);

	lua_pushboolean(L, 1);
	return 1;
}

/* ---- tp.monitor(spec) / tp.clear_monitors() ---- */

static void monitor_list_clear(struct tp_lua_monitor_list *list) {
	free(list->items);
	list->items = NULL;
	list->len = 0;
	list->cap = 0;
}

static bool monitor_list_set(struct tp_lua_monitor_list *list,
							 const struct tp_monitor_config *cfg) {
	for (size_t i = 0; i < list->len; i++) {
		if (strcmp(list->items[i].output, cfg->output) == 0) {
			list->items[i] = *cfg;
			return true;
		}
	}
	if (list->len == list->cap) {
		size_t next = list->cap ? list->cap * 2 : 8;
		struct tp_monitor_config *grown =
			realloc(list->items, next * sizeof(*grown));
		if (!grown)
			return false;
		list->items = grown;
		list->cap = next;
	}
	list->items[list->len++] = *cfg;
	return true;
}

static bool parse_mode_string(const char *str, struct tp_monitor_config *cfg) {
	if (!str || !*str)
		return false;
	if (strcmp(str, "preferred") == 0) {
		cfg->has_mode = true;
		cfg->mode_preferred = true;
		return true;
	}
	int w = 0, h = 0;
	double hz = 0.0;
	char extra = 0;
	int n = sscanf(str, "%dx%d@%lf%c", &w, &h, &hz, &extra);
	if (n == 3 && w > 0 && h > 0 && hz > 0.0 && hz <= 1000.0) {
		cfg->has_mode = true;
		cfg->mode_preferred = false;
		cfg->mode_w = w;
		cfg->mode_h = h;
		cfg->mode_refresh_mhz = (int32_t)(hz * 1000.0 + 0.5);
		return true;
	}
	n = sscanf(str, "%dx%d%c", &w, &h, &extra);
	if (n == 2 && w > 0 && h > 0) {
		cfg->has_mode = true;
		cfg->mode_preferred = false;
		cfg->mode_w = w;
		cfg->mode_h = h;
		cfg->mode_refresh_mhz = 0;
		return true;
	}
	return false;
}

static int l_tp_monitor(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_monitors)
		return luaL_error(L, "tp.monitor can only be called while loading config");
	luaL_checktype(L, 1, LUA_TTABLE);

	struct tp_monitor_config cfg = {0};
	bool has_output = false;

	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		if (lua_type(L, -2) != LUA_TSTRING)
			return luaL_error(L, "tp.monitor: keys must be strings");
		const char *key = lua_tostring(L, -2);
		if (strcmp(key, "output") == 0) {
			check_string_buf(L, -1, "tp.monitor.output", true, cfg.output,
							 sizeof(cfg.output));
			has_output = true;
		} else if (strcmp(key, "mode") == 0) {
			if (lua_type(L, -1) != LUA_TSTRING)
				return luaL_error(L, "tp.monitor.mode: expected string");
			if (!parse_mode_string(lua_tostring(L, -1), &cfg))
				return luaL_error(
					L, "tp.monitor.mode: expected 'preferred' or 'WxH[@Hz]'");
		} else if (strcmp(key, "position") == 0) {
			if (lua_type(L, -1) == LUA_TSTRING) {
				const char *ps = lua_tostring(L, -1);
				if (strcmp(ps, "auto") == 0) {
					cfg.has_pos = true;
					cfg.pos_auto = true;
				} else {
					int x = 0, y = 0;
					char extra = 0;
					if (sscanf(ps, "%dx%d%c", &x, &y, &extra) != 2 &&
						sscanf(ps, "%d,%d%c", &x, &y, &extra) != 2)
						return luaL_error(
							L, "tp.monitor.position: expected 'auto', 'XxY', or {x, y}");
					cfg.has_pos = true;
					cfg.pos_auto = false;
					cfg.pos_x = x;
					cfg.pos_y = y;
				}
			} else if (lua_type(L, -1) == LUA_TTABLE) {
				lua_getfield(L, -1, "x");
				if (lua_isnil(L, -1)) {
					lua_pop(L, 1);
					lua_geti(L, -1, 1);
				}
				int32_t x = tp_lua_check_int(L, -1, "tp.monitor.position.x",
											 -32768, 32768);
				lua_pop(L, 1);
				lua_getfield(L, -1, "y");
				if (lua_isnil(L, -1)) {
					lua_pop(L, 1);
					lua_geti(L, -1, 2);
				}
				int32_t y = tp_lua_check_int(L, -1, "tp.monitor.position.y",
											 -32768, 32768);
				lua_pop(L, 1);
				cfg.has_pos = true;
				cfg.pos_auto = false;
				cfg.pos_x = x;
				cfg.pos_y = y;
			} else {
				return luaL_error(
					L, "tp.monitor.position: expected string or table");
			}
		} else if (strcmp(key, "scale") == 0) {
			if (lua_type(L, -1) == LUA_TSTRING &&
				strcmp(lua_tostring(L, -1), "auto") == 0) {
				cfg.has_scale = false;
			} else {
				cfg.scale = check_float_range(L, -1, "tp.monitor.scale", 0.5f,
											  8.0f);
				cfg.has_scale = true;
			}
		} else if (strcmp(key, "transform") == 0) {
			cfg.transform =
				check_int_range(L, -1, "tp.monitor.transform", 0, 7);
			cfg.has_transform = true;
		} else if (strcmp(key, "enabled") == 0) {
			cfg.enabled = check_bool(L, -1, "tp.monitor.enabled");
			cfg.has_enabled = true;
		} else if (strcmp(key, "disabled") == 0) {
			cfg.enabled = !check_bool(L, -1, "tp.monitor.disabled");
			cfg.has_enabled = true;
		} else {
			tam_log_warn("tp.monitor: unknown key '%s' ignored", key);
		}
		lua_pop(L, 1);
	}
	if (!has_output)
		return luaL_error(L, "tp.monitor: 'output' is required");
	if (!monitor_list_set(&lua->staged_monitors, &cfg))
		return luaL_error(L, "tp.monitor: out of memory");
	return 0;
}

static int l_tp_clear_monitors(lua_State *L) {
	struct tp_lua *lua = tp_lua_from_state(L);
	if (!lua || !lua->staging_monitors)
		return luaL_error(
			L, "tp.clear_monitors can only be called while loading config");
	monitor_list_clear(&lua->staged_monitors);
	return 0;
}

bool tp_lua_monitor_rule(const struct tp_lua *lua, const char *output_name,
						 struct tp_monitor_config *out) {
	if (!lua || !out)
		return false;
	const struct tp_monitor_config *wildcard = NULL;
	for (size_t i = 0; i < lua->active_monitors.len; i++) {
		const struct tp_monitor_config *m = &lua->active_monitors.items[i];
		if (output_name && *output_name && strcmp(m->output, output_name) == 0) {
			*out = *m;
			return true;
		}
		if (m->output[0] == '\0')
			wildcard = m;
	}
	if (wildcard) {
		*out = *wildcard;
		return true;
	}
	return false;
}

size_t tp_lua_monitor_count(const struct tp_lua *lua) {
	return lua ? lua->active_monitors.len : 0;
}

static const luaL_Reg safe_libs[] = {
	{LUA_GNAME, luaopen_base},
	{LUA_COLIBNAME, luaopen_coroutine},
	{LUA_MATHLIBNAME, luaopen_math},
	{LUA_STRLIBNAME, luaopen_string},
	{LUA_TABLIBNAME, luaopen_table},
	{LUA_UTF8LIBNAME, luaopen_utf8},
	{NULL, NULL},
};

static const luaL_Reg tp_funcs[] = {
	{"config", l_tp_config},
	{"spawn", l_tp_spawn},
	{"lock", l_tp_lock},
	{"capture", l_tp_capture},
	{"read_file", l_tp_read_file},
	{"include", l_tp_include},
	{"monitor", l_tp_monitor},
	{"clear_monitors", l_tp_clear_monitors},
	{NULL, NULL},
};

struct tp_lua *tp_lua_create(void) {
	struct tp_lua *lua = calloc(1, sizeof(*lua));
	if (!lua)
		return NULL;

	lua->L = luaL_newstate();
	if (!lua->L) {
		free(lua);
		return NULL;
	}
	/* Count from here on; what luaL_newstate allocated is already in use. */
	lua->mem_used = (size_t)lua_gc(lua->L, LUA_GCCOUNT) * 1024 +
					(size_t)lua_gc(lua->L, LUA_GCCOUNTB);
	lua_setallocf(lua->L, tp_lua_alloc, lua);
	lua->spawn_fn = tp_spawn;
	lua->spawn_argv_fn = tp_spawn_argv;

	lua_State *L = lua->L;
	lua_pushlightuserdata(L, &tp_lua_registry_key);
	lua_pushlightuserdata(L, lua);
	lua_rawset(L, LUA_REGISTRYINDEX);

	for (const luaL_Reg *lib = safe_libs; lib->func; lib++) {
		luaL_requiref(L, lib->name, lib->func, 1);
		lua_pop(L, 1);
	}

	/* Strip blocking/unbounded file-loading globals from base. */
	lua_pushnil(L);
	lua_setglobal(L, "dofile");
	lua_pushnil(L);
	lua_setglobal(L, "loadfile");

	/* Route print() through tam_log_info. */
	lua_pushcfunction(L, l_print);
	lua_setglobal(L, "print");

	/* Expose the tp table (config, spawn, read_file, include, monitor, bind/unbind/dsp, on/off, rule). */
	luaL_newlib(L, tp_funcs);
	lua_pushliteral(L, TP_VERSION);
	lua_setfield(L, -2, "version");
	const char *home = getenv("HOME");
	lua_pushstring(L, (home && *home) ? home : "");
	lua_setfield(L, -2, "home");
	const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
	char cfg_dir[PATH_MAX];
	if (xdg_cfg && *xdg_cfg) {
		snprintf(cfg_dir, sizeof(cfg_dir), "%s", xdg_cfg);
	} else if (home && *home) {
		snprintf(cfg_dir, sizeof(cfg_dir), "%s/.config", home);
	} else {
		cfg_dir[0] = '\0';
	}
	lua_pushstring(L, cfg_dir);
	lua_setfield(L, -2, "config_dir");
	tp_lua_bindings_open(L);
	tp_lua_events_open(L);
	tp_lua_rules_open(L);
	lua_setglobal(L, "tp");

	tp_lua_bindings_load_defaults(lua);
	return lua;
}

void tp_lua_destroy(struct tp_lua *lua) {
	if (!lua)
		return;
	monitor_list_clear(&lua->staged_monitors);
	monitor_list_clear(&lua->active_monitors);
	tp_lua_rules_finish(lua);
	tp_lua_hooks_finish(lua);
	tp_lua_bindings_finish(lua);
	if (lua->L)
		lua_close(lua->L);
	free(lua);
}

void tp_lua_set_spawn_fn(struct tp_lua *lua, void (*fn)(const char *cmd)) {
	if (lua)
		lua->spawn_fn = fn;
}

void tp_lua_set_spawn_argv_fn(struct tp_lua *lua,
							  void (*fn)(const char *const argv[])) {
	if (lua)
		lua->spawn_argv_fn = fn;
}

void tp_lua_set_lock_fn(struct tp_lua *lua, tp_lua_lock_fn fn, void *userdata) {
	if (!lua)
		return;
	lua->lock_fn = fn;
	lua->lock_userdata = userdata;
}

void tp_lua_set_lock_argv_fn(struct tp_lua *lua, tp_lua_lock_argv_fn fn,
							 void *userdata) {
	if (!lua)
		return;
	lua->lock_argv_fn = fn;
	lua->lock_argv_userdata = userdata;
}

void tp_lua_set_capture_fn(struct tp_lua *lua, tp_lua_capture_fn fn,
						   void *userdata) {
	if (!lua)
		return;
	lua->capture_fn = fn;
	lua->capture_userdata = userdata;
}

void tp_lua_set_capture_argv_fn(struct tp_lua *lua, tp_lua_capture_argv_fn fn,
								void *userdata) {
	if (!lua)
		return;
	lua->capture_argv_fn = fn;
	lua->capture_argv_userdata = userdata;
}

bool tp_lua_default_config_path(char *out, size_t out_size) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	int n;
	if (xdg && *xdg) {
		n = snprintf(out, out_size, "%s/tam-pane/init.lua", xdg);
	} else {
		const char *home = getenv("HOME");
		if (!home || !*home)
			return false;
		n = snprintf(out, out_size, "%s/.config/tam-pane/init.lua", home);
	}
	return n > 0 && (size_t)n < out_size;
}

bool tp_lua_exec_string(struct tp_lua *lua, const char *code, size_t code_len,
						const char *chunk_name, uint64_t max_instructions,
						uint32_t timeout_ms, const struct tp_settings *base,
						struct tp_settings *out, char *err_buf,
						size_t err_buf_size) {
	if (!lua || !lua->L || !code)
		return false;

	lua_State *L = lua->L;
	int top = lua_gettop(L);

	struct tp_settings staged;
	if (base)
		staged = *base;
	else
		tp_settings_defaults(&staged);

	/* Text-only ("t") rejects precompiled binary Lua bytecode. */
	int rc = luaL_loadbufferx(L, code, code_len, chunk_name ? chunk_name : "lua",
							  "t");
	if (rc != LUA_OK) {
		const char *msg = lua_tostring(L, -1);
		if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "%s", msg ? msg : "load error");
		lua_settop(L, top);
		return false;
	}

	if (!tp_lua_bindings_stage_begin(lua)) {
		if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "out of memory staging bindings");
		lua_settop(L, top);
		return false;
	}
	tp_lua_hooks_stage_begin(lua);
	tp_lua_rules_stage_begin(lua);
	monitor_list_clear(&lua->staged_monitors);
	lua->staging_monitors = true;

	struct tp_settings *prev_staging = lua->staging;
	lua->staging = &staged;
	rc = tp_lua_guarded_pcall(lua, 0, 0, max_instructions, timeout_ms,
							  chunk_name ? chunk_name : "lua");
	lua->staging = prev_staging;

	if (rc != LUA_OK) {
		const char *msg = lua_tostring(L, -1);
		if (err_buf && err_buf_size > 0 && rc == LUA_ERRMEM && lua->mem_refused)
			snprintf(err_buf, err_buf_size, "Lua memory limit (%d MiB) reached",
					 TP_LUA_MEMORY_MAX >> 20);
		else if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "%s", msg ? msg : "runtime error");
		lua->staging_monitors = false;
		monitor_list_clear(&lua->staged_monitors);
		tp_lua_rules_stage_abort(lua);
		tp_lua_hooks_stage_abort(lua);
		tp_lua_bindings_stage_abort(lua);
		lua_settop(L, top);
		return false;
	}

	lua->staging_monitors = false;
	monitor_list_clear(&lua->active_monitors);
	lua->active_monitors = lua->staged_monitors;
	lua->staged_monitors = (struct tp_lua_monitor_list){0};
	tp_lua_rules_stage_commit(lua);
	tp_lua_hooks_stage_commit(lua);
	tp_lua_bindings_stage_commit(lua);
	if (out)
		*out = staged;
	lua_settop(L, top);
	return true;
}

static bool load_file_common(struct tp_lua *lua, const char *path,
							 const struct tp_settings *base,
							 struct tp_settings *out, int *out_errno,
							 char *err_buf, size_t err_buf_size) {
	if (out_errno)
		*out_errno = 0;
	if (!lua || !path || !*path) {
		if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "invalid config path");
		return false;
	}

	char *buf = NULL;
	size_t len = 0;
	int err = 0;
	if (!read_regular_file(path, TP_LUA_READ_FILE_MAX_CAP, true, &buf, &len,
						   &err)) {
		if (out_errno)
			*out_errno = err;
		if (err_buf && err_buf_size > 0) {
			if (err == ENOENT)
				snprintf(err_buf, err_buf_size, "config file not found: %s",
						 path);
			else if (err == EFBIG)
				snprintf(err_buf, err_buf_size,
						 "config file %s exceeds %d bytes", path,
						 TP_LUA_READ_FILE_MAX_CAP);
			else if (err == EINVAL)
				snprintf(err_buf, err_buf_size,
						 "config path %s is not a regular file", path);
			else
				snprintf(err_buf, err_buf_size, "cannot read config %s: %s",
						 path, strerror(err));
		}
		return false;
	}

	char chunk_name[512];
	snprintf(chunk_name, sizeof(chunk_name), "@%s", path);

	bool ok = tp_lua_exec_string(
		lua, buf, len, chunk_name, TP_LUA_CONFIG_MAX_INSTRUCTIONS,
		TP_LUA_CONFIG_TIMEOUT_MS, base, out, err_buf, err_buf_size);
	free(buf);
	return ok;
}

bool tp_lua_load_file(struct tp_lua *lua, const char *path,
					  bool warn_if_missing, const struct tp_settings *base,
					  struct tp_settings *out) {
	int err = 0;
	char err_buf[512] = {0};
	if (!load_file_common(lua, path, base, out, &err, err_buf,
						  sizeof(err_buf))) {
		if (err == ENOENT) {
			if (warn_if_missing)
				tam_log_warn("config file not found: %s (using defaults)",
							 path);
			else
				tam_log_debug("no config file at %s (using defaults)", path);
		} else if (err != 0) {
			tam_log_error("%s (using defaults)", err_buf);
		} else {
			tam_log_error("config %s failed: %s (using defaults)", path,
						  err_buf);
		}
		return false;
	}

	tam_log_info("loaded config: %s", path);
	return true;
}

bool tp_lua_reload_file(struct tp_lua **lua_slot, const char *path,
						struct tp_settings *out, char *err_buf,
						size_t err_buf_size) {
	if (!lua_slot || !path || !*path || !out) {
		if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "invalid reload arguments");
		return false;
	}

	struct tp_lua *next = tp_lua_create();
	if (!next) {
		if (err_buf && err_buf_size > 0)
			snprintf(err_buf, err_buf_size, "cannot create Lua state");
		return false;
	}

	if (*lua_slot) {
		next->spawn_fn = (*lua_slot)->spawn_fn;
		next->spawn_argv_fn = (*lua_slot)->spawn_argv_fn;
		next->lock_fn = (*lua_slot)->lock_fn;
		next->lock_userdata = (*lua_slot)->lock_userdata;
		next->lock_argv_fn = (*lua_slot)->lock_argv_fn;
		next->lock_argv_userdata = (*lua_slot)->lock_argv_userdata;
		next->capture_fn = (*lua_slot)->capture_fn;
		next->capture_userdata = (*lua_slot)->capture_userdata;
		next->capture_argv_fn = (*lua_slot)->capture_argv_fn;
		next->capture_argv_userdata = (*lua_slot)->capture_argv_userdata;
		next->action_fn = (*lua_slot)->action_fn;
		next->action_userdata = (*lua_slot)->action_userdata;
	}

	struct tp_settings next_settings;
	tp_settings_defaults(&next_settings);
	int err = 0;
	if (!load_file_common(next, path, NULL, &next_settings, &err, err_buf,
						  err_buf_size)) {
		tp_lua_destroy(next);
		return false;
	}

	tp_lua_destroy(*lua_slot);
	*lua_slot = next;
	*out = next_settings;
	return true;
}
