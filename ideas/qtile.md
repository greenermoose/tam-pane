# Qtile

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Python. **License:** MIT.
- **Base:** X11, or Wayland through wlroots bindings.
- **Scope:** a tiling window manager configured in Python, with many
  layouts.

## Ideas worth taking

- **A wide layout catalogue** (Columns, MonadTall, Bsp, Max, TreeTab) as a
  checklist for our layouts. consider (P3, 0.2.4).
- **Lazy, composable key binding helpers** (`lazy.window.kill()`). This
  is the same idea as Hyprland's dispatchers as values. adopt (P2, 0.1.1).

## Things to avoid

- **Python in the compositor process.**

## Where to look

- `libqtile/layout/`
- `libqtile/lazy.py`
