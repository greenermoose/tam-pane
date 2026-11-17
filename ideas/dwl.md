# dwl

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C, about 3,000 lines in one file. **License:** GPL v3.
- **Base:** wlroots.
- **Scope:** dwm for Wayland. Tags, master-stack, configuration in
  `config.h`. MangoWM grew out of it.

## Ideas worth taking

- **Small size as a design rule.** adopt.
  - dwl stays small by moving features into patches. We stay small with
    modules and a line budget per file.
- **Tags and per-monitor tag sets.** adopt (P1, through MangoWM).
- **The clean teardown order.** adopt (P1, 0.0.1).
  - Remove the listeners, destroy the clients, then the backend, then the
    display, then the scene.
- **A status line on stdout for bars.** consider (P3 IPC).
  - A plain text stream of tags and titles, which is simple and robust.

## Things to avoid

- **One source file.** Good for a minimal WM; it does not scale to our
  scope.
- **Features as patches.** Patches conflict with each other; we keep
  features as modules behind settings.

## Where to look

- `dwl.c` (`setup`, `cleanup`, `arrange`, `focusclient`)
- `config.def.h`
