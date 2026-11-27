# tam-pane Lua API (`arch/lua-api.md`)

**Status:** 0.1.0–0.1.19 shipped and accepted (Phase P2 + R1 hardening); 0.2.0 adds `tp.lock(...)` and the `tp.bind(..., { locked = true })` binding gate; 0.2.1 adds `tp.capture(...)`; 0.2.5 adds layout settings, `tp.dsp.layout`, `tp.dsp.scratchpad`, and the `scratchpad` window rule.

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
- **Pattern matching (0.1.8):** `string.find`, `match`, `gmatch` and
  `gsub` run in C, where the count hook never fires. A marked patch to
  Lua's matcher checks the same wall-clock deadline every 4,096 steps.
  A runaway pattern therefore ends with `execution timed out`, while
  long legitimate matching within the deadline still runs.
- **Memory (0.1.8):** a Lua state may hold at most 64 MiB
  (`TP_LUA_MEMORY_MAX`) while Lua code runs. A growth past that raises
  Lua's "not enough memory" error inside the call; the log says `lua: ...
  reached the 64 MiB memory limit`, and a failing config reports
  `Lua memory limit (64 MiB) reached`. The compositor keeps running.
- **Not a security boundary.** The sandbox keeps a broken config from
  hanging or crashing the compositor. It does not make the config safe
  to run if you did not write it: `tp.spawn` starts any program, and
  `tp.read_file` reads any file you can read. Treat strings from events
  as untrusted input. Pass them to `tp.spawn{...}`, never into a
  `tp.spawn(cmd)` string.

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
| `border_width` | integer | `0..16` logical pixels for window borders. `0` disables borders. | `0` |
| `border_color_focused` | string | `#RRGGBB[AA]`, `0xRRGGBB[AA]`, or `"none"`/`""` (0.2.17) | `"#00ff00ff"` |
| `border_color_unfocused` | string | `#RRGGBB[AA]`, `0xRRGGBB[AA]`, or `"none"`/`""` (0.2.17) | `"none"` |
| `corner_radius` | integer | `0..64` logical pixels for rounded window corners and borders; `0` uses plain scene path (0.4.0) | `0` |
| `shadow_enabled` | boolean | Enable analytical GLES2 drop shadows behind windows (0.4.0) | `false` |
| `shadow_radius` | integer | `0..128` logical pixels for shadow spread/blur (0.4.0) | `16` |
| `shadow_offset_x` | integer | `-64..64` logical pixels horizontal shadow offset (0.4.0) | `0` |
| `shadow_offset_y` | integer | `-64..64` logical pixels vertical shadow offset (0.4.0) | `4` |
| `shadow_color` | string | `#RRGGBB[AA]`, `0xRRGGBB[AA]`, or `"none"`/`""` for focused window shadow (0.4.0) | `"#00000073"` |
| `shadow_inactive_color` | string | `#RRGGBB[AA]`, `0xRRGGBB[AA]`, or `"none"`/`""` for unfocused window shadow (0.4.0) | `"#00000040"` |
| `focus_follows_mouse` | boolean | Give the window under the moving pointer keyboard focus. | `false` |
| `locker` | string | Absolute executable path for `tam-pane msg lock`. An empty string disables the command. | `""` |
| `split_ratio` | number | `0.05..0.95` first child's share for a new split | `0.5` |
| `width_bias` | number | `0.1..10.0` side-by-side when `w > h * width_bias` | `1.0` |
| `preserve_split` | boolean | keep each split's initial orientation | `false` |
| `smart_split` | boolean | pointer direction picks split edge | `false` |
| `proportional_split` | boolean | keep same-direction runs proportional | `false` |
| `layout` | string | `"dwindle"`, `"master"`, `"scroller"`, or `"monocle"` (0.2.5) | `"dwindle"` |
| `master_ratio` | number | `0.10..0.90` master-stack pane share (0.2.5) | `0.55` |
| `master_count` | integer | `1..8` master-stack master windows (0.2.5) | `1` |
| `scroller_width` | number | `0.15..1.0` default scroller column width fraction (0.2.5) | `0.5` |
| `resize_step` | integer | `1..500` pixels per keyboard resize step | `40` |
| `terminal` | string | non-empty, max 255 bytes | `$TERMINAL` or `"foot"` |
| `cursor_theme` | string | max 127 bytes; `""` uses `XCURSOR_THEME` default | `""` |
| `cursor_size` | integer | `1..256` pixels | `24` |
| `repeat_rate` | integer | `1..200` keys per second | `25` |
| `repeat_delay` | integer | `1..5000` milliseconds | `600` |

