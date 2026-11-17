# sway

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C. **License:** MIT.
- **Base:** wlroots (sway's developers started wlroots).
- **Scope:** an i3-compatible tiling compositor with manual tree tiling:
  containers, splits, tabs and stacks.

## Ideas worth taking

- **A precise IPC with subscriptions.** adopt the idea (P3, 0.2.3).
  - The i3 IPC (`swaymsg`, `get_tree`, event subscriptions) is the model
    every bar and script expects.
  - We keep the idea and use a simpler line format.
- **Output and input configuration by name and identifier.** adopt (P2).
  - For example `output DP-2 mode ...` and `input type:keyboard ...`.
- **Criteria-based window rules.** adopt (P2, 0.1.3).
  - `[app_id="foot"] floating enable` maps cleanly onto Lua matching.
- **Conservative scope.** consider.
  - sway refuses features outside its remit, which keeps it stable.
- **Server-side decorations and titles for tabbed containers.** consider.

## Things to avoid

- **Manual tiling as the only mode.** Fred wants dynamic tiling.
- **i3 config syntax.** It is our model for a tidy config, but we use Lua.

## Where to look

- `sway/tree/` (container tree)
- `sway/ipc-server.c`
- `sway/commands/`
