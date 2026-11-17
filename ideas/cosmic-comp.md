# cosmic-comp

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Rust. **License:** GPL v3.
- **Base:** Smithay.
- **Scope:** the compositor of System76's COSMIC desktop, which has both
  floating and auto-tiling per workspace.

## Ideas worth taking

- **Tiling and floating as a per-workspace toggle,** with windows dragged
  between tiles. consider (P1, 0.0.4).
- **Window stacks (tabs) inside the tiling tree.** consider (after P3).
- **Explicit focus-follows-mouse, cursor-follows-focus and similar as
  simple settings.** adopt (P2).

## Things to avoid

- **Tight coupling to its own desktop shell.** Our shell is Quickshell, and
  it talks to us through IPC.

## Where to look

- `src/shell/layout/tiling/`
