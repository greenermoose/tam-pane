/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pure unit tests for tam-pane's sandboxed Lua 5.5 runtime (no wlroots). */
#include "check.h"
#include "tam-pane/lua.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <tam.h>
#include <time.h>
#include <unistd.h>

static char last_spawned[256];
static int spawn_count;
static char spawn_log[16][256];
static int spawn_log_count;

static struct tp_action last_action;
static int action_count;

static void mock_spawn(const char *cmd) {
	spawn_count++;
	snprintf(last_spawned, sizeof(last_spawned), "%s", cmd);
	if (spawn_log_count < 16)
		snprintf(spawn_log[spawn_log_count++], sizeof(spawn_log[0]), "%s", cmd);
}

static int argv_spawn_count;
static char last_argv[512]; /* arguments joined with '|' */

static void mock_spawn_argv(const char *const argv[]) {
	argv_spawn_count++;
	size_t pos = 0;
	last_argv[0] = '\0';
	for (size_t i = 0; argv[i]; i++) {
		int n = snprintf(last_argv + pos, sizeof(last_argv) - pos, "%s%s",
						 i ? "|" : "", argv[i]);
		if (n < 0 || (size_t)n >= sizeof(last_argv) - pos)
			break;
		pos += (size_t)n;
	}
}

static void reset_spawn_log(void) {
	spawn_count = 0;
	spawn_log_count = 0;
	last_spawned[0] = '\0';
}

static void mock_action(const struct tp_action *act, void *userdata) {
	(void)userdata;
	action_count++;
	last_action = *act;
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

	/* One integer check for config, rules and dispatchers (R1 finding
	 * 14): the same two messages everywhere. Every message is fully
	 * formatted: lua_pushfstring knows only %d %I %f %s %c %p %U, and
	 * before 0.1.16 %lld, %u, %zu and %.2f reached the user (B8). */
	{
		static const struct {
			const char *src, *want;
		} cases[] = {
			{"tp.config{ gaps = 999 }",
			 "tp.config.gaps: 999 out of range [0, 256]"},
			{"tp.config{ gaps = 1.5 }", "tp.config.gaps: expected integer"},
			{"tp.dsp.tag('view', 10)", "tp.dsp.tag: 10 out of range [1, 9]"},
			{"tp.dsp.tag('view', 'x')", "tp.dsp.tag: expected integer"},
			{"tp.rule{ match = { title = 'x' }, tag = 0 }",
			 "tp.rule.tag: 0 out of range [1, 9]"},
			{"tp.rule{ match = { title = 'x' }, size = { 100, 200, 3 } }",
			 "unexpected index 3"},
			{"tp.config{ width_bias = 100 }",
			 "tp.config.width_bias: value out of range [0.10, 10.00]"},
			{"tp.dsp.spawn(string.rep('x', 100000))",
			 "tp.dsp.spawn: command too long (max "},
			{"tp.rule{ match = { title = string.rep('x', 100000) } }",
			 "string too long (max "},
		};
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			out = base;
			err[0] = '\0';
			CHECK(!tp_lua_exec_string(lua, cases[i].src, strlen(cases[i].src),
									  "int-check", TP_LUA_CONFIG_MAX_INSTRUCTIONS,
									  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
									  sizeof(err)));
			if (!strstr(err, cases[i].want))
				fprintf(stderr, "int-check %zu: got '%s'\n", i, err);
			CHECK(strstr(err, cases[i].want) != NULL);
			CHECK(strchr(err, '%') == NULL);
		}
	}
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
	tp_lua_set_spawn_argv_fn(lua, mock_spawn_argv);

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
		"print('hello from lua', 5.5)\n";
	CHECK(tp_lua_exec_string(lua, sandbox_check, strlen(sandbox_check),
							 "sandbox", TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));

	/* tp.spawn while the config loads would run again on every reload and
	 * before WAYLAND_DISPLAY is set: it is an error that names the startup
	 * hook, in both forms, and nothing is spawned. */
	spawn_count = 0;
	argv_spawn_count = 0;
	const char *spawn_at_load[] = {"tp.spawn('foot -e true')",
								   "tp.spawn{'foot', '-e', 'true'}"};
	for (size_t i = 0; i < 2; i++) {
		err[0] = '\0';
		CHECK(!tp_lua_exec_string(lua, spawn_at_load[i],
								  strlen(spawn_at_load[i]), "spawn_at_load",
								  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
		CHECK(strstr(err, "tp.on('startup'") != NULL);
	}
	CHECK(spawn_count == 0 && argv_spawn_count == 0);

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

static void test_chord_parser(void) {
	struct tp_key_spec spec;
	char err[128] = {0};

	CHECK(tp_binding_parse_chord("Meta+Return", &spec, err, sizeof(err)));
	CHECK(spec.mods == TP_MOD_META && spec.sym == XKB_KEY_Return);

	/* Uppercase letter canonicalizes to level-0 lowercase keysym. */
	CHECK(tp_binding_parse_chord("Meta+Shift+E", &spec, err, sizeof(err)));
	CHECK(spec.mods == (TP_MOD_META | TP_MOD_SHIFT) && spec.sym == XKB_KEY_e);

	/* Modifier and key aliases, case-insensitivity, whitespace trimming. */
	CHECK(tp_binding_parse_chord(" super + control + mod1 + shift + Enter ",
								 &spec, err, sizeof(err)));
	CHECK(spec.mods ==
			  (TP_MOD_META | TP_MOD_CTRL | TP_MOD_ALT | TP_MOD_SHIFT) &&
		  spec.sym == XKB_KEY_Return);

	CHECK(tp_binding_parse_chord("Meta+,", &spec, err, sizeof(err)));
	CHECK(spec.mods == TP_MOD_META && spec.sym == XKB_KEY_comma);
	CHECK(tp_binding_parse_chord("Meta+.", &spec, err, sizeof(err)));
	CHECK(spec.mods == TP_MOD_META && spec.sym == XKB_KEY_period);
	CHECK(tp_binding_parse_chord("Meta++", &spec, err, sizeof(err)));
	CHECK(spec.mods == TP_MOD_META && spec.sym == XKB_KEY_plus);
	CHECK(tp_binding_parse_chord("+", &spec, err, sizeof(err)));
	CHECK(spec.mods == 0 && spec.sym == XKB_KEY_plus);
	CHECK(tp_binding_parse_chord("Logo+Esc", &spec, err, sizeof(err)));
	CHECK(spec.mods == TP_MOD_META && spec.sym == XKB_KEY_Escape);
	CHECK(tp_binding_parse_chord("Mod4+Space", &spec, err, sizeof(err)));
	CHECK(spec.mods == TP_MOD_META && spec.sym == XKB_KEY_space);
	CHECK(tp_binding_parse_chord("XF86AudioRaiseVolume", &spec, err,
								 sizeof(err)));
	CHECK(spec.mods == 0 && spec.sym == XKB_KEY_XF86AudioRaiseVolume);

	/* Invalid chords must fail cleanly with a diagnostic. */
	const char *bad[] = {
		"",		 "   ",		  "Meta+",			"+a",
		"++",	 "Meta++a",	  "Bogus+Return",	"Meta+NotARealKeySym",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		err[0] = '\0';
		CHECK(!tp_binding_parse_chord(bad[i], &spec, err, sizeof(err)));
		CHECK(err[0] != '\0');
	}
}

static void test_default_bindings(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_action_fn(lua, mock_action, NULL);

	/* 17 core + 12 directional + 36 tag defaults = 65 bindings. */
	CHECK(tp_lua_binding_count(lua) == 65);
	CHECK(!tp_lua_has_unmodified_bindings(lua));

	action_count = 0;
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_Return, false));
	CHECK(action_count == 1 && last_action.kind == TP_ACT_TERMINAL);

	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_r, false));
	CHECK(action_count == 2 && last_action.kind == TP_ACT_RELOAD);

	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_w, false));
	CHECK(last_action.kind == TP_ACT_CLOSE);

	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_e, false));
	CHECK(last_action.kind == TP_ACT_QUIT);

	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_j, false));
	CHECK(last_action.kind == TP_ACT_FOCUS && last_action.dir == +1);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_k, false));
	CHECK(last_action.kind == TP_ACT_FOCUS && last_action.dir == -1);

	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_v, false));
	CHECK(last_action.kind == TP_ACT_TOGGLE_FLOAT);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_f, false));
	CHECK(last_action.kind == TP_ACT_TOGGLE_FULLSCREEN);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_Tab, false));
	CHECK(last_action.kind == TP_ACT_TAG_BACK);

	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_comma, false));
	CHECK(last_action.kind == TP_ACT_MONITOR_FOCUS && last_action.dir == -1);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_period, false));
	CHECK(last_action.kind == TP_ACT_MONITOR_MOVE && last_action.dir == +1);

	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_Left, false));
	CHECK(last_action.kind == TP_ACT_SWAP && last_action.dir == TP_DIR_LEFT);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_CTRL, XKB_KEY_Right, false));
	CHECK(last_action.kind == TP_ACT_MOVE && last_action.dir == TP_DIR_RIGHT);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_ALT, XKB_KEY_Up, false));
	CHECK(last_action.kind == TP_ACT_RESIZE && last_action.dir == TP_DIR_UP &&
		  last_action.step == 0);

	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_3, false));
	CHECK(last_action.kind == TP_ACT_TAG_VIEW && last_action.tag == 3);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_5, false));
	CHECK(last_action.kind == TP_ACT_TAG_MOVE && last_action.tag == 5);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_CTRL, XKB_KEY_7, false));
	CHECK(last_action.kind == TP_ACT_TAG_TOGGLE_VIEW && last_action.tag == 7);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT | TP_MOD_CTRL, XKB_KEY_9, false));
	CHECK(last_action.kind == TP_ACT_TAG_TOGGLE_CLIENT &&
		  last_action.tag == 9);

	/* Unbound key returns false. */
	CHECK(!tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_z, false));

	tp_lua_destroy(lua);
}

