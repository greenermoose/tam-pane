# P1: Foundation (0.0.x)

**Status:** in progress. 0.0.1 is built and tested headless.

## Goal

A small, clean compositor on wlroots that tiles windows with dwindle,
shows them by tag, and runs nested under Hyprland and on a spare monitor
session. It sets the module structure everything later builds on.

## Versions

### 0.0.1: framework compiles

- Repository, license and documentation folders (`ideas/`, `arch/`,
  `plans/`, `bugs/`).
- Plain Makefile. Protocol headers are generated with `wayland-scanner`.
- Modules: `server`, `monitor`, `client`, `focus`, `input/{input,keyboard,pointer}`,
  `bindings`, `spawn`, `settings`, `layout/{dwindle,arrange}`, `tags`.
- Pure-logic unit tests for tags and dwindle. A headless smoke test that
  starts, maps windows and stops.

### 0.0.2: first nested run

- Run `build/tam-pane` as a window inside Hyprland (wlroots' Wayland
  backend). Fred tests it; agents never launch it on his monitors.
- Fix whatever the first real use shows: focus, pointer enter and leave,
  keyboard repeat, cursor image.
- Add `tests/` coverage for any logic bug found.

### 0.0.3: tags in use

- Check every tag binding interactively.
- Move a window to several tags at once (Meta+Shift+Ctrl+N, as in dwm).
- Per-tag remembered focus: returning to a tag focuses the window you
  left.

### 0.0.4: tiling complete

- Resize splits from the keyboard (grow and shrink along a direction).
- Move and resize floating windows with Meta plus a mouse button.
- Swap and move tiled windows by direction.
- Optional proportional resizing on insert and remove (mango's
  `dwindle_manual_split` arithmetic, rewritten as pure functions with
  tests).

### 0.0.5: panels and monitors

- layer-shell: panels, bars, launchers and notifications, with exclusive
  zones shrinking `tp_monitor.usable`. Layer popups.
- Several monitors: focus and move windows between them, and handle
  hot-plug without losing windows (they move to another monitor and come
  back by name when it returns).
- Quickshell runs as the bar.

## Acceptance

- `make clean && make`: no warnings.
- `make test`: every unit test and headless check passes.
- 0.0.2 to 0.0.5: Fred runs the build nested or on a spare session and
  confirms the version's features. Record the exact build he ran.

## Dependencies

- Uses: wlroots 0.20, wayland-server, xkbcommon, libtam (static).
- None added in this phase.

## Risks

- **wlroots API drift.** wlroots changes its API between minor versions.
  We pin 0.20 and record each change we make for a new wlroots version in
  `arch/dependencies.md`. P6 removes the dependency.
- **Nested testing steals focus on Fred's active monitor.** Fred runs the
  nested window himself, on the monitor he chooses.

## Open points

- None.
