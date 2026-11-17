# Wayfire

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C++. **License:** MIT.
- **Base:** wlroots.
- **Scope:** a 3D-effects compositor (cube, wobbly windows) built almost
  entirely from plugins, with an optional tiling plugin.

## Ideas worth taking

- **Effects as separate units over a stable render interface.** adopt
  the principle (P5).
  - Our effects module must be removable without touching core window
    management.
- **A "render manager" that owns damage and frame scheduling.** consider
  (P5).

## Things to avoid

- **Shared-object plugins.** These carry the same ABI fragility as
  Hyprland's.

## Where to look

- `src/core/`, `src/api/wayfire/` (plugin API)
- `plugins/tile/`