static void test_custom_bindings_and_callbacks(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_spawn_fn(lua, mock_spawn);
	tp_lua_set_spawn_argv_fn(lua, mock_spawn_argv);
	tp_lua_set_action_fn(lua, mock_action, NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	out = base;
	char err[256] = {0};

	const char *cfg =
		"tp.bind('Meta+Return', tp.dsp.spawn('kitty'))\n"
		"tp.bind('Meta+Alt+Down', tp.dsp.resize('down', 80))\n"
		"tp.bind('XF86AudioRaiseVolume', tp.dsp.spawn('vol-up'))\n"
		"assert(tp.unbind('Meta+v') == true)\n"
		"assert(tp.unbind('Meta+v') == false)\n"
		"tp.bind('Meta+x', function()\n"
		"  tp.dsp.tag('view', 2)()\n"
		"  tp.spawn('from-callback')\n"
		"end)\n"
		"tp.bind('Meta+y', function() error('callback boom') end)\n"
		"tp.bind('Meta+z', function() while true do end end)\n"
		"tp.bind('Meta+a', function()\n"
		"  tp.spawn{'printf', '%s\\n', \"it's; rm -rf ~ #\"}\n"
		"  local many = {}\n"
		"  for i = 1, 65 do many[i] = 'x' end\n"
		"  for _, bad in ipairs({ {}, {''}, {'a', 5}, {'a\\0b'}, many }) do\n"
		"    tp.spawn(pcall(tp.spawn, bad) and 'bad-accepted' or 'bad-rejected')\n"
		"  end\n"
		"end)\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "custom_binds",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(tp_lua_has_unmodified_bindings(lua));

	/* Overridden dispatcher + explicit resize step + unmodified key. */
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_Return, false));
	CHECK(last_action.kind == TP_ACT_SPAWN &&
		  strcmp(last_action.cmd, "kitty") == 0);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_ALT, XKB_KEY_Down, false));
	CHECK(last_action.kind == TP_ACT_RESIZE &&
		  last_action.dir == TP_DIR_DOWN && last_action.step == 80);
	CHECK(tp_lua_handle_key(lua, 0, XKB_KEY_XF86AudioRaiseVolume, false));
	CHECK(last_action.kind == TP_ACT_SPAWN &&
		  strcmp(last_action.cmd, "vol-up") == 0);
	CHECK(!tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_v, false));

	/* Plain Lua callback invoking a dispatcher and tp.spawn. */
	spawn_count = 0;
	last_spawned[0] = '\0';
	action_count = 0;
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_x, false));
	CHECK(action_count == 1 && last_action.kind == TP_ACT_TAG_VIEW &&
		  last_action.tag == 2);
	CHECK(spawn_count == 1 && strcmp(last_spawned, "from-callback") == 0);

	/* tp.spawn{...} passes each argument to the program as is, with no
	 * shell, and rejects malformed argument lists. */
	reset_spawn_log();
	argv_spawn_count = 0;
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_a, false));
	CHECK(argv_spawn_count == 1);
	CHECK(strcmp(last_argv, "printf|%s\n|it's; rm -rf ~ #") == 0);
	CHECK(spawn_log_count == 5);
	for (int i = 0; i < spawn_log_count; i++)
		CHECK(strcmp(spawn_log[i], "bad-rejected") == 0);

	/* Callback runtime error and runaway loop are contained cleanly. */
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_y, false));
	uint64_t t0 = now_ms();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_z, false));
	CHECK(now_ms() - t0 < 500);

	/* Interpreter and bindings still work after callback aborts. */
	action_count = 0;
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_x, false));
	CHECK(action_count == 1);

	/* Atomic rollback: clear_binds + error keeps previous bindings & refs. */
	size_t before_count = tp_lua_binding_count(lua);
	const char *bad_clear =
		"tp.clear_binds()\n"
		"tp.bind('Meta+q', tp.dsp.close())\n"
		"error('abort after clear')\n";
	CHECK(!tp_lua_exec_string(lua, bad_clear, strlen(bad_clear), "rollback",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	CHECK(tp_lua_binding_count(lua) == before_count);
	CHECK(!tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_q, false));
	action_count = 0;
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_x, false));
	CHECK(action_count == 1);

	/* Invalid dispatcher / bind calls fail cleanly. */
	const char *bad_binds[] = {
		"tp.bind('BadMod+Return', tp.dsp.close())",
		"tp.bind('Meta+Return', 123)",
		"tp.dsp.spawn('')",
		"tp.dsp.focus('sideways')",
		"tp.dsp.swap('diagonal')",
		"tp.dsp.resize('left', 0)",
		"tp.dsp.tag('view', 10)",
		"tp.dsp.tag('nope', 1)",
		"tp.dsp.monitor('jump', 'next')",
		"tp.dsp.close()()",
	};
	for (size_t i = 0; i < sizeof(bad_binds) / sizeof(bad_binds[0]); i++) {
		CHECK(!tp_lua_exec_string(lua, bad_binds[i], strlen(bad_binds[i]),
								  "bad_bind", TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
	}

	/* clear_binds + single custom binding succeeds. */
	const char *minimal =
		"tp.clear_binds()\n"
		"tp.bind('Meta+q', tp.dsp.close())\n";
	CHECK(tp_lua_exec_string(lua, minimal, strlen(minimal), "minimal",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(tp_lua_binding_count(lua) == 1);
	CHECK(!tp_lua_has_unmodified_bindings(lua));
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_q, false));
	CHECK(last_action.kind == TP_ACT_CLOSE);

	tp_lua_destroy(lua);
}

static void test_event_hooks(void) {
	/* Event name round-trips and invalid names. */
	for (unsigned int i = 0; i < TP_EVENT_COUNT; i++) {
		const char *name = tp_event_kind_name((enum tp_event_kind)i);
		CHECK(name != NULL);
		enum tp_event_kind parsed = TP_EVENT_COUNT;
		CHECK(tp_event_kind_from_name(name, &parsed));
		CHECK(parsed == (enum tp_event_kind)i);
	}
	enum tp_event_kind bad_kind = TP_EVENT_COUNT;
	CHECK(tp_event_kind_name(TP_EVENT_COUNT) == NULL);
	CHECK(!tp_event_kind_from_name("window.bogus", &bad_kind));
	CHECK(!tp_event_kind_from_name("", &bad_kind));

	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_spawn_fn(lua, mock_spawn);
	tp_lua_set_action_fn(lua, mock_action, NULL);

	CHECK(tp_lua_hook_count(lua, TP_EVENT_COUNT) == 0);
	CHECK(!tp_lua_has_hooks(lua, TP_EVENT_WINDOW_OPEN));

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	out = base;
	char err[256] = {0};

	/* Register hooks for all 6 events, test ordering, table isolation,
	 * tp.off(id), and tp.clear_hooks(event). */
	const char *cfg =
		"local id1 = tp.on('window.open', function(ev)\n"
		"  tp.spawn(string.format('open1:%s:%s:%s:%s:%d:%d:%s:%s:%d,%d,%d,%d',\n"
		"    ev.event, ev.app_id, ev.title, ev.monitor, ev.tags, ev.tag,\n"
		"    tostring(ev.floating), tostring(ev.fullscreen), ev.x, ev.y, ev.w, ev.h))\n"
		"  ev.app_id = 'mutated'\n"
		"end)\n"
		"local id2 = tp.on('window.open', function(ev)\n"
		"  tp.spawn('open2:' .. ev.app_id)\n"
		"end)\n"
		"local id3 = tp.on('window.open', function(ev)\n"
		"  tp.spawn('open3:should-be-removed')\n"
		"end)\n"
		"assert(type(id1) == 'number' and id2 > id1 and id3 > id2)\n"
		"assert(tp.off(id3) == true)\n"
		"assert(tp.off(id3) == false)\n"
		"assert(tp.off(99999) == false)\n"
		"tp.on('window.close', function(ev)\n"
		"  tp.spawn('close:' .. ev.event .. ':' .. ev.app_id .. ':' .. ev.title)\n"
		"end)\n"
		"tp.on('window.focus', function(ev)\n"
		"  tp.spawn(string.format('focus:%s:%s:%s:%s',\n"
		"    ev.event, tostring(ev.focused), ev.app_id, ev.monitor))\n"
		"end)\n"
		"tp.on('tag.change', function(ev)\n"
		"  tp.spawn(string.format('tag:%s:%s:%d->%d:%d',\n"
		"    ev.event, ev.monitor, ev.prev_tags, ev.tags, ev.tag))\n"
		"end)\n"
		"tp.on('monitor.add', function(ev)\n"
		"  tp.spawn(string.format('madd:%s:%s:%d,%d,%d,%d:%.2f',\n"
		"    ev.event, ev.name, ev.x, ev.y, ev.w, ev.h, ev.scale))\n"
		"end)\n"
		"tp.on('monitor.remove', function(ev)\n"
		"  tp.spawn(string.format('mrem:%s:%s:%d,%d,%d,%d:%.2f',\n"
		"    ev.event, ev.name, ev.x, ev.y, ev.w, ev.h, ev.scale))\n"
		"end)\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "hooks",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(tp_lua_has_hooks(lua, TP_EVENT_WINDOW_OPEN));
	CHECK(tp_lua_hook_count(lua, TP_EVENT_WINDOW_OPEN) == 2);
	CHECK(tp_lua_hook_count(lua, TP_EVENT_COUNT) == 7);

	/* window.open: ordering and fresh table isolation. */
	reset_spawn_log();
	struct tp_event ev = {
		.kind = TP_EVENT_WINDOW_OPEN,
		.window = {
			.app_id = "foot",
			.title = "shell",
			.monitor = "DP-1",
			.tags = tp_tag_mask(2) | tp_tag_mask(4),
			.floating = true,
			.fullscreen = false,
			.geom = {10, 20, 800, 600},
		},
	};
	CHECK(tp_lua_emit_event(lua, &ev) == 2);
	CHECK(spawn_log_count == 2);
	CHECK(strcmp(spawn_log[0],
				 "open1:window.open:foot:shell:DP-1:10:2:true:false:10,20,800,600") == 0);
	CHECK(strcmp(spawn_log[1], "open2:foot") == 0);

	/* window.close */
	reset_spawn_log();
	ev.kind = TP_EVENT_WINDOW_CLOSE;
	CHECK(tp_lua_emit_event(lua, &ev) == 1);
	CHECK(spawn_log_count == 1 &&
		  strcmp(spawn_log[0], "close:window.close:foot:shell") == 0);

	/* window.focus (focused = true and focused = false) */
	reset_spawn_log();
	ev.kind = TP_EVENT_WINDOW_FOCUS;
	ev.window.focused = true;
	CHECK(tp_lua_emit_event(lua, &ev) == 1);
	CHECK(strcmp(spawn_log[0], "focus:window.focus:true:foot:DP-1") == 0);

	struct tp_event ev_clear = {
		.kind = TP_EVENT_WINDOW_FOCUS,
		.window = {.monitor = "DP-1", .focused = false},
	};
	CHECK(tp_lua_emit_event(lua, &ev_clear) == 1);
	CHECK(strcmp(spawn_log[1], "focus:window.focus:false::DP-1") == 0);

	/* tag.change */
	reset_spawn_log();
	struct tp_event ev_tag = {
		.kind = TP_EVENT_TAG_CHANGE,
		.tag = {.monitor = "DP-1",
				.prev_tags = tp_tag_mask(1),
				.tags = tp_tag_mask(3)},
	};
	CHECK(tp_lua_emit_event(lua, &ev_tag) == 1);
	CHECK(strcmp(spawn_log[0], "tag:tag.change:DP-1:1->4:3") == 0);

	/* monitor.add and monitor.remove */
	reset_spawn_log();
	struct tp_event ev_mon = {
		.kind = TP_EVENT_MONITOR_ADD,
		.monitor = {.name = "HDMI-A-1", .area = {1920, 0, 1280, 720}, .scale = 1.5f},
	};
	CHECK(tp_lua_emit_event(lua, &ev_mon) == 1);
	CHECK(strcmp(spawn_log[0], "madd:monitor.add:HDMI-A-1:1920,0,1280,720:1.50") == 0);
	ev_mon.kind = TP_EVENT_MONITOR_REMOVE;
	CHECK(tp_lua_emit_event(lua, &ev_mon) == 1);
	CHECK(strcmp(spawn_log[1], "mrem:monitor.remove:HDMI-A-1:1920,0,1280,720:1.50") == 0);

	/* Error & watchdog isolation: throwing hook and infinite loop do not
	 * prevent subsequent hooks from running. */
	const char *iso_cfg =
		"tp.on('window.open', function() error('hook boom') end)\n"
		"tp.on('window.open', function() while true do end end)\n"
		"tp.on('window.open', function(e) tp.spawn('survived:' .. e.app_id) end)\n";
	CHECK(tp_lua_exec_string(lua, iso_cfg, strlen(iso_cfg), "iso",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	/* Reload replaced previous hooks cleanly (no duplicates from earlier cfg). */
	CHECK(tp_lua_hook_count(lua, TP_EVENT_COUNT) == 3);
	CHECK(!tp_lua_has_hooks(lua, TP_EVENT_TAG_CHANGE));

	reset_spawn_log();
	uint64_t t0 = now_ms();
	ev.kind = TP_EVENT_WINDOW_OPEN;
	CHECK(tp_lua_emit_event(lua, &ev) == 3);
	CHECK(now_ms() - t0 < 500);
	CHECK(spawn_log_count == 1 && strcmp(spawn_log[0], "survived:foot") == 0);

	/* Atomic rollback: failed config keeps previous active hooks intact. */
	const char *bad_reload =
		"tp.on('tag.change', function() tp.spawn('never') end)\n"
		"error('rollback hooks')\n";
	CHECK(!tp_lua_exec_string(lua, bad_reload, strlen(bad_reload), "bad_reload",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	CHECK(tp_lua_hook_count(lua, TP_EVENT_COUNT) == 3);
	CHECK(!tp_lua_has_hooks(lua, TP_EVENT_TAG_CHANGE));
	CHECK(tp_lua_has_hooks(lua, TP_EVENT_WINDOW_OPEN));

	/* tp.clear_hooks(event) and tp.clear_hooks() */
	const char *clear_cfg =
		"tp.on('window.open', function() tp.spawn('a') end)\n"
		"tp.on('tag.change', function() tp.spawn('b') end)\n"
		"tp.clear_hooks('window.open')\n";
	CHECK(tp_lua_exec_string(lua, clear_cfg, strlen(clear_cfg), "clear_one",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(!tp_lua_has_hooks(lua, TP_EVENT_WINDOW_OPEN));
	CHECK(tp_lua_hook_count(lua, TP_EVENT_TAG_CHANGE) == 1);

	/* Validation errors: unknown event, non-function, bad off arg, runtime call. */
	const char *bad_hooks[] = {
		"tp.on('nope.event', function() end)",
		"tp.on('window.open', 'not-a-fn')",
		"tp.off('not-an-int')",
		"tp.clear_hooks('nope.event')",
	};
	for (size_t i = 0; i < sizeof(bad_hooks) / sizeof(bad_hooks[0]); i++) {
		CHECK(!tp_lua_exec_string(lua, bad_hooks[i], strlen(bad_hooks[i]),
								  "bad_hook", TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
	}

	/* Calling tp.on / tp.off / tp.clear_hooks at runtime inside a hook fails. */
	const char *rt_cfg =
		"tp.on('window.open', function()\n"
		"  local ok1 = pcall(tp.on, 'window.close', function() end)\n"
		"  local ok2 = pcall(tp.off, 1)\n"
		"  local ok3 = pcall(tp.clear_hooks)\n"
		"  tp.spawn(tostring(ok1) .. ':' .. tostring(ok2) .. ':' .. tostring(ok3))\n"
		"end)\n";
	CHECK(tp_lua_exec_string(lua, rt_cfg, strlen(rt_cfg), "rt",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	reset_spawn_log();
	CHECK(tp_lua_emit_event(lua, &ev) == 1);
	CHECK(spawn_log_count == 1 &&
		  strcmp(spawn_log[0], "false:false:false") == 0);

	tp_lua_destroy(lua);
}

/* tp.on('startup', fn) is where autostart belongs: the hook may spawn,
 * and it runs when the compositor delivers the one startup event. */
static void test_startup_hook(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_spawn_fn(lua, mock_spawn);
	tp_lua_set_spawn_argv_fn(lua, mock_spawn_argv);
	struct tp_settings base, out;
	tp_settings_defaults(&base);
	char err[256] = {0};
	const char *cfg =
		"tp.on('startup', function(ev)\n"
		"  tp.spawn('started:' .. ev.event)\n"
		"  tp.spawn{'waybar', '-c', 'bar.json'}\n"
		"end)\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "startup",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(tp_lua_has_hooks(lua, TP_EVENT_STARTUP));
	reset_spawn_log();
	argv_spawn_count = 0;
	struct tp_event ev = {.kind = TP_EVENT_STARTUP};
	CHECK(tp_lua_emit_event(lua, &ev) == 1);
	CHECK(spawn_count == 1 && strcmp(last_spawned, "started:startup") == 0);
	CHECK(argv_spawn_count == 1 && strcmp(last_argv, "waybar|-c|bar.json") == 0);
	tp_lua_destroy(lua);
}

/* A pathological pattern used to run in C for hours, past the watchdog. */
static void on_hang_alarm(int sig) {
	(void)sig;
	static const char msg[] = "test_lua: hung in Lua pattern matching\n";
	if (write(2, msg, sizeof(msg) - 1) < 0) {
	}
	_exit(1);
}

/* Work that Lua does inside C library calls stays within the limits:
 * memory is capped, and pattern matching obeys the wall-clock deadline. */
static void test_memory_and_pattern_limits(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_spawn_fn(lua, mock_spawn);
	struct tp_settings base, out;
	tp_settings_defaults(&base);
	char err[256] = {0};

	/* Doubling a string reaches 64 MiB in 26 steps: refused, reported as
	 * the memory limit, and the state still works afterwards. */
	const char *grow = "local s = 'x'\nwhile true do s = s .. s end\n";
	CHECK(!tp_lua_exec_string(lua, grow, strlen(grow), "grow",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	CHECK(strstr(err, "memory limit") != NULL);
	const char *big_rep = "local s = string.rep('x', 1 << 30)\n";
	CHECK(!tp_lua_exec_string(lua, big_rep, strlen(big_rep), "rep",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	CHECK(strstr(err, "memory limit") != NULL);

	signal(SIGALRM, on_hang_alarm);
	alarm(20);
	const char *cfg =
		"local subject = string.rep('a', 255)\n"
		"tp.bind('Meta+p', function()\n"
		"  string.find(subject, '.-.-.-.-.-.-b')\n"
		"  tp.spawn('not-reached')\n"
		"end)\n"
		"tp.bind('Meta+g', function()\n"
		"  local n = select(2, string.rep('ab', 100000):gsub('a', 'c'))\n"
		"  tp.spawn('gsub:' .. n)\n"
		"end)\n"
		"tp.rule{ match = { title = '.-.-.-.-.-.-b' }, floating = true }\n"
		"tp.rule{ match = { title = 'TAIL' }, tag = 2 }\n"
		"tp.rule{ match = { title = 'a$' }, tag = 3 }\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "limits",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));

	/* A runaway pattern in a callback stops at the 100 ms deadline. */
	reset_spawn_log();
	uint64_t t0 = now_ms();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_p, false));
	CHECK(now_ms() - t0 < 1000);
	CHECK(spawn_count == 0);

	/* Long legitimate matching within the deadline still runs. */
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_g, false));
	CHECK(spawn_count == 1 && strcmp(last_spawned, "gsub:100000") == 0);

	/* Rules: the runaway rule is cut off and does not match. Subjects are
	 * cut to TP_LUA_RULE_SUBJECT_MAX bytes, so a long title's tail is not
	 * seen but its first 255 bytes are. */
	char title[4096];
	memset(title, 'a', sizeof(title) - 5);
	memcpy(title + sizeof(title) - 5, "TAIL", 5);
	struct tp_rule_result res;
	t0 = now_ms();
	CHECK(tp_lua_eval_rules(lua, "app", title, &res));
	CHECK(now_ms() - t0 < 1000);
	CHECK(!res.has_floating);
	CHECK(res.has_tags && res.tags == tp_tag_mask(3));
	alarm(0);
	tp_lua_destroy(lua);
}

static void test_window_rules(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_spawn_fn(lua, mock_spawn);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	out = base;
	char err[256] = {0};

	/* Initially no rules; eval fast-path returns false. */
	struct tp_rule_result res;
	CHECK(tp_lua_rule_count(lua) == 0);
	CHECK(!tp_lua_eval_rules(lua, "foot", "shell", &res));
	CHECK(!res.matched);

	/* Pattern matching, composition, later-rule precedence, remove_rule,
	 * array & keyed size/position, center, tag vs tags, monitor, floating. */
	const char *cfg =
		"local r1 = tp.rule{\n"
		"  match = { app_id = '^foot$' },\n"
		"  tag = 2,\n"
		"  size = { 800, 500 },\n"
		"}\n"
		"local r2 = tp.rule{\n"
		"  match = { app_id = '^foot$', title = 'float%-%d+' },\n"
		"  floating = true,\n"
		"  position = { 40, 60 },\n"
		"  monitor = 'DP-2',\n"
		"}\n"
		"local r_drop = tp.rule{\n"
		"  match = { app_id = '^foot$' },\n"
		"  tag = 9,\n"
		"}\n"
		"assert(r1 == 1 and r2 == 2 and r_drop == 3)\n"
		"assert(tp.remove_rule(r_drop) == true)\n"
		"assert(tp.remove_rule(999) == false)\n"
		"tp.rule{\n"
		"  match = { title = '^Dialog:' },\n"
		"  tags = { 1, 3, 5 },\n"
		"  floating = false,\n"
		"  size = { w = 640, h = 480 },\n"
		"  position = 'center',\n"
		"}\n"
		"tp.rule{\n"
		"  match = { app_id = 'pavu.*' },\n"
		"  floating = true,\n"
		"  position = { x = -10, y = 25 },\n"
		"}\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "rules_cfg",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(tp_lua_rule_count(lua) == 4);

	/* Rule 1 only: app_id='foot', title='shell'. */
	CHECK(tp_lua_eval_rules(lua, "foot", "shell", &res));
	CHECK(res.matched);
	CHECK(res.has_tags && res.tags == tp_tag_mask(2));
	CHECK(!res.has_floating);
	CHECK(res.has_size && res.width == 800 && res.height == 500);
	CHECK(res.pos_mode == TP_RULE_POS_NONE);
	CHECK(!res.has_monitor);

	/* Rules 1 + 2 compose: tag & size from r1, floating/pos/monitor from r2. */
	CHECK(tp_lua_eval_rules(lua, "foot", "float-42", &res));
	CHECK(res.matched);
	CHECK(res.has_tags && res.tags == tp_tag_mask(2));
	CHECK(res.has_floating && res.floating == true);
	CHECK(res.has_size && res.width == 800 && res.height == 500);
	CHECK(res.pos_mode == TP_RULE_POS_COORDS && res.x == 40 && res.y == 60);
	CHECK(res.has_monitor && strcmp(res.monitor, "DP-2") == 0);

	/* Rule 3: multi-tag bitmask, floating=false, keyed size, position='center'. */
	CHECK(tp_lua_eval_rules(lua, "other", "Dialog: Save", &res));
	CHECK(res.matched);
	CHECK(res.has_tags &&
		  res.tags == (tp_tag_mask(1) | tp_tag_mask(3) | tp_tag_mask(5)));
	CHECK(res.has_floating && res.floating == false);
	CHECK(res.has_size && res.width == 640 && res.height == 480);
	CHECK(res.pos_mode == TP_RULE_POS_CENTER);

	/* Rule 4: keyed position with negative x, NULL title handled safely. */
	CHECK(tp_lua_eval_rules(lua, "pavucontrol", NULL, &res));
	CHECK(res.matched);
	CHECK(res.has_floating && res.floating == true);
	CHECK(res.pos_mode == TP_RULE_POS_COORDS && res.x == -10 && res.y == 25);

	/* Non-matching window returns false (NULL app_id & title safe). */
	CHECK(!tp_lua_eval_rules(lua, "firefox", "Home", &res));
	CHECK(!res.matched);
	CHECK(!tp_lua_eval_rules(lua, NULL, NULL, &res));

	/* Atomic rollback: failed config keeps previous 4 active rules intact. */
	const char *bad_reload =
		"tp.rule{ match = { app_id = 'firefox' }, tag = 4 }\n"
		"error('rollback rules')\n";
	CHECK(!tp_lua_exec_string(lua, bad_reload, strlen(bad_reload), "bad_rules",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	CHECK(tp_lua_rule_count(lua) == 4);
	CHECK(!tp_lua_eval_rules(lua, "firefox", "Home", &res));
	CHECK(tp_lua_eval_rules(lua, "foot", "shell", &res));

	/* User shadowing global string.find does not break rule matching. */
	const char *shadow_cfg =
		"string.find = function() error('shadowed') end\n"
		"tp.rule{ match = { app_id = '^kitty$' }, tag = 3 }\n";
	CHECK(tp_lua_exec_string(lua, shadow_cfg, strlen(shadow_cfg), "shadow",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(tp_lua_eval_rules(lua, "kitty", "term", &res));
	CHECK(res.has_tags && res.tags == tp_tag_mask(3));

	/* tp.clear_rules() empties staged rules. */
	const char *clear_cfg =
		"tp.rule{ match = { app_id = 'a' }, tag = 1 }\n"
		"tp.clear_rules()\n";
	CHECK(tp_lua_exec_string(lua, clear_cfg, strlen(clear_cfg), "clear_rules",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(tp_lua_rule_count(lua) == 0);

	/* Validation errors: bad specs, malformed Lua patterns, conflicts. */
	const char *bad_rules[] = {
		"tp.rule('not-a-table')",
		"tp.rule{ tag = 1 }",
		"tp.rule{ match = {}, tag = 1 }",
		"tp.rule{ match = { app_id = '' }, tag = 1 }",
		"tp.rule{ match = { appid = 'foot' }, tag = 1 }",
		"tp.rule{ match = { app_id = '[unclosed' }, tag = 1 }",
		"tp.rule{ match = { title = '%b' }, tag = 1 }",
		"tp.rule{ match = { app_id = 'foot' } }",
		"tp.rule{ match = { app_id = 'foot' }, tag = 0 }",
		"tp.rule{ match = { app_id = 'foot' }, tag = 10 }",
		"tp.rule{ match = { app_id = 'foot' }, tags = 2 }",
		"tp.rule{ match = { app_id = 'foot' }, tags = {} }",
		"tp.rule{ match = { app_id = 'foot' }, tags = { 1, 10 } }",
		"tp.rule{ match = { app_id = 'foot' }, tag = 1, tags = { 2 } }",
		"tp.rule{ match = { app_id = 'foot' }, floating = 'yes' }",
		"tp.rule{ match = { app_id = 'foot' }, size = { 32, 200 } }",
		"tp.rule{ match = { app_id = 'foot' }, size = { 200 } }",
		"tp.rule{ match = { app_id = 'foot' }, size = { 200, h = 200 } }",
		"tp.rule{ match = { app_id = 'foot' }, position = 'top-left' }",
		"tp.rule{ match = { app_id = 'foot' }, position = { 10 } }",
		"tp.rule{ match = { app_id = 'foot' }, monitor = '' }",
		"tp.rule{ match = { app_id = 'foot' }, float = true }",
		"tp.remove_rule('bad')",
	};
	for (size_t i = 0; i < sizeof(bad_rules) / sizeof(bad_rules[0]); i++) {
		CHECK(!tp_lua_exec_string(lua, bad_rules[i], strlen(bad_rules[i]),
								  "bad_rule", TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
	}

	/* Calling tp.rule / tp.remove_rule / tp.clear_rules at runtime fails. */
	const char *rt_cfg =
		"tp.on('window.open', function()\n"
		"  local ok1 = pcall(tp.rule, { match = { app_id = 'a' }, tag = 1 })\n"
		"  local ok2 = pcall(tp.remove_rule, 1)\n"
		"  local ok3 = pcall(tp.clear_rules)\n"
		"  tp.spawn(tostring(ok1) .. ':' .. tostring(ok2) .. ':' .. tostring(ok3))\n"
		"end)\n";
	CHECK(tp_lua_exec_string(lua, rt_cfg, strlen(rt_cfg), "rt_rules",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	struct tp_event ev = { .kind = TP_EVENT_WINDOW_OPEN };
	reset_spawn_log();
	CHECK(tp_lua_emit_event(lua, &ev) == 1);
	CHECK(spawn_log_count == 1 &&
		  strcmp(spawn_log[0], "false:false:false") == 0);

	tp_lua_destroy(lua);
}

static void test_live_reload(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_spawn_fn(lua, mock_spawn);
	tp_lua_set_action_fn(lua, mock_action, NULL);

	char dir[] = "/tmp/tam-pane-reload-test.XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char cfg_path[256], fifo_path[256], missing_path[256];
	snprintf(cfg_path, sizeof(cfg_path), "%s/init.lua", dir);
	snprintf(fifo_path, sizeof(fifo_path), "%s/pipe", dir);
	snprintf(missing_path, sizeof(missing_path), "%s/missing.lua", dir);
	CHECK(mkfifo(fifo_path, 0600) == 0);

	struct tp_settings cur;
	tp_settings_defaults(&cur);
	char err[256] = {0};

	/* 1) Initial reload loads settings, custom binding, hook, rule, and global. */
	const char *v1 =
		"my_global = 'from_v1'\n"
		"tp.config{ gap_inner = 18, gap_outer = 24, repeat_rate = 50 }\n"
		"tp.bind('Meta+x', function()\n"
		"  tp.spawn('v1:' .. tostring(my_global))\n"
		"end)\n"
		"tp.bind('Meta+Shift+y', function()\n"
		"  tp.dsp.reload()()\n"
		"end)\n"
		"tp.on('window.open', function(ev) tp.spawn('h1:' .. ev.app_id) end)\n"
		"tp.rule{ match = { app_id = '^alpha$' }, tag = 4, floating = true }\n";
	CHECK(write_text(cfg_path, v1));
	CHECK(tp_lua_reload_file(&lua, cfg_path, &cur, err, sizeof(err)));
	CHECK(cur.gap_inner == 18 && cur.gap_outer == 24 && cur.repeat_rate == 50);
	CHECK(tp_lua_binding_count(lua) == 67);
	CHECK(tp_lua_hook_count(lua, TP_EVENT_WINDOW_OPEN) == 1);
	CHECK(tp_lua_rule_count(lua) == 1);

	/* Spawn and action handlers survived the swap. */
	reset_spawn_log();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_x, false));
	CHECK(spawn_log_count == 1 && strcmp(spawn_log[0], "v1:from_v1") == 0);
	action_count = 0;
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_y, false));
	CHECK(action_count == 1 && last_action.kind == TP_ACT_RELOAD);

	/* 2) Second reload replaces config cleanly from fresh defaults:
	 * removed gap_outer reverts to default (8), removed Meta+x is gone,
	 * stale my_global is nil, old hook/rule replaced. */
	const char *v2 =
		"tp.config{ gap_inner = 4 }\n"
		"tp.bind('Meta+z', function()\n"
		"  tp.spawn('v2:' .. tostring(my_global))\n"
		"end)\n"
		"tp.on('window.open', function(ev) tp.spawn('h2:' .. ev.app_id) end)\n"
		"tp.rule{ match = { app_id = '^beta$' }, tag = 2 }\n";
	CHECK(write_text(cfg_path, v2));
	CHECK(tp_lua_reload_file(&lua, cfg_path, &cur, err, sizeof(err)));
	struct tp_settings defs;
	tp_settings_defaults(&defs);
	CHECK(cur.gap_inner == 4);
	CHECK(cur.gap_outer == defs.gap_outer);
	CHECK(cur.repeat_rate == defs.repeat_rate);
	CHECK(tp_lua_binding_count(lua) == 66);
	CHECK(!tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_x, false));
	reset_spawn_log();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_z, false));
	CHECK(spawn_log_count == 1 && strcmp(spawn_log[0], "v2:nil") == 0);
	struct tp_rule_result rr;
	CHECK(!tp_lua_eval_rules(lua, "alpha", "", &rr));
	CHECK(tp_lua_eval_rules(lua, "beta", "", &rr) && rr.has_tags &&
		  rr.tags == tp_tag_mask(2));
	struct tp_event ev = {.kind = TP_EVENT_WINDOW_OPEN};
	snprintf(ev.window.app_id, sizeof(ev.window.app_id), "foot");
	reset_spawn_log();
	CHECK(tp_lua_emit_event(lua, &ev) == 1);
	CHECK(spawn_log_count == 1 && strcmp(spawn_log[0], "h2:foot") == 0);

	/* 3) Broken reloads fail atomically and preserve v2 completely. */
	const char *broken[] = {
		"tp.config{ gaps = ",
		"tp.config{ gaps = 99 }\nerror('reload-boom')\n",
		"tp.config{ gap_inner = -5 }\n",
		"tp.bind('Bogus+a', tp.dsp.close())\n",
		"tp.rule{ match = { app_id = '[broken' }, tag = 1 }\n",
		"while true do end\n",
		"\x1bLua\x55\x00",
	};
	for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
		CHECK(write_text(cfg_path, broken[i]));
		err[0] = '\0';
		CHECK(!tp_lua_reload_file(&lua, cfg_path, &cur, err, sizeof(err)));
		CHECK(err[0] != '\0');
		CHECK(cur.gap_inner == 4);
		CHECK(cur.gap_outer == defs.gap_outer);
		CHECK(tp_lua_binding_count(lua) == 66);
		CHECK(tp_lua_rule_count(lua) == 1);
		CHECK(tp_lua_hook_count(lua, TP_EVENT_WINDOW_OPEN) == 1);
	}

	/* Missing file and FIFO fail cleanly without blocking or losing v2. */
	err[0] = '\0';
	CHECK(!tp_lua_reload_file(&lua, missing_path, &cur, err, sizeof(err)));
	CHECK(strstr(err, "not found") != NULL);
	CHECK(cur.gap_inner == 4);

	err[0] = '\0';
	CHECK(!tp_lua_reload_file(&lua, fifo_path, &cur, err, sizeof(err)));
	CHECK(strstr(err, "not a regular file") != NULL);
	CHECK(cur.gap_inner == 4);

	/* v2 binding and hook still work after all failed reloads. */
	reset_spawn_log();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_z, false));
	CHECK(spawn_log_count == 1 && strcmp(spawn_log[0], "v2:nil") == 0);

	unlink(cfg_path);
	unlink(fifo_path);
	rmdir(dir);
	tp_lua_destroy(lua);
}

static char lock_log[8][128];
static size_t lock_log_count;

static void reset_lock_log(void) { lock_log_count = 0; }

static void mock_lock(const char *cmd, void *userdata) {
	(void)userdata;
	if (lock_log_count < 8)
		snprintf(lock_log[lock_log_count++], 128, "sh:%s", cmd);
}

static void mock_lock_argv(const char *const argv[], void *userdata) {
	(void)userdata;
	if (lock_log_count >= 8)
		return;
	char *dst = lock_log[lock_log_count++];
	size_t pos = (size_t)snprintf(dst, 128, "argv:");
	for (size_t i = 0; argv[i] && pos + 2 < 128; i++)
		pos += (size_t)snprintf(dst + pos, 128 - pos, "%s%s", i ? "|" : "",
								argv[i]);
}

static void test_lock_and_binding_gate(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_spawn_fn(lua, mock_spawn);
	tp_lua_set_action_fn(lua, mock_action, NULL);
	tp_lua_set_lock_fn(lua, mock_lock, NULL);
	tp_lua_set_lock_argv_fn(lua, mock_lock_argv, NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	char err[256] = {0};

	const char *cfg =
		"tp.bind('Meta+a', function() tp.spawn('normal-a') end)\n"
		"tp.bind('Meta+b', function() tp.spawn('locked-b') end, { locked = true })\n"
		"tp.bind('Meta+c', function() tp.spawn('explicit-unlocked-c') end, { locked = false })\n"
		"tp.bind('Meta+l', function() tp.lock{'hyprlock', '--immediate'} end)\n"
		"tp.bind('Meta+Shift+l', function() tp.lock('swaylock -f') end)\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "lock_gate",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));

	/* Unlocked: all three bindings fire. */
	reset_spawn_log();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_a, false));
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_b, false));
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_c, false));
	CHECK(spawn_log_count == 3);

	/* Locked: only { locked = true } fires; normal and explicit-false are gated. */
	reset_spawn_log();
	CHECK(!tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_a, true));
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_b, true));
	CHECK(!tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_c, true));
	CHECK(spawn_log_count == 1 && strcmp(spawn_log[0], "locked-b") == 0);

	/* tp.lock argv and string forms invoke the lock handlers. */
	reset_lock_log();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_l, false));
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_l, false));
	CHECK(lock_log_count == 2);
	CHECK(strcmp(lock_log[0], "argv:hyprlock|--immediate") == 0);
	CHECK(strcmp(lock_log[1], "sh:swaylock -f") == 0);

	/* Validation errors: bad options table, non-boolean locked, tp.lock at load. */
	const char *bad[] = {
		"tp.bind('Meta+q', function() end, 'not-a-table')",
		"tp.bind('Meta+q', function() end, { locked = 'yes' })",
		"tp.lock('hyprlock')",
		"tp.lock{'hyprlock'}",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(!tp_lua_exec_string(lua, bad[i], strlen(bad[i]), "bad_lock",
								  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
	}

	/* Lock handlers survive tp_lua_reload_file. */
	char dir[] = "/tmp/tam-pane-lock-reload.XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char cfg_path[256];
	snprintf(cfg_path, sizeof(cfg_path), "%s/init.lua", dir);
	CHECK(write_text(cfg_path,
					 "tp.bind('Meta+l', function() tp.lock{'locker'} end)\n"));
	CHECK(tp_lua_reload_file(&lua, cfg_path, &out, err, sizeof(err)));
	reset_lock_log();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_l, false));
	CHECK(lock_log_count == 1 && strcmp(lock_log[0], "argv:locker") == 0);
	unlink(cfg_path);
	rmdir(dir);

	tp_lua_destroy(lua);
}

