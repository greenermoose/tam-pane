# P2: Lua scripting (0.1.x)

**Status:** in progress (0.1.3 accepted by Fred on 2026-11-21 on commit
`04522cc`, binary SHA-256
`fd4029ca5a198b68a018111e5a509b70df84ebc2203a18acd6d847418d4a09a6`; 0.1.2,
0.1.1, and 0.1.0 accepted on 2026-11-21).

## Goal

Lua is built into tam-pane. The configuration, key bindings, event hooks,
window rules and (later) custom layouts are Lua. A script can never block
or crash the compositor.

## Versions

### 0.1.0: the interpreter and init.lua

- Fetch the Lua 5.5 release tarball from lua.org into its own empty
  directory. Check its SHA-256 against the value lua.org publishes, then
  copy the C sources (not the `lua` or `luac` programs) into
  `third_party/lua/`, with its license and version noted in `UPSTREAM.md`.
- Build them in our Makefile as a static object set.
- Load `$XDG_CONFIG_HOME/tam-pane/init.lua` (default
  `~/.config/tam-pane/init.lua`), and fall back to the compiled-in
  settings when it is missing or fails.
- The `tp` table exposes the settings: `tp.config{ gaps = 8, ... }`.
- Every call into Lua runs under an instruction budget and a wall-clock
  deadline, using a `lua_sethook` count hook as Hyprland's
  `guardedPCall` does.

### 0.1.1: bindings and dispatchers

- `tp.bind("Meta+Return", tp.dsp.spawn("foot"))`, in the style of
  Hyprland's `hl.dsp.*`, with dispatchers as values.
- A binding may also call a plain Lua function.
- The compiled-in table in `src/bindings.c` becomes the defaults Lua
  starts from, and the C actions become the dispatchers.

### 0.1.2: event hooks

- `tp.on("window.open", fn)` and so on for open, close, focus, tag change,
  and monitor add and remove.
- Hooks run after the compositor has finished its own handling, never
  inside a wlroots callback that holds state mid-change.

### 0.1.3: window rules

- Match on app id and title (Lua patterns, so no pcre2), and set tags,
  floating, size, position and monitor.

### 0.1.4: reload

- Reload the configuration in place. A broken file keeps the last good
  configuration, and the error is reported through the log and IPC.

## Safety rules (from Hyprland's 2026-10-31 freeze)

- Do not open Lua's `io` and `os` libraries as they come. Hyprland calls
  `luaL_openlibs`, which exposes `io.popen` and `os.execute`, and a
  blocking child process deadlocked the compositor.
- Provide `tp.spawn` (non-blocking, through `tp_spawn`) and
  `tp.read_file` (bounded) instead.
- The instruction budget catches runaway loops, but not a blocking C call.
  Expose no blocking C calls at all.

## Acceptance

- Unit tests for the binding parser, rules matching and config loading,
  run headless.
- A runaway `while true do end` in a hook is cut off within the deadline,
  and the compositor keeps running.
- Fred's chosen bindings, written in `init.lua`, work nested.

## Dependencies

- Adds: Lua 5.5 sources in the tree. No external dependency.

## Risks

- **Version.** tam-pane uses Lua 5.5, by Fred's choice
  (2026-11-17), the same major version Hyprland 0.56 and later embed.
  Configurations still will not carry over word for word, because the
  API is our own.
- **Two Lua versions in one process.** Until wlroots goes (P6), Arch's
  libinput loads liblua 5.4 for its plugins. tam-pane's static Lua 5.5
  must not export its `lua_*` symbols: link without `-rdynamic`, and check
  with `nm -D build/tam-pane` that none appear.

## Open points

- The exact `tp` API names. Draft them in `arch/lua-api.md` and show Fred
  before 0.1.1.
