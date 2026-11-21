# P1: Foundation (0.0.x)

**Status:** complete. 0.0.5 panels and monitors accepted by Fred on
2026-11-21. Next phase: P2 (`plans/P2-lua.md`, 0.1.0).

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

Accepted on 2026-11-17: Fred reported "worked" after the nested input
check using helper commit `ec41c2d`. The tested compositor binary SHA-256
was `9d8797596fef75c5ed7147ffbe2425f6251cbdf0589422cd0340c838527e0e51`
(it still reported 0.0.1 before this acceptance bump).

- Run `build/tam-pane` as a window inside Hyprland (wlroots' Wayland
  backend). Fred tests it; agents never launch it on his monitors.
- Fix whatever the first real use shows: focus, pointer enter and leave,
  keyboard repeat, cursor image.
- Add `tests/` coverage for any logic bug found.

### 0.0.3: tags in use

- Accepted on 2026-11-17 after representative nested checks on commit
  `dea128d`. Automated tests cover all nine tag numbers; no exhaustive
  human sweep is required.
- Move a window to several tags at once (Meta+Shift+Ctrl+N, as in dwm).
- Per-tag remembered focus: returning to a tag focuses the window you
  left.

### 0.0.4: tiling complete

- Accepted on 2026-11-21 after Fred's nested check passed on commit
  `9745163` (binary SHA-256
  `1593e83af26158b6897c47979ce7da34d71a4c249d4685fe8e7f2e287269f9b9`).
- Resize splits from the keyboard (grow and shrink along a direction).
- Move and resize floating windows with Meta plus a mouse button.
- Swap and move tiled windows by direction.
- Optional proportional resizing on insert and remove (mango's
  `dwindle_manual_split` arithmetic, rewritten as pure functions with
  tests).

### 0.0.5: panels and monitors

- Accepted on 2026-11-21 after Fred's nested Quickshell bar check passed
  on commit `a412e0b` (binary SHA-256
  `5c2a1b2befab973d9212d3c82ff64a8149e74a365bf4264f193d8c5da1401295`).
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
  confirms representative behavior that automation cannot establish. Record
  the exact build he ran. Agents automate exhaustive combinations; never ask
  Fred to repeat them manually. Keep human checks brief and enjoyable, and
  stop when Fred says he has tested enough.

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