static char capture_log[8][128];
static size_t capture_log_count;

static void reset_capture_log(void) { capture_log_count = 0; }

static void mock_capture(const char *cmd, void *userdata) {
	(void)userdata;
	if (capture_log_count < 8)
		snprintf(capture_log[capture_log_count++], 128, "sh:%s", cmd);
}

static void mock_capture_argv(const char *const argv[], void *userdata) {
	(void)userdata;
	if (capture_log_count >= 8)
		return;
	char *dst = capture_log[capture_log_count++];
	size_t pos = (size_t)snprintf(dst, 128, "argv:");
	for (size_t i = 0; argv[i] && pos + 2 < 128; i++)
		pos += (size_t)snprintf(dst + pos, 128 - pos, "%s%s", i ? "|" : "",
								argv[i]);
}

static void test_capture_launcher(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_capture_fn(lua, mock_capture, NULL);
	tp_lua_set_capture_argv_fn(lua, mock_capture_argv, NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	char err[256] = {0};

	const char *cfg =
		"tp.bind('Meta+s', function() tp.capture{'grim', '/tmp/shot.png'} end)\n"
		"tp.bind('Meta+Shift+s', function() tp.capture('grim out.png') end)\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "capture_cfg",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));

	reset_capture_log();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_s, false));
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_s, false));
	CHECK(capture_log_count == 2);
	CHECK(strcmp(capture_log[0], "argv:grim|/tmp/shot.png") == 0);
	CHECK(strcmp(capture_log[1], "sh:grim out.png") == 0);

	/* tp.capture refused at config load and rejects empty args. */
	const char *bad[] = {
		"tp.capture('grim')",
		"tp.capture{'grim'}",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(!tp_lua_exec_string(lua, bad[i], strlen(bad[i]), "bad_capture",
								  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
	}

	/* Capture handlers survive tp_lua_reload_file. */
	char dir[] = "/tmp/tam-pane-cap-reload.XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char cfg_path[256];
	snprintf(cfg_path, sizeof(cfg_path), "%s/init.lua", dir);
	CHECK(write_text(cfg_path,
					 "tp.bind('Meta+s', function() tp.capture{'grim'} end)\n"));
	CHECK(tp_lua_reload_file(&lua, cfg_path, &out, err, sizeof(err)));
	reset_capture_log();
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_s, false));
	CHECK(capture_log_count == 1 && strcmp(capture_log[0], "argv:grim") == 0);
	unlink(cfg_path);
	rmdir(dir);

	tp_lua_destroy(lua);
}