### `tp.spawn(cmd)` and `tp.spawn{prog, arg, ...}`

Both forms start a detached program with a double fork, so they never
block the compositor event loop.

- `tp.spawn(cmd)` runs the string `cmd` with `/bin/sh -c` (`tp_spawn`).
  Use it for literal commands that you write yourself.
- `tp.spawn{prog, arg, ...}` (0.1.7) runs `prog`, found on `PATH`, with
  exactly these arguments and no shell (`tp_spawn_argv`). Use it whenever
  an argument comes from an event or a window. Window titles and app ids
  are set by clients, so a title like `'; rm -rf ~ #` stays one harmless
  argument:

  ```lua
  tp.on("window.open", function(ev)
  	tp.spawn{"notify-send", "Opened", ev.title}   -- safe
  	-- tp.spawn("notify-send Opened " .. ev.title) -- shell injection
  end)
  ```

  The list must hold 1 to 64 strings (`TP_LUA_SPAWN_ARGV_MAX`). None may
  contain a NUL byte, and the program name must not be empty. If the
  program cannot be started, tam-pane's log shows
  `tam-pane: cannot run <prog>: <reason>`.

**Not while the config loads (0.1.7).** `tp.spawn` called while
`init.lua` loads raises an error. Such a spawn would run again on every
reload, before tam-pane's own `WAYLAND_DISPLAY` exists (nested, the
program would open on the host), and even if the rest of the config then
fails. Put autostart in a `startup` hook instead:

```lua
tp.on("startup", function()
	tp.spawn{"waybar"}
	tp.spawn("quickshell -c ~/.config/quickshell")
end)
```

`startup` fires once, after the Wayland socket is up. A reload replaces
the hooks but does not fire `startup` again, so autostart runs exactly
once per session.

### `tp.read_file(path[, max_bytes])`

Reads up to `max_bytes` (default 65,536; hard cap 1,048,576) from a
regular file opened with `O_NONBLOCK` and verified with `S_ISREG`. Returns
`(content)` on success or `(nil, err)` on failure. Refuses FIFOs, sockets,
and device nodes immediately without blocking.

## Shipped in 0.1.1

All default key bindings are loaded via Lua on startup before `init.lua`
runs. `init.lua` may override individual bindings, remove defaults with
`tp.unbind`, or start from scratch with `tp.clear_binds()`. Binding changes
stage alongside `tp.config` and roll back atomically if `init.lua` fails.

### `tp.bind(chord, action[, opts])`

Binds a key chord string to either a `tp.dispatcher` value or a plain Lua
callback function, replacing any existing binding for the same `(mods, key)`.
Raises a Lua error on invalid chord syntax, unknown modifier/key, or invalid
action type.

- **Options (`opts`, optional table, 0.2.0):** `{ locked = true }` allows the
  binding to run while the session is locked (`ext-session-lock-v1`). By
  default (`locked = false`), bindings are gated while locked so keystrokes
  reach the lock screen instead of invoking compositor actions.
- **Modifiers (case-insensitive):** `Meta` (`Super`, `Logo`, `Mod4`),
  `Shift`, `Ctrl` (`Control`), `Alt` (`Mod1`). Unmodified keys (e.g.
  `"XF86AudioRaiseVolume"`) are allowed.
- **Keys:** Any XKB keysym name (canonicalized with `xkb_keysym_to_lower()`
  so `"Meta+W"` and `"Meta+w"` match level-0 `w`), plus aliases `","`, `"."`,
  `"/"`, `"-"`, `"="`, `"+"`, `"Enter"`, `"Esc"`, `"Space"`.
- **Callbacks:** Plain Lua functions run under a tighter runtime watchdog
  (1,000,000 instructions / 100 ms `CLOCK_MONOTONIC` deadline); errors or
  timeouts are logged and the compositor keeps running.

**Reserved chords (0.1.9, updated 0.2.0).** Two chords are handled in C before
any binding is looked up, so `tp.bind` cannot take them and `tp.clear_binds()`
cannot remove them:

- `Ctrl+Alt+F1`..`F12` switch VT when tam-pane owns a session (it runs
  on the hardware). Nested or headless, they are ordinary keys.
- `Ctrl+Alt+Shift+Backspace` is the emergency quit (gated while the session is
  locked in 0.2.0 so it cannot drop a locked session to an unlocked VT shell).

