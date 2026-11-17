# bspwm

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C. **License:** BSD 2-Clause.
- **Base:** X11 (XCB).
- **Scope:** windows as leaves of a binary tree, controlled entirely
  through a socket by `bspc`, with no built-in key bindings.

## Ideas worth taking

- **Binary space partitioning with "preselection".** adopt (P1, 0.0.4).
  - Choose where the next window goes before opening it. This is
    Hyprland's direction override.
- **Every action available over the socket.** adopt (P3, 0.2.3).
  - The IPC is the API, and the bindings just call it.
- **Automatic schemes** (longest side, alternate, spiral) as named
  options. adopt (P2 settings).

## Things to avoid

- **Needing a separate hotkey daemon.**

## Where to look

- `src/tree.c`
- `doc/bspwm.1.asciidoc`