static void test_layouts_and_scratchpad(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_action_fn(lua, mock_action, NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	out = base;
	char err[256] = {0};

	/* Valid layout config + dispatchers + scratchpad rule */
	const char *cfg =
		"tp.config{\n"
		"  layout = 'scroller',\n"
		"  master_ratio = 0.65,\n"
		"  master_count = 2,\n"
		"  scroller_width = 0.75,\n"
		"}\n"
		"tp.bind('Meta+m', tp.dsp.layout('set', 'master'))\n"
		"tp.bind('Meta+Shift+m', tp.dsp.layout('nmaster', 1))\n"
		"tp.rule{ match = { app_id = '^notes$' }, scratchpad = true }\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "layout_cfg",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(out.layout == 2); /* TP_LAYOUT_SCROLLER */
	CHECK(out.master_ratio > 0.64f && out.master_ratio < 0.66f);
	CHECK(out.master_count == 2);
	CHECK(out.scroller_width > 0.74f && out.scroller_width < 0.76f);

	/* Default bindings: Meta+Space, Meta+Shift+Space, Meta+minus, Meta+Shift+minus */
	action_count = 0;
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_space, false));
	CHECK(action_count == 1 &&
		  last_action.kind == TP_ACT_LAYOUT_CYCLE && last_action.dir == 1);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_space,
							false));
	CHECK(action_count == 2 &&
		  last_action.kind == TP_ACT_LAYOUT_CYCLE && last_action.dir == -1);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_minus, false));
	CHECK(action_count == 3 &&
		  last_action.kind == TP_ACT_SCRATCHPAD_TOGGLE);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_minus,
							false));
	CHECK(action_count == 4 &&
		  last_action.kind == TP_ACT_SCRATCHPAD_MOVE);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_m, false));
	CHECK(action_count == 5 &&
		  last_action.kind == TP_ACT_LAYOUT_SET && last_action.dir == 1);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_m, false));
	CHECK(action_count == 6 &&
		  last_action.kind == TP_ACT_LAYOUT_NMASTER &&
		  last_action.dir == 1);

	/* Scratchpad rule evaluation */
	struct tp_rule_result r = {0};
	CHECK(tp_lua_eval_rules(lua, "notes", "scratch", &r));
	CHECK(r.matched && r.has_scratchpad && r.scratchpad);

	/* Invalid config and dispatcher arguments rejected */
	const char *bad[] = {
		"tp.config{ layout = 'grid' }",
		"tp.config{ master_ratio = 0.01 }",
		"tp.config{ master_count = 0 }",
		"tp.config{ scroller_width = 1.5 }",
		"tp.dsp.layout('set', 'bogus')",
		"tp.dsp.layout('nmaster', 0)",
		"tp.dsp.scratchpad('drop')",
		"tp.rule{ match = { app_id = 'a' }, scratchpad = 'yes' }",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(!tp_lua_exec_string(lua, bad[i], strlen(bad[i]), "bad_layout",
								  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
	}

	tp_lua_destroy(lua);
}

static void test_controls_0_2_8(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_action_fn(lua, mock_action, NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	char err[256] = {0};

	/* XKB + pointer settings, tp.monitor, code:N, release, mouse_bind, send_key, capture, focus dir */
	const char *cfg =
		"assert(type(tp.home) == 'string')\n"
		"assert(type(tp.config_dir) == 'string')\n"
		"tp.config{\n"
		"  kb_layout = 'us,dk',\n"
		"  kb_options = 'compose:ralt',\n"
		"  numlock_by_default = true,\n"
		"  natural_scroll = true,\n"
		"  scroll_factor = 1.5,\n"
		"}\n"
		"tp.monitor{ output = 'DP-2', mode = '1920x1080@60', position = {0, 288}, scale = 1.25, transform = 0 }\n"
		"tp.monitor{ output = '', mode = 'preferred', position = 'auto', scale = 1.0 }\n"
		"tp.bind('Meta+code:10', tp.dsp.tag('view', 1), { desc = 'Tag 1' })\n"
		"tp.bind('F9', tp.dsp.spawn('voxtype record stop'), { release = true, desc = 'Stop' })\n"
		"tp.bind('Meta+Left', tp.dsp.focus('left'))\n"
		"tp.bind('Meta+v', tp.dsp.send_key('Ctrl+v'))\n"
		"tp.bind('Print', tp.dsp.capture('grim /tmp/x.png'))\n"
		"tp.mouse_bind('Meta+mouse:273', tp.dsp.spawn('tam-menu toggle start'))\n"
		"tp.mouse_bind('Meta+Alt+mouse:273', tp.dsp.mouse_resize())\n"
		"tp.mouse_bind('Meta+mouse_up', tp.dsp.tag('back'))\n";

	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "controls_0_2_8",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(strcmp(out.kb_layout, "us,dk") == 0);
	CHECK(strcmp(out.kb_options, "compose:ralt") == 0);
	CHECK(out.numlock_by_default == true);
	CHECK(out.natural_scroll == true);
	CHECK(out.scroll_factor > 1.49f && out.scroll_factor < 1.51f);
	CHECK(tp_lua_monitor_count(lua) == 2);

	struct tp_monitor_config mc = {0};
	CHECK(tp_lua_monitor_rule(lua, "DP-2", &mc));
	CHECK(mc.has_mode && mc.mode_w == 1920 && mc.mode_h == 1080 &&
		  mc.mode_refresh_mhz == 60000);
	CHECK(mc.has_pos && !mc.pos_auto && mc.pos_x == 0 && mc.pos_y == 288);
	CHECK(mc.has_scale && mc.scale > 1.24f && mc.scale < 1.26f);

	/* Wildcard fallback */
	CHECK(tp_lua_monitor_rule(lua, "HDMI-A-9", &mc));
	CHECK(mc.mode_preferred && mc.pos_auto);

	/* Keycode, release, directional focus, send_key, capture */
	action_count = 0;
	CHECK(tp_lua_handle_key_ex(lua, TP_MOD_META, XKB_KEY_NoSymbol, 10, false,
							   false));
	CHECK(action_count == 1 && last_action.kind == TP_ACT_TAG_VIEW &&
		  last_action.tag == 1);
	CHECK(!tp_lua_handle_key_ex(lua, 0, XKB_KEY_F9, 75, false, false));
	CHECK(tp_lua_handle_key_ex(lua, 0, XKB_KEY_F9, 75, true, false));
	CHECK(action_count == 2 && last_action.kind == TP_ACT_SPAWN);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_Left, false));
	CHECK(action_count == 3 && last_action.kind == TP_ACT_FOCUS_DIR &&
		  last_action.dir == TP_DIR_LEFT);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META, XKB_KEY_v, false));
	CHECK(action_count == 4 && last_action.kind == TP_ACT_SEND_KEY &&
		  last_action.mods == TP_MOD_CTRL && last_action.sym == XKB_KEY_v);
	CHECK(tp_lua_handle_key(lua, 0, XKB_KEY_Print, false));
	CHECK(action_count == 5 && last_action.kind == TP_ACT_CAPTURE);

	/* Mouse bindings */
	struct tp_action grab = {0};
	CHECK(tp_lua_handle_mouse(lua, TP_MOD_META, 273, false, &grab));
	CHECK(action_count == 6 && last_action.kind == TP_ACT_SPAWN);
	CHECK(tp_lua_handle_mouse(lua, TP_MOD_META | TP_MOD_ALT, 273, false,
							  &grab));
	CHECK(grab.kind == TP_ACT_MOUSE_RESIZE);
	CHECK(tp_lua_handle_mouse(lua, TP_MOD_META, TP_MOUSE_WHEEL_UP, false,
							  &grab));
	CHECK(action_count == 7 && last_action.kind == TP_ACT_TAG_BACK);

	/* tp.include optional missing + real include + atomic rollback on error */
	char dir_tmpl[] = "/tmp/tam-pane-inc-XXXXXX";
	char *dir = mkdtemp(dir_tmpl);
	CHECK(dir != NULL);
	char inc_path[256], bad_path[256];
	snprintf(inc_path, sizeof(inc_path), "%s/override.lua", dir);
	snprintf(bad_path, sizeof(bad_path), "%s/bad.lua", dir);
	FILE *f = fopen(inc_path, "w");
	CHECK(f != NULL);
	fputs("tp.config{ gaps = 14 }\n", f);
	fclose(f);
	f = fopen(bad_path, "w");
	CHECK(f != NULL);
	fputs("error('boom inside include')\n", f);
	fclose(f);

	char inc_cfg[512];
	snprintf(inc_cfg, sizeof(inc_cfg),
			 "assert(tp.include('%s/missing.lua', true) == false)\n"
			 "assert(tp.include('%s', true) == true)\n",
			 dir, inc_path);
	CHECK(tp_lua_exec_string(lua, inc_cfg, strlen(inc_cfg), "inc_ok",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(out.gap_inner == 14 && out.gap_outer == 14);

	snprintf(inc_cfg, sizeof(inc_cfg),
			 "tp.config{ gaps = 20 }\ntp.include('%s')\n", bad_path);
	CHECK(!tp_lua_exec_string(lua, inc_cfg, strlen(inc_cfg), "inc_bad",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	CHECK(out.gap_inner == 14); /* rolled back */

	unlink(inc_path);
	unlink(bad_path);
	rmdir(dir);
	tp_lua_destroy(lua);
}

static void test_gamma_freeze_and_lock_events_0_2_16(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	struct tp_settings base, out;
	tp_settings_defaults(&base);
	char err[256] = {0};

	last_action = (struct tp_action){0};
	action_count = 0;
	tp_lua_set_action_fn(lua, mock_action, NULL);

	const char *cfg =
		"tp.bind('Meta+Shift+G', tp.dsp.gamma(4000))\n"
		"tp.bind('Meta+Shift+H', tp.dsp.gamma('reset'))\n"
		"tp.bind('Meta+Shift+Z', tp.dsp.freeze('on'))\n"
		"tp.bind('Meta+Shift+X', tp.dsp.freeze('off'))\n"
		"tp.on('session.lock', function(ev) assert(ev.locked == true) end)\n"
		"tp.on('session.unlock', function(ev) assert(ev.locked == false) end)\n"
		"tp.on('gamma.change', function(ev)\n"
		"  assert(ev.temperature == 4000 and ev.enabled == true)\n"
		"end)\n";
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "test_0_2_16",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));

	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_g, false));
	CHECK(action_count == 1 && last_action.kind == TP_ACT_GAMMA &&
		  last_action.gamma_temp == 4000);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_h, false));
	CHECK(action_count == 2 && last_action.kind == TP_ACT_GAMMA &&
		  last_action.gamma_temp == 6500);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_z, false));
	CHECK(action_count == 3 && last_action.kind == TP_ACT_FREEZE &&
		  last_action.dir == 1);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_x, false));
	CHECK(action_count == 4 && last_action.kind == TP_ACT_FREEZE &&
		  last_action.dir == 0);

	CHECK(tp_lua_has_hooks(lua, TP_EVENT_SESSION_LOCK));
	CHECK(tp_lua_has_hooks(lua, TP_EVENT_SESSION_UNLOCK));
	CHECK(tp_lua_has_hooks(lua, TP_EVENT_GAMMA_CHANGE));

	struct tp_event ev = {.kind = TP_EVENT_SESSION_LOCK};
	ev.session_lock.locked = true;
	tp_lua_emit_event(lua, &ev);
	ev = (struct tp_event){.kind = TP_EVENT_SESSION_UNLOCK};
	ev.session_lock.locked = false;
	tp_lua_emit_event(lua, &ev);
	ev = (struct tp_event){.kind = TP_EVENT_GAMMA_CHANGE};
	ev.gamma.temperature = 4000;
	ev.gamma.enabled = true;
	tp_lua_emit_event(lua, &ev);

	const char *bad_gamma = "tp.bind('Meta+g', tp.dsp.gamma(100))\n";
	CHECK(!tp_lua_exec_string(lua, bad_gamma, strlen(bad_gamma), "bad_gamma",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	const char *bad_freeze = "tp.bind('Meta+z', tp.dsp.freeze('maybe'))\n";
	CHECK(!tp_lua_exec_string(lua, bad_freeze, strlen(bad_freeze), "bad_freeze",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));

	tp_lua_destroy(lua);
}

static void test_controls_and_borders_0_2_17(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);
	tp_lua_set_action_fn(lua, mock_action, NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	CHECK(base.border_color_focused[1] == 1.0f &&
		  base.border_color_focused[3] == 1.0f);
	CHECK(base.border_color_unfocused[3] == 0.0f);

	const char *cfg =
		"tp.config({\n"
		"  border_width = 3,\n"
		"  border_color_focused = '#ff8000',\n"
		"  border_color_unfocused = '#4080c080',\n"
		"})\n"
		"tp.bind('Meta+Shift+1', tp.dsp.monitor('disable', 'eDP-1'))\n"
		"tp.bind('Meta+Shift+2', tp.dsp.monitor('enable', 'eDP-1'))\n"
		"tp.bind('Meta+Shift+3', tp.dsp.pin())\n"
		"tp.bind('Meta+Shift+4', tp.dsp.pin('on'))\n"
		"tp.bind('Meta+Shift+5', tp.dsp.center())\n"
		"tp.bind('Meta+Shift+6', tp.dsp.raise())\n";
	char err[256] = {0};
	CHECK(tp_lua_exec_string(lua, cfg, strlen(cfg), "cfg_0_2_17",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(out.border_width == 3);
	CHECK(out.border_color_focused[0] > 0.99f &&
		  out.border_color_focused[1] > 0.49f &&
		  out.border_color_focused[1] < 0.51f &&
		  out.border_color_focused[2] == 0.0f &&
		  out.border_color_focused[3] == 1.0f);
	/* #4080c080 -> alpha ~ 128/255 = 0.502, premultiplied blue ~ 0.753*0.502 */
	CHECK(out.border_color_unfocused[3] > 0.49f &&
		  out.border_color_unfocused[3] < 0.51f);
	CHECK(out.border_color_unfocused[2] > 0.36f &&
		  out.border_color_unfocused[2] < 0.39f);

	action_count = 0;
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_1, false));
	CHECK(action_count == 1 && last_action.kind == TP_ACT_MONITOR_ENABLE &&
		  last_action.dir == 0 && strcmp(last_action.cmd, "eDP-1") == 0);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_2, false));
	CHECK(action_count == 2 && last_action.kind == TP_ACT_MONITOR_ENABLE &&
		  last_action.dir == 1 && strcmp(last_action.cmd, "eDP-1") == 0);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_3, false));
	CHECK(action_count == 3 && last_action.kind == TP_ACT_PIN &&
		  last_action.dir == -1);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_4, false));
	CHECK(action_count == 4 && last_action.kind == TP_ACT_PIN &&
		  last_action.dir == 1);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_5, false));
	CHECK(action_count == 5 && last_action.kind == TP_ACT_CENTER);
	CHECK(tp_lua_handle_key(lua, TP_MOD_META | TP_MOD_SHIFT, XKB_KEY_6, false));
	CHECK(action_count == 6 && last_action.kind == TP_ACT_RAISE);

	const char *cfg_none =
		"tp.config({ border_color_focused = '0x00ff00ff', "
		"border_color_unfocused = 'none' })\n";
	CHECK(tp_lua_exec_string(lua, cfg_none, strlen(cfg_none), "cfg_none",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));
	CHECK(out.border_color_focused[1] == 1.0f &&
		  out.border_color_unfocused[3] == 0.0f);

	const char *bad_col = "tp.config({ border_color_focused = '#12345' })\n";
	CHECK(!tp_lua_exec_string(lua, bad_col, strlen(bad_col), "bad_col",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));
	const char *bad_pin = "tp.bind('Meta+p', tp.dsp.pin('maybe'))\n";
	CHECK(!tp_lua_exec_string(lua, bad_pin, strlen(bad_pin), "bad_pin",
							  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							  sizeof(err)));

	tp_lua_destroy(lua);
}

