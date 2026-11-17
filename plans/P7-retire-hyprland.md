# P7: Retire Hyprland (1.0.0)

**Status:** not started.

## Goal

tam-pane is the only Tamlinux compositor, and Hyprland is removed from the
system.

## Versions

- **1.0.0** Hyprland, its plugins and its packages are removed through a
  Tamlinux candidate. The tam-pane configuration and IPC are declared
  stable.

## Acceptance

- At least one Tamlinux release has run tam-pane as the daily session with
  no fallback to Hyprland.
- Every step moves only on Fred's Test, Run and Release words. Removing
  packages is a privileged step with its own approval and rollback point.

## Dependencies

- Removes: Hyprland and its packages from the Tamlinux package set.

## Risks

- **No fallback after removal.** The rollback point (a Snapper snapshot
  and the previous Home Manager generation) is recorded first.

## Open points

- None.