### `tp.lock(cmd)` and `tp.lock{prog, arg, ...}` (0.2.0)

Launches a screen locker (e.g. `tp.lock{"tam-hide"}`) over a pre-created
Wayland `WAYLAND_SOCKET` and grants that exact client connection access to
`ext_session_lock_manager_v1` (which is hidden from all other clients). Accepts
the same string or argv-table forms as `tp.spawn`, and is refused while the
config loads.

### `tp.capture(cmd)` and `tp.capture{prog, arg, ...}` (0.2.1)

Launches a screen-capture tool or portal backend (e.g.
`tp.capture{"grim", "/tmp/shot.png"}` or
`tp.capture{"/usr/lib/xdg-desktop-portal-wlr"}`) over a pre-created Wayland
`WAYLAND_SOCKET` and grants that exact client connection access to the raw
capture globals (`zwlr_screencopy_manager_v1`,
`ext_image_copy_capture_manager_v1`,
`ext_output_image_capture_source_manager_v1`, and
`ext_foreign_toplevel_image_capture_source_manager_v1`), which are hidden from
all other clients. Multiple concurrent capture connections are supported and
each grant is revoked automatically on disconnect. Because `WAYLAND_SOCKET` is
consumed by one Wayland client, launch a single capture client per `tp.capture`
call. Accepts the same string or argv-table forms as `tp.spawn`, and is refused
while the config loads.

### `tp.unbind(chord)`

Removes the binding for `chord` from the staged table. Returns `true` if one
was removed, `false` otherwise.

### `tp.clear_binds()`

Removes all staged bindings (including the defaults).

### `tp.dsp.*` (dispatcher values)

Each constructor validates its arguments immediately and returns a
`tp.dispatcher` userdata value. Passing a dispatcher to `tp.bind` runs the C
action directly without entering the Lua VM on keypress. Dispatchers are also
callable (`tp.dsp.close()()`) inside plain Lua callbacks (calling one during
`init.lua` load raises an error).

| Constructor | Arguments | Behavior |
|---|---|---|
| `tp.dsp.spawn(cmd[, focused_target])` | command (max 255 bytes), optional boolean | Spawn via `/bin/sh -c`. When the flag is true, require a focused window and append `--target 0xID`. |
| `tp.dsp.mouse_move([background_command])` | optional command (max 255 bytes) | Drag a window, or run the command on the desktop background. |
| `tp.dsp.terminal()` | none | Spawn configured `settings.terminal` |
| `tp.dsp.close()` | none | Close focused window |
| `tp.dsp.quit()` | none | Quit `tam-pane` |
| `tp.dsp.focus(dir)` | `"next"` \| `"prev"` | Cycle window focus |
| `tp.dsp.swap(dir)` | `"left"` \| `"right"` \| `"up"` \| `"down"` | Swap focused tiled window with neighbor |
| `tp.dsp.move(dir)` | `"left"` \| `"right"` \| `"up"` \| `"down"` | Move focused tiled window in direction |
| `tp.dsp.resize(dir[, step])` | `dir`, optional int `1..500` | Resize split or floating window (default: `resize_step`) |
| `tp.dsp.toggle_float()` | none | Toggle floating on focused window |
| `tp.dsp.toggle_fullscreen()` | none | Toggle fullscreen on focused window |
| `tp.dsp.tag(op[, n])` | `"view"`, `"move"`, `"toggle_view"`, `"toggle_client"` with `n` in `1..9`; or `"back"` | Tag view / client tag operations |
| `tp.dsp.monitor(op, dir)` | `"focus"` \| `"move"`, `"prev"` \| `"next"` | Focus or move focused window to adjacent monitor |
| `tp.dsp.reload()` | none | Reload the Lua configuration in place (deferred via Wayland idle) |
| `tp.dsp.layout(op[, arg])` | `"set"` + layout name; `"next"` \| `"prev"`; `"nmaster"` + delta (`-8..8`, non-zero) | Switch or cycle the active tag slot's layout, or adjust master count (0.2.5) |
| `tp.dsp.scratchpad(op)` | `"toggle"` \| `"move"` | Toggle scratchpad visibility on active monitor, or move focused window to/from scratchpad (0.2.5) |

## Shipped in 0.1.2