static void test_r1_17_pattern_and_utf8_0_3_0(void) {
	struct tp_lua *lua = tp_lua_create();
	CHECK(lua != NULL);

	struct tp_settings base, out;
	tp_settings_defaults(&base);
	char err[256] = {0};

	/* Malformed patterns after the first item must be rejected at load time. */
	static const char *const bad_patterns[] = {
		"foo[bar",
		"abc)",
		"abc(def",
		"abc%b",
		"abc%b(",
		"abc%f",
		"abc%fx",
		"abc%",
		"abc%1",
		"abc%0",
		"[a-%",
		"()foo%2",
		"((a)%1)",
		"foo[]",
	};
	for (size_t i = 0; i < sizeof(bad_patterns) / sizeof(bad_patterns[0]); i++) {
		char src[256];
		snprintf(src, sizeof(src),
				 "tp.rule({ match = { app_id = \"%s\" }, floating = true })\n",
				 bad_patterns[i]);
		err[0] = '\0';
		CHECK(!tp_lua_exec_string(lua, src, strlen(src), "bad_pat",
								  TP_LUA_CONFIG_MAX_INSTRUCTIONS,
								  TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
								  sizeof(err)));
	}

	/* Valid complex patterns must still be accepted. */
	const char *good_src =
		"tp.rule({ match = { app_id = '^foot$' }, tag = 1 })\n"
		"tp.rule({ match = { app_id = 'foo[a-z]+' }, tag = 2 })\n"
		"tp.rule({ match = { app_id = '(foo)%1' }, tag = 3 })\n"
		"tp.rule({ match = { app_id = '%b()' }, tag = 4 })\n"
		"tp.rule({ match = { app_id = '%f[%a]foo' }, tag = 5 })\n"
		"tp.rule({ match = { app_id = '[]a]' }, tag = 6 })\n"
		"tp.rule({ match = { app_id = '[^]a]' }, tag = 7 })\n"
		"tp.rule({ match = { title = '^a.*\xe3\x81\x82$' }, floating = true })\n";
	err[0] = '\0';
	CHECK(tp_lua_exec_string(lua, good_src, strlen(good_src), "good_pats",
							 TP_LUA_CONFIG_MAX_INSTRUCTIONS,
							 TP_LUA_CONFIG_TIMEOUT_MS, &base, &out, err,
							 sizeof(err)));

	/* UTF-8-safe subject truncation: 'a' + 86 * 3-byte 'あ' = 259 bytes.
	 * Byte 256 falls mid-codepoint; safe truncation backs up to 253 bytes
	 * (complete UTF-8), so '^a(あ)+$' still matches cleanly. */
	char utf8_title[300];
	size_t pos = 0;
	utf8_title[pos++] = 'a';
	for (int i = 0; i < 86; i++) {
		utf8_title[pos++] = (char)0xe3;
		utf8_title[pos++] = (char)0x81;
		utf8_title[pos++] = (char)0x82;
	}
	utf8_title[pos] = '\0';
	struct tp_rule_result res = {0};
	CHECK(tp_lua_eval_rules(lua, "none", utf8_title, &res));
	CHECK(res.matched && res.has_floating && res.floating);

	tp_lua_destroy(lua);
}

int main(void) {
	tam_log_set_level(TAM_LOG_ERROR);
	test_config_and_fallback();
	test_sandbox_and_helpers();
	test_watchdog();
	test_default_path();
	test_chord_parser();
	test_default_bindings();
	test_custom_bindings_and_callbacks();
	test_event_hooks();
	test_startup_hook();
	test_memory_and_pattern_limits();
	test_window_rules();
	test_live_reload();
	test_lock_and_binding_gate();
	test_capture_launcher();
	test_layouts_and_scratchpad();
	test_controls_0_2_8();
	test_gamma_freeze_and_lock_events_0_2_16();
	test_controls_and_borders_0_2_17();
	test_r1_17_pattern_and_utf8_0_3_0();
	CHECK_DONE("test_lua");
}
