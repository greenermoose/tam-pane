/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pure unit tests for tam-pane's sandboxed Lua 5.5 runtime (no wlroots). */
#include "check.h"
#include "tam-pane/lua.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <tam.h>
#include <time.h>
#include <unistd.h>

static char last_spawned[256];
static int spawn_count;

static void mock_spawn(const char *cmd) {
	spawn_count++;
	snprintf(last_spawned, sizeof(last_spawned), "%s", cmd);
}

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool write_text(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	size_t n = strlen(text);
	bool ok = fwrite(text, 1, n, f) == n;
	fclose(f);
	return ok;
}

static void test_config_and_fallback(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	out = base;

	/* Missing file leaves defaults intact and returns false. */
	CHECK(!tp_lua_load_file(lua, "/tmp/tam-pane-nonexistent-init.lua", false,
							&base, &out));
	CHECK(out.gap_inner == base.gap_inner);
	CHECK(out.gap_outer == base.gap_outer);

	/* Valid tp.config updates every field; gap_outer overrides gaps. */
	const char *cfg =
		"tp.config{\n"
		"  gaps = 12,\n"
		"  gap_outer = 20,\n"
		"  split_ratio = 0.6,\n"
		"  width_bias = 1.25,\n"
		"  preserve_split = true,\n"
		"  smart_split = true,\n"
		"  proportional_split = true,\n"
		"  resize_step = 64,\n"
		"  terminal = 'kitty',\n"
		"  cursor_theme = 'Adwaita',\n"
		"  cursor_size = 32,\n"
		"  repeat_rate = 40,\n"
		"  repeat_delay = 300,\n"
		"  unknown_future_key = 123,\n"
		"}\n";
	char err[256] = {0};
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "test_valid",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(out.gap_inner == 12);
	CHECK(out.gap_outer == 20);
	CHECK(fabsf(out.split_ratio - 0.6f) < 0.001f);
	CHECK(fabsf(out.width_bias - 1.25f) < 0.001f);
	CHECK(out.preserve_split == true);
	CHECK(out.smart_split == true);
	CHECK(out.proportional_split == true);
	CHECK(out.resize_step == 64);
	CHECK(strcmp(out.terminal, "kitty") == 0);
	CHECK(tp_settings_cursor_theme(&out) != NULL);
	CHECK(strcmp(out.cursor_theme, "Adwaita") == 0);
	CHECK(out.cursor_size == 32);
	CHECK(out.repeat_rate == 40);
	CHECK(out.repeat_delay == 300);

	/* Empty cursor_theme resets to NULL helper result. */
	const char *clear_cursor = "tp.config{ cursor_theme = '' }";
	CHECK(tp_lua_exec_string(lua, clear_cursor, strlen(clear_cursor), "clear",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &out, &out, err,
							 sizeof(err)));
	CHECK(tp_settings_cursor_theme(&out) == NULL);

	/* Atomic rollback: valid tp.config followed by runtime error keeps base. */
	out = base;
	const char *partial_err =
		"tp.config{ gaps = 42 }\n"
		"error('boom')\n";
	CHECK(!tp_lua_exec_string(lua, partial_err, strlen(partial_err), "partial",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	CHECK(strstr(err, "boom") != NULL);
	CHECK(out.gap_inner == base.gap_inner);
	CHECK(out.gap_outer == base.gap_outer);

	/* Syntax error fails cleanly. */
	const char *bad_syntax = "tp.config{ gaps = ";
	CHECK(!tp_lua_exec_string(lua, bad_syntax, strlen(bad_syntax), "syntax",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));

	/* Type and range validation reject bad values. */
	const char *bad_cases[] = {
		"tp.config{ gap_inner = '8' }",
		"tp.config{ gap_inner = -1 }",
		"tp.config{ gap_outer = 999 }",
		"tp.config{ split_ratio = 1.5 }",
		"tp.config{ preserve_split = 1 }",
		"tp.config{ terminal = '' }",
		"tp.config{ cursor_size = 0 }",
		"tp.config{ repeat_rate = 0 }",
		"tp.config{ [1] = 2 }",
	};
	for (size_t i = 0; i < sizeof(bad_cases) / sizeof(bad_cases[0]); i++) {
		CHECK(!tp_lua_exec_string(lua, bad_cases[i], strlen(bad_cases[i]),
								  "bad_val", TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
		CHECK(out.gap_inner == base.gap_inner);
	}

	tp_lua_destroy(lua);
}

static void test_sandbox_and_helpers(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_spawn_fn(lua, mock_spawn);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	out = base;
	char err[256] = {0};

	/* Unsafe libraries/globals must be nil; safe ones must work. */
	const char *sandbox_check =
		"assert(io == nil, 'io must be nil')\n"
		"assert(os == nil, 'os must be nil')\n"
		"assert(package == nil, 'package must be nil')\n"
		"assert(debug == nil, 'debug must be nil')\n"
		"assert(dofile == nil, 'dofile must be nil')\n"
		"assert(loadfile == nil, 'loadfile must be nil')\n"
		"assert(type(tp.version) == 'string' and #tp.version > 0)\n"
		"assert(math.floor(3.7) == 3)\n"
		"assert(string.upper('ok') == 'OK')\n"
		"assert(table.concat({'a', 'b'}, '-') == 'a-b')\n"
		"assert(utf8.len('tam-pane') == 8)\n"
		"local co = coroutine.create(function() return 7 end)\n"
		"local ok, val = coroutine.resume(co)\n"
		"assert(ok and val == 7)\n"
		"print('hello from lua', 5.5)\n"
		"tp.spawn('foot -e true')\n";
	spawn_count = 0;
	last_spawned[0] = '\0';
	CHECK(tp_lua_exec_string(lua, sandbox_check, strlen(sandbox_check),
							 "sandbox", TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(spawn_count == 1);
	CHECK(strcmp(last_spawned, "foot -e true") == 0);

	/* Precompiled binary bytecode chunk must be rejected ("t" mode). */
	const char bytecode[] = "\x1bLua\x55\x00";
	CHECK(!tp_lua_exec_string(lua, bytecode, sizeof(bytecode) - 1, "bytecode",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));

	/* File loading and tp.read_file (including FIFO non-blocking refusal). */
	char dir[] = "/tmp/tam-pane-lua-test.XXXXXX";
	CHECK(mkdtemp(dir) != NULL);

	char data_path[256], fifo_path[256], cfg_path[256];
	snprintf(data_path, sizeof(data_path), "%s/sample.txt", dir);
	snprintf(fifo_path, sizeof(fifo_path), "%s/pipe", dir);
	snprintf(cfg_path, sizeof(cfg_path), "%s/init.lua", dir);

	CHECK(write_text(data_path, "0123456789abcdef"));
	CHECK(mkfifo(fifo_path, 0600) == 0);

	char script[1024];
	snprintf(script, sizeof(script),
			 "local full, e1 = tp.read_file('%s')\n"
			 "assert(full == '0123456789abcdef' and e1 == nil)\n"
			 "local capped = tp.read_file('%s', 4)\n"
			 "assert(capped == '0123')\n"
			 "local p, e2 = tp.read_file('%s')\n"
			 "assert(p == nil and e2 == 'not a regular file')\n"
			 "tp.config{ gaps = 14 }\n",
			 data_path, data_path, fifo_path);
	CHECK(write_text(cfg_path, script));

	out = base;
	CHECK(tp_lua_load_file(lua, cfg_path, true, &base, &out));
	CHECK(out.gap_inner == 14);
	CHECK(out.gap_outer == 14);

	/* Loading a FIFO as init.lua must fail immediately without blocking. */
	out = base;
	CHECK(!tp_lua_load_file(lua, fifo_path, true, &base, &out));
	CHECK(out.gap_inner == base.gap_inner);

	unlink(data_path);
	unlink(fifo_path);
	unlink(cfg_path);
	rmdir(dir);

	tp_lua_destroy(lua);
}

static void test_watchdog(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	out = base;
	char err[256] = {0};

	/* 1. Cut off by instruction budget. */
	const char *loop = "while true do end";
	CHECK(!tp_lua_exec_string(lua, loop, strlen(loop), "instr_test", 50000,
							  5000, &base, &out, err, sizeof(err)));
	CHECK(strstr(err, "instruction budget") != NULL);

	/* 2. Cut off by wall-clock deadline (huge instruction budget, 40ms cap). */
	uint64_t t0 = now_ms();
	CHECK(!tp_lua_exec_string(lua, loop, strlen(loop), "clock_test",
							  10000000000ULL, 40, &base, &out, err,
							  sizeof(err)));
	uint64_t elapsed = now_ms() - t0;
	CHECK(strstr(err, "timed out") != NULL);
	CHECK(elapsed < 500);

	/* 3. Nested pcall cannot escape the watchdog. */
	const char *escape =
		"while true do pcall(function() while true do end end) end";
	CHECK(!tp_lua_exec_string(lua, escape, strlen(escape), "escape_test",
							  100000, 1000, &base, &out, err, sizeof(err)));

	/* 4. Interpreter remains healthy after watchdog aborts. */
	const char *good = "tp.config{ gaps = 18 }";
	CHECK(tp_lua_exec_string(lua, good, strlen(good), "recover",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(out.gap_inner == 18);
	CHECK(out.gap_outer == 18);

	tp_lua_destroy(lua);
}

static void test_default_path(void) {
	char path[512];
	const char *old_xdg = getenv("XDG_CONFIG_HOME");
	char *saved_xdg = old_xdg ? strdup(old_xdg) : NULL;

	setenv("XDG_CONFIG_HOME", "/tmp/custom-xdg", 1);
	CHECK(tp_lua_default_config_path(path, sizeof(path)));
	CHECK(strcmp(path, "/tmp/custom-xdg/tam-pane/init.lua") == 0);

	unsetenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (home && *home) {
		char expected[512];
		snprintf(expected, sizeof(expected), "%s/.config/tam-pane/init.lua",
				 home);
		CHECK(tp_lua_default_config_path(path, sizeof(path)));
		CHECK(strcmp(path, expected) == 0);
	}

	if (saved_xdg) {
		setenv("XDG_CONFIG_HOME", saved_xdg, 1);
		free(saved_xdg);
	} else {
		unsetenv("XDG_CONFIG_HOME");
	}
}

int main(void) {
	tam_log_set_level(TAM_LOG_ERROR);
	test_config_and_fallback();
	test_sandbox_and_helpers();
	test_watchdog();
	test_default_path();
	CHECK_DONE("test_lua");
}
