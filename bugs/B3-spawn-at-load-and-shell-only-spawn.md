# B3: tp.spawn runs at config load, and only through a shell

- **Status:** Fix available
- **Severity:** high
- **Affected versions:** 0.1.0 to 0.1.6
- **Fixed in:** 0.1.7
- **Reported:** 2026-11-21, by Claude Code (claude-opus-5-5)
- **Found by:** R1 (findings 3 and 7)

## What happens

1. `tp.spawn("waybar")` at the top of `init.lua` runs while the config
   loads. That is before tam-pane sets its own `WAYLAND_DISPLAY`, so
   when nested the program opens on the host. It also runs again on every
   reload, and it still runs when a later line of the config fails, so
   the config's rollback is not atomic.
2. The only spawn form is `/bin/sh -c` on a string. Hooks that build a
   command from `ev.title` or `ev.app_id` run whatever shell syntax a
   client puts in its title.

## What should happen

Autostart runs once per session, after the socket is up. Strings from
events reach a program as plain arguments.

## Steps to reproduce

1. Put `tp.spawn("foot")` at the top of `init.lua`, start tam-pane, then
   press Meta+Shift+R: a second foot opens.
2. `tp.on('window.open', function(ev) tp.spawn("notify-send " ..
   ev.title) end)`, then open a window titled `x; touch /tmp/pwned`:
   the file appears.

## Cause

`l_tp_spawn` in `src/lua.c` had no guard for config loading, there was
no event to run after startup, and `tp_spawn` only accepted a shell
command string.

## Fix and verification

- `tp.spawn` raises an error while the config loads, and the error names
  `tp.on('startup', ...)`.
- The new `startup` event is emitted once by `tp_server_start`, after
  `WAYLAND_DISPLAY` is set. Reloads never emit it again.
- `tp.spawn{prog, arg, ...}` runs the program with `execvp`
  (`tp_spawn_argv`) and no shell. It takes at most 64 string arguments,
  with no NUL bytes and a non-empty program name.
- `arch/lua-api.md` documents the forms and says the sandbox is not a
  security boundary.

Regression tests:

- `tests/test_lua.c`: spawn at load is refused in both forms; argv is
  passed through unchanged, including a hostile string; malformed lists
  are rejected; a startup hook spawns.
- The `startup-once` smoke check in `tests/smoke-headless.sh`: the hook
  ran once, saw tam-pane's own `WAYLAND_DISPLAY`, and did not run again
  after SIGHUP.

`tools/demo-0.1.7` is Fred's nested check. Not yet checked in a build
Fred runs.
