# labwc

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C. **License:** GPL v2.
- **Base:** wlroots.
- **Scope:** an Openbox-like stacking compositor, configured with
  Openbox-style XML and menus.

## Ideas worth taking

- **Very careful protocol coverage and XWayland behaviour** for a small C
  codebase. adopt as a reference (P3).
- **Clear code layout, one subsystem per directory.** adopt.
- **Server-side decorations drawn without pango, using its own simple
  font rendering path.** consider (P5, titles without pango).

## Things to avoid

- **XML configuration.**

## Where to look

- `src/` (organised by subsystem)
- `src/xwayland.c`
