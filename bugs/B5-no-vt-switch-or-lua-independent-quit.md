# B5: no VT switching, and no quit that a config cannot remove

- **Status:** Fix available (the quit is verified; the VT switch waits for
  the first DRM trial)
- **Severity:** high
- **Affected versions:** 0.0.1 to 0.1.8
- **Fixed in:** 0.1.9
- **Reported:** 2026-11-21, by Claude Code (claude-opus-5-5)
- **Found by:** R1 (finding 10)

## What happens

- On real hardware, Ctrl+Alt+F1–F12 does nothing: no code calls
  `wlr_session_change_vt`. A hung or broken session cannot be left for
  a text VT, so the only way out is SysRq.
- Every binding lives in Lua. An `init.lua` that calls `tp.clear_binds()`
  and binds no quit key leaves no way to exit.

## What should happen

VT switching and one emergency quit always work, whatever the config.

## Steps to reproduce

1. Load a config containing only `tp.clear_binds()`, and press
   Meta+Shift+E: nothing happens.
2. Run tam-pane from a text VT and press Ctrl+Alt+F2: nothing happens.

Automated: the compositor-keys case at the end of
`tests/test_input_headless.c`. Against 0.1.8 it does not build, because
the `change_vt` and `quit_requested` fields are new.

## Cause

`on_key` in `src/input/keyboard.c` passed every key to the Lua bindings
or the client. Nothing handled keys in C.

## Fix and verification

`run_compositor_key` runs before the Lua bindings.

- Ctrl+Alt+F1–F12 (the keymap turns them into `XF86Switch_VT_n`) call
  `server.change_vt`. That is set only when tam-pane owns a session, so
  nested or headless they stay ordinary keys.
- Ctrl+Alt+Shift+Backspace (matched on the level-0 symbol, with exactly
  those modifiers) logs `emergency quit` and calls `tp_server_quit`.
  Fred chose this chord on 2026-11-21.

Tests:

- with no session, Ctrl+Alt+F3 passes through;
- with a stand-in session, Ctrl+Alt+F2 and F12 switch to VT 2 and 12 and
  are consumed;
- plain F2 is an ordinary key;
- with every binding cleared, Meta+Shift+E does nothing,
  Ctrl+Alt+Backspace does nothing, and Ctrl+Alt+Shift+Backspace quits.

`make test` passes. `sandbox/demo-0.1.9` is Fred's nested check of the
quit. By Fred's choice, VT switching is checked on real hardware at the
first DRM trial (`plans/P4-dogfood-tamlinux-0.7.md`). Fred checked the
quit nested with `sandbox/demo-0.1.9` on 2026-11-21 (commit 92f1b11, binary
SHA-256
`17b80f2b31ef0cb0a2eb1cfc646fa6ad7608749f7a0b987fa4c0502414c04834`), and the
demo printed PASS. Set this record to Verified once the VT switch passes
on real hardware.
