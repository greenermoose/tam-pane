# P4: Dogfood in Tamlinux 0.7 (0.3.x)

**Status:** not started.

## Goal

Fred uses tam-pane every day as the Tamlinux 0.7.x compositor, with
Hyprland still installed as the fallback session.

## Versions

- **0.3.0** A `tamlinux-packages` recipe for tam-pane, a
  `wayland-sessions` entry, and a session target so portals and user
  services start with it. Deployed only through a recorded assembly on
  Fred's Test word.
- **0.3.1** Quickshell and the `fred.*` plugins use tam-pane's IPC wherever
  they use Hyprland's today (workspaces become tags, monitors, window
  titles).
- **0.3.x** Fixes from daily use, each recorded in `bugs/`.

## Acceptance

- At the first run on real hardware from a text VT: Ctrl+Alt+F-keys
  switch away and back, and Ctrl+Alt+Shift+Backspace quits. 0.1.9
  accepted these on automated tests and a nested check only, because
  nested runs have no session (B5). Fred chose this on 2026-11-21.
- At that first run, on Fred's three monitors: drag a floating window
  onto another monitor and back, move windows with Meta+Shift+Comma and
  Meta+Shift+Period, and unplug and replug a monitor. Its windows leave
  and come back. These check 0.1.10's attach/detach refactor, which needs
  two or more monitors; the nested window has one. Fred deferred this
  check to this phase on 2026-11-21.
- A full working day on tam-pane without falling back to Hyprland.
- Every `bugs/` record found in this phase is Verified or Deferred, with a
  reason.

## Dependencies

- None added.

## Risks

- **Losing the session.** Hyprland stays installed and selectable at login
  for the whole phase. A rollback point is recorded before each Test.

## Open points

- Which Tamlinux candidate first carries tam-pane is Fred's decision,
  through `fred-stage include`.