Event hooks register Lua callbacks for compositor lifecycle and state changes.
Hooks run **after** the compositor finishes its own handling via a deferred
Wayland idle callback (never re-entrantly inside a wlroots signal handler),
and each invocation runs under the runtime watchdog (1,000,000 instructions /
100 ms `CLOCK_MONOTONIC` deadline). An error or timeout in one hook is logged
and does not stop remaining hooks on the same event.

**Cascades (0.1.6).** An action taken inside a hook can raise another
event, for example `tp.dsp.tag('view', 2)()` inside a `tag.change` hook.
Those hook-caused events are delivered in a later batch, after the event
loop has handled input and frames. A hook-caused event is one round
deeper than the event whose hook caused it. After 16 rounds
(`TP_EVENT_CASCADE_MAX`), tam-pane drops the event and logs `lua hooks:
dropping <event> after 16 rounds ...` once. A hook that keeps
re-triggering itself therefore stops after 16 rounds instead of freezing
the compositor. Events that the compositor itself raises (input, windows
mapping, monitors) always start again at round 0.

Hook registrations stage alongside `tp.config` and `tp.bind` during config
load and roll back atomically if `init.lua` fails. Each config load starts
from an empty staged hook set so reloads replace hooks without duplicating
them.

### `tp.on(event, fn)`

Registers `fn(ev)` for `event` and returns an integer handle `id` (`>= 1`).
Multiple hooks on the same event run in registration order; each receives a
fresh plain table `ev` (value snapshot with no live compositor pointers).
Raises a Lua error on an unknown event name, non-function callback, or call
outside config load.

| Event | Payload fields in `ev` |
|---|---|
| `"window.open"` | `event`, `app_id`, `title`, `monitor`, `tags` (bitmask), `tag` (`1..9` or `0`), `floating`, `fullscreen`, `x`, `y`, `w`, `h` |
| `"window.close"` | `event`, `app_id`, `title`, `monitor`, `tags`, `tag`, `floating`, `fullscreen`, `x`, `y`, `w`, `h` |
| `"window.focus"` | `event`, `focused` (`true`/`false`), `app_id`, `title`, `monitor`, `tags`, `tag`, `floating`, `fullscreen`, `x`, `y`, `w`, `h` (empty/zeroed when `focused == false`) |
| `"tag.change"` | `event`, `monitor`, `tags` (new bitmask), `prev_tags` (previous bitmask), `tag` (lowest active `1..9`) |
| `"monitor.add"` | `event`, `name`, `x`, `y`, `w`, `h`, `scale` |
| `"monitor.remove"` | `event`, `name`, `x`, `y`, `w`, `h`, `scale` |
| `"session.lock"` (0.2.16) | `event`, `locked` (`true`) |
| `"session.unlock"` (0.2.16) | `event`, `locked` (`false`) |
| `"gamma.change"` (0.2.16) | `event`, `temperature` (Kelvin), `enabled` (`true` when `temperature ~= 6500`) |
| `"startup"` (0.1.7) | `event`; fires once per session, after the Wayland socket is up (never on reload) |

### `tp.off(id)`

Removes the staged hook with handle `id`. Returns `true` if removed, `false`
otherwise. Callable only while loading config.

### `tp.clear_hooks([event])`

Removes all staged hooks, or only those for `event` when an event name is
passed. Callable only while loading config.

## Shipped in 0.1.3

Window rules match new toplevels on `app_id` and/or `title` using Lua's built-in
`string.find` pattern engine (no `pcre2` dependency). Patterns are checked at
config load time, so most malformed patterns fail loudly with atomic rollback.
The check matches against an empty string, so a pattern that is malformed
only after its first item (such as `"a%"`) passes it, then fails and is
logged each time a window maps (R1 finding 17). At map time each pattern
runs under the runtime watchdog (1,000,000 instructions / 100 ms
`CLOCK_MONOTONIC` deadline, which also bounds the matcher since 0.1.8) using a
cached reference to `string.find`. Rules see the first 255 bytes of
`app_id` and `title` (`TP_LUA_RULE_SUBJECT_MAX`), the same as event hooks
(0.1.8).

Rule registrations stage alongside `tp.config`, `tp.bind`, and `tp.on`, starting
from an empty set on each config load and rolling back atomically on error.
Multiple matching rules evaluate in registration order and compose, with later
rules overriding earlier ones for fields they set.

### `tp.rule(spec)`

Registers a window rule and returns an integer handle `id` (`>= 1`). Callable
only while loading config.

- **`match`** (required table): must contain `app_id` and/or `title` (non-empty
  Lua pattern strings, max 255 bytes). When both are set, both must match.
