# Pinnacle

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Rust. **License:** GPL v3.
- **Base:** Smithay.
- **Scope:** a compositor configured in Lua (or Rust) over gRPC, with
  awesome-style tags.

## Ideas worth taking

- **Tags plus Lua, on Wayland.** It is the nearest relative of tam-pane's
  feature set. consider as a reference (P2).
- **A typed, documented Lua API with editor stubs.** adopt (P2).
  - Hyprland also ships `hl.meta.lua` stubs; we should ship `tp.meta.lua`.

## Things to avoid

- **Configuration in a separate process over gRPC.** Fast enough for them,
  but heavy; our Lua runs in-process under a budget.

## Where to look

- `api/lua/`
