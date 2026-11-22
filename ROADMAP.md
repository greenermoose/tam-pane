# tam-pane roadmap

Each milestone has a version number. A phase (see `plans/`) covers a range
of versions. Versions follow semantic versioning: during 0.x, the minor
number marks a phase's big step and the patch number marks each
milestone inside it.

**Current version:** 0.1.11, a candidate (Phase P2 complete at 0.1.4,
then hardened from 0.1.5 to fix review R1's findings; 0.1.5–0.1.9
accepted by Fred on 2026-11-21, and his check of 0.1.10 deferred to P4;
development continues on branch `dev`).

## Tamlinux timing

Fred decided this on 2026-11-17:

- tam-pane is developed while the workstation runs Tamlinux 0.6.x on
  Hyprland.
- Dogfooding starts with Tamlinux 0.7.x, using tam-pane 0.3.x.
- This replaces the earlier plan to move Tamlinux 0.7 to Sway. Fred found
  that Sway will not meet Tamlinux's needs.
- Removing dependencies (phase P6) continues while tam-pane is in daily
  use.

## Milestones

### Phase P1: foundation (0.0.x), `plans/P1-foundation.md`

- **0.0.1** The framework compiles cleanly with plain Make. It starts and
  stops on the headless backend, and maps client windows there. The tag
  and dwindle unit tests pass.
- **0.0.2** First run nested in a window under Hyprland: windows open,
  close and take focus, keyboard and pointer work, and the compiled-in
  Meta bindings work.
- **0.0.3** Tags work interactively: view a tag, toggle tags, move a
  window to a tag, go back to the previous view.
- **0.0.4** Dynamic tiling is complete: dwindle with resizing by keyboard
  and mouse, floating windows that move and resize, and fullscreen.
- **0.0.5** layer-shell and multiple monitors, so a Quickshell bar runs
  and windows move between monitors.

### Phase P2: Lua (0.1.x), `plans/P2-lua.md`

- **0.1.0** The Lua 5.5 sources are in the tree, built by our Makefile.
  `~/.config/tam-pane/init.lua` sets the settings, and an instruction
  budget guards every call.
- **0.1.1** Key bindings and dispatchers come from Lua, and the
  compiled-in table goes.
- **0.1.2** Event hooks: window opened, closed and focused; tag changed;
  monitor added and removed.
- **0.1.3** Window rules: match on app id and title, and set tags,
  floating, size and monitor.
- **0.1.4** Reload the configuration without restarting, with errors
  reported and the last good configuration kept.

After P2, review R1 (`reviews/R1-code-review-0.1.4.md`) found bugs that
are fixed before P3, one version each:

- **0.1.5** A tiled resize drag no longer uses freed memory when another
  window leaves the tree (B1).
- **0.1.6** A hook that re-triggers itself no longer freezes the
  compositor: hook-caused events drain on a timer and stop after 16
  rounds (B2).
- **0.1.7** Autostart goes in `tp.on('startup', ...)`, `tp.spawn` is
  refused while the config loads, and `tp.spawn{prog, arg, ...}` runs a
  program with no shell (B3).
- **0.1.8** Lua memory is capped at 64 MiB, and pattern matching obeys
  the watchdog deadline (B4).
- **0.1.9** Keys no config can remove, handled in C before Lua:
  Ctrl+Alt+F1–F12 switch VT when tam-pane owns a session, and
  Ctrl+Alt+Shift+Backspace quits (B5). The VT switch is checked on real
  hardware at the first DRM trial (P4).
- **0.1.10** One `tp_client_attach` / `tp_client_detach` for every
  change of a window's monitor, replacing five hand-written copies
  (R1 finding 13). A floating drag onto another monitor now re-lays out
  both monitors (finding 6). Checked by the headless tests; Fred deferred
  his own check to the first run on real hardware (P4).
- **0.1.11** A new window splits the focused window, as the README says,
  not the last leaf of the tree (R1 finding 12).
- Next: R1's remaining fixes (`work/WHATNOW`).

### Phase P3: daily-driver parity (0.2.x), `plans/P3-daily-parity.md`

- **0.2.0** The protocols the desktop shell needs: foreign-toplevel,
  ext-workspace (for tags), session-lock, idle notify and inhibit, output
  power.
- **0.2.1** Screens and capture: screencopy and image-copy, output
  management, gamma, fractional scale, cursor-shape.
- **0.2.2** Text and activation: text-input and input-method, xdg-activation,
  primary selection, decorations.
- **0.2.3** An IPC socket and a `tam-pane msg` client.
- **0.2.4** More layouts (master-stack, scroller, monocle) and a
  scratchpad.
- **0.2.5** XWayland, with a temporary xcb dependency.

### Phase P4: dogfood in Tamlinux 0.7 (0.3.x), `plans/P4-dogfood-tamlinux-0.7.md`

- **0.3.0** Packaged in `tamlinux-packages` with a login session entry.
  Hyprland stays installed as the fallback session.
- **0.3.1** Quickshell and the `fred.*` plugins work through tam-pane's
  IPC in place of Hyprland's.
- **0.3.x** Fixes from daily use, recorded in `bugs/`.
- Tamlinux's desktop tests switch their offscreen compositor from headless
  Sway to tam-pane once tam-pane's headless mode can serve them (Tamlinux
  D268).

### Phase P5: visuals (0.4.x), `plans/P5-visuals.md`

- **0.4.x** Our own GLES drawing on Mesa: borders, rounded corners,
  shadows, blur and animations. No scenefx.

### Phase P6: own the stack (0.5.x to 0.7.x), `plans/P6-own-the-stack.md`

- **0.5.x** libtam input (evdev and keymaps) replaces libinput and
  xkbcommon.
- **0.6.x** A libtam Wayland wire protocol replaces libwayland-server.
- **0.7.x** A libtam KMS and session backend, plus Mesa GBM and EGL,
  replaces wlroots. This reaches the goal: libc, libtam and Mesa.

### Phase P7: retire Hyprland (1.0.0), `plans/P7-retire-hyprland.md`

- **1.0.0** tam-pane becomes the only Tamlinux compositor and Hyprland is
  removed. This happens only on Fred's Test, Run and Release words.

## How a milestone is done

A milestone is done when its acceptance checks in the phase plan pass and
Fred has tried the build. When a milestone lands, bump `VERSION` in the
same change and update the current version here.