- **Properties** (at least one required; unknown keys raise an error):

| Key | Type | Notes |
|---|---|---|
| `tag` | integer `1..9` | Assign window to a single tag (mutually exclusive with `tags`) |
| `tags` | table of integers `1..9` | Non-empty array of tag numbers (mutually exclusive with `tag`) |
| `floating` | boolean | Force floating (`true`) or tiled (`false`), overriding dialog auto-float |
| `size` | `{ w, h }` or `{ w = ..., h = ... }` | Floating dimensions in pixels (`64..16384`); remembered even when tiled |
| `position` | `{ x, y }`, `{ x = ..., y = ... }`, or `"center"` | Floating offset relative to target monitor `usable` area (`-16384..16384`), or centered on `usable` |
| `monitor` | string (max 127 bytes) | Target output name (e.g. `"DP-2"`); falls back to active monitor with a warning if disconnected |
| `scratchpad` | boolean | Start matching window hidden in the floating scratchpad without stealing focus (0.2.5) |

When a rule routes a new window to a non-visible tag or a monitor other than
the active one, it is placed on that tag/monitor without stealing keyboard focus
from the currently active window. `window.open` hooks observe the final
rule-applied state.

### `tp.remove_rule(id)`

Removes the staged rule with handle `id`. Returns `true` if removed, `false`
otherwise. Callable only while loading config.

### `tp.clear_rules()`

Removes all staged window rules. Callable only while loading config.

## Shipped in 0.1.4

Live configuration reload re-reads the active configuration file (`-c <path>`,
otherwise `$XDG_CONFIG_HOME/tam-pane/init.lua` or `~/.config/tam-pane/init.lua`)
into a **fresh** sandboxed Lua state starting from compiled-in defaults
(`settings`, default key bindings, empty event hooks, and empty window rules).

- **Atomic rollback:** If the file is missing, unreadable, non-regular,
  oversize, binary bytecode, fails to parse, raises a runtime error, or exceeds
  the watchdog budget, the candidate Lua state is destroyed and the last good
  configuration (settings, bindings, hooks, rules, and Lua VM) remains
  completely untouched. The diagnostic is logged and returned through the C
  error buffer (`tp_server_reload`) ready for 0.2.3 IPC.
- **No lingering state:** Settings omitted on reload revert to defaults;
  removed bindings, hooks, rules, and Lua globals disappear cleanly.
- **Live application:** On success, `tam-pane` propagates `width_bias`,
  `preserve_split`, and `proportional_split` to every monitor's layout trees,
  updates keyboard `repeat_rate`/`repeat_delay` when changed, recreates the
  xcursor manager when `cursor_theme`/`cursor_size` changed, and re-arranges
  all monitors with the new gaps.
- **Triggers:**
  - `tp.dsp.reload()` (default binding `Meta+Shift+R`), scheduled via a
    coalesced Wayland idle callback so calling `tp.dsp.reload()()` from a Lua
    binding callback or event hook is always re-entrancy safe.
  - `SIGHUP` (`pkill -HUP tam-pane`).
  - Direct C entry point `tp_server_reload(s, err_buf, err_buf_size)` for 0.2.3
    IPC.

## Shipped in 0.2.16

- **`tp.dsp.gamma(kelvin_or_reset)`:** Creates a dispatcher that sets the
  compositor's native color temperature LUT (`1000..25000` K), or restores the
  identity LUT (`6500` K) when passed `"reset"` or `"off"`.
- **`tp.dsp.freeze("on" | "off")`:** Creates a dispatcher that snapshots all
  enabled outputs onto `TP_LAYER_FREEZE` (below `TP_LAYER_OVERLAY`) or clears
  the freeze.
- **Events:** `"session.lock"`, `"session.unlock"`, and `"gamma.change"` are
  available to `tp.on(...)` hooks and IPC `subscribe`.

## Shipped in 0.2.17

- **Themeable window borders:** `tp.config({ border_color_focused = "#RRGGBB[AA]", border_color_unfocused = "#RRGGBB[AA]" | "none" })`, live-updated on `reload`.
- **Monitor enable/disable:** `tp.dsp.monitor("enable" | "disable", output_name)` for clamshell and laptop panel control.
- **Window pinning and floating helpers:** `tp.dsp.pin(["on" | "off" | "toggle"])`, `tp.dsp.center()`, and `tp.dsp.raise()`.
