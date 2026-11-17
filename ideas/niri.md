# niri

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Rust. **License:** GPL v3.
- **Base:** Smithay.
- **Scope:** a scrollable-tiling compositor. Windows sit in columns on an
  infinite horizontal strip, and opening a window never resizes the
  others.

## Ideas worth taking

- **Scrollable tiling.** adopt (P3, 0.2.4, the scroller layout).
  - MangoWM already has a scroller; niri is the reference for how it
    should feel.
- **Dynamic workspaces per monitor, created and removed as needed.**
  consider; it relates to how tags appear to bars.
- **An overview that zooms out to all workspaces.** consider (after P3).
- **Careful, well-documented configuration with validation errors shown
  on screen.** adopt (P2, 0.1.4).
- **Screencast of single windows through the portal.** adopt (P3, 0.2.1).

## Things to avoid

- **KDL configuration.** It is fine, but we use Lua.

## Where to look

- `src/layout/` (scrolling, columns)
- the wiki pages on configuration and the overview
