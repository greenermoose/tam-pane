# KWin

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C++. **License:** GPL v2 or later.
- **Base:** its own backends, on Qt.
- **Scope:** KDE Plasma's compositor; full desktop features, both X11 and
  Wayland.

## Ideas worth taking

- **Scripting window management in JavaScript,** with an API of windows,
  outputs and signals. adopt the idea (P2; ours is Lua).
- **Mature handling of multi-monitor, HDR and colour management.**
  consider as a reference (P3 and later).
- **Window rules with an interactive "detect window properties" helper.**
  consider (P2, 0.1.3, via IPC).

## Things to avoid

- **Size and the Qt dependency.**

## Where to look

- `src/scripting/`
- `src/rules.cpp`
