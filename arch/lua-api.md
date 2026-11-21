# tam-pane Lua API (`arch/lua-api.md`)

**Status:** 0.1.0 shipped; 0.1.1–0.1.4 names are a draft for review before 0.1.1.

tam-pane embeds **Lua 5.5.1** statically from `third_party/lua/` with no
dynamic symbol exports (`lua_*` symbols stay internal to `tam-pane`).

## Safety model

- **Opened standard libraries:** `base` (with `dofile` and `loadfile`
  removed, and `print` routed to `tam_log_info`), `coroutine`, `math`,
  `string`, `table`, `utf8`.
- **Not opened (and not linked):** `io`, `os`, `package`, `debug`.
- **Text-only chunks:** `luaL_loadbufferx(..., "t")` rejects precompiled
  Lua bytecode.
- **Watchdog:** every call into Lua runs under a `LUA_MASKCOUNT` hook
  (every 10,000 instructions) enforcing both an instruction budget
  (10,000,000 for config load) and a `CLOCK_MONOTONIC` wall-clock deadline
  (1,500 ms for config load). Runaway loops abort with a Lua error and the
  compositor keeps running.

## Configuration file

On startup, tam-pane loads:

1. the path given to `-c <config-file>`, if passed; otherwise
2. `$XDG_CONFIG_HOME/tam-pane/init.lua`, if `XDG_CONFIG_HOME` is set;
   otherwise
3. `$HOME/.config/tam-pane/init.lua`.

If the file is missing, unreadable, fails to parse, raises an error, or
exceeds the watchdog budget, tam-pane logs the reason and falls back
atomically to the compiled-in defaults.

## Shipped in 0.1.0

### `tp.version`

String containing the current `tam-pane` version (e.g. `"0.1.0"`).

### `tp.config(table)`

Validates and stages compositor settings. May be called more than once in
`init.lua`; changes commit atomically only when `init.lua` finishes
without error. Unknown keys log a warning and are ignored; invalid types
or out-of-range values raise a Lua error.

| Key | Type | Range / Notes | Default |
|---|---|---|---|
| `gaps` | integer | `0..256`; sets both `gap_inner` and `gap_outer` (overridden by specific keys) | `8` |
| `gap_inner` | integer | `0..256` pixels between tiled windows | `8` |
| `gap_outer` | integer | `0..256` pixels between windows and usable monitor edge | `8` |
| `split_ratio` | number | `0.05..0.95` first child's share for a new split | `0.5` |
| `width_bias` | number | `0.1..10.0` side-by-side when `w > h * width_bias` | `1.0` |
| `preserve_split` | boolean | keep each split's initial orientation | `false` |
| `smart_split` | boolean | pointer direction picks split edge | `false` |
| `proportional_split` | boolean | keep same-direction runs proportional | `false` |
| `resize_step` | integer | `1..500` pixels per keyboard resize step | `40` |
| `terminal` | string | non-empty, max 255 bytes | `$TERMINAL` or `"foot"` |
| `cursor_theme` | string | max 127 bytes; `""` uses `XCURSOR_THEME` default | `""` |
| `cursor_size` | integer | `1..256` pixels | `24` |
| `repeat_rate` | integer | `1..200` keys per second | `25` |
| `repeat_delay` | integer | `1..5000` milliseconds | `600` |

### `tp.spawn(cmd)`

Launches `cmd` via `/bin/sh -c` using double-fork (`tp_spawn`). Never
blocks the compositor event loop.

### `tp.read_file(path[, max_bytes])`

Reads up to `max_bytes` (default 65,536; hard cap 1,048,576) from a
regular file opened with `O_NONBLOCK` and verified with `S_ISREG`. Returns
`(content)` on success or `(nil, err)` on failure. Refuses FIFOs, sockets,
and device nodes immediately without blocking.

## Draft for 0.1.1–0.1.4 (for review)

- **0.1.1 Bindings & dispatchers:** `tp.bind("Meta+Return", tp.dsp.spawn("foot"))`,
  `tp.unbind("Meta+Return")`, `tp.dsp.*` values (`spawn`, `close`, `focus`,
  `swap`, `move`, `resize`, `toggle_float`, `toggle_fullscreen`, `tag`,
  `monitor`, `quit`), plus plain Lua callback functions.
- **0.1.2 Event hooks:** `tp.on("window.open", fn)` for `window.open`,
  `window.close`, `window.focus`, `tag.change`, `monitor.add`,
  `monitor.remove`.
- **0.1.3 Window rules:** `tp.rule{ match = { app_id = "...", title = "..." }, ... }`.
- **0.1.4 Reload:** `tp.dsp.reload()` / IPC reload keeping the last good
  configuration on error.
