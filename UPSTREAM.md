# Code taken from other projects

tam-pane is written fresh. When another project has code that works well,
we read it, then write it into tam-pane's own modules in C and in our own
style. This file lists every such piece: where it came from, what changed,
and why it earned its place. Ideas taken without code go in `ideas/`
instead.

When you take something new, add a row here in the same change.

**Reference clones.** These are kept outside this repository and are
updated with `tools/pull-references` (see `ideas/sources`):

- MangoWM: `~/Code/mango`, `https://github.com/mangowm/mango`, branch `main`.
  Read at commit `5ba74a95` (2026-11-17).
- Hyprland: `~/Code/hyprland`, `https://github.com/hyprwm/Hyprland`, branch
  `main`. A shallow clone, read at commit `f3e0768` (0.57.0 development,
  2026-11-17).

## Pieces taken

### Dwindle tree

- **tam-pane files:** `src/layout/dwindle.c`, `include/tam-pane/layout/dwindle.h`
- **Source:** MangoWM `src/layout/dwindle.c` (`dwindle_insert`,
  `dwindle_remove`, `dwindle_assign`, `dwindle_swap_clients`), commit
  `5ba74a95`, GPL-3.0-or-later.
- **How:** Rewritten. It keeps the same binary tree: inner nodes split,
  leaves hold windows, and a removed leaf's sibling takes its parent's
  place. Removed: the globals `server` and `config`, the static
  locked-node pointers, direct `Client` and `wlr_box` use, and resize
  calls inside the layout pass. Items are opaque pointers and placement
  goes through a callback, so the tree is tested without a display. The
  gap arithmetic is simplified.
- **Why:** The algorithm is small and correct, and it is exactly what
  dynamic tiling needs.

### Dwindle split rules

- **tam-pane files:** `src/layout/dwindle.c` (`tp_dwindle_auto_split`,
  `tp_dwindle_smart_split`, and re-choosing an unlocked split's
  orientation at layout time)
- **Source:** Hyprland
  `src/layout/algorithm/tiled/dwindle/DwindleAlgorithm.cpp`
  (`split_width_multiplier`, `smart_split`, `preserve_split`), commit
  `f3e0768`, BSD-3-Clause.
- **How:** Rewritten in C. The slope test `abs(dy/dx) < h/w` is multiplied
  out, so a zero delta never divides. Hyprland's ratio runs from 0.1 to
  1.9 around 1.0; ours is the first child's share, from 0.05 to 0.95.
- **Why:** These rules are why Hyprland's tiling feels right, and users
  coming from Hyprland expect them.

### Tag view model

- **tam-pane files:** `src/tags.c`, `include/tam-pane/tags.h`
- **Source:** MangoWM `include/mango/manage/monitor.h` (`tagset[2]`,
  `seltags`) and `include/mango/manage/client.h` (`VISIBLEON`), commit
  `5ba74a95`, GPL-3.0-or-later. The design is dwm's (MIT), carried through
  dwl.
- **How:** Rewritten as pure functions over a small `tp_tagview` struct.
  Showing an empty set, and toggling the last tag off, are both refused in
  one place. Mango's per-tag slot 0 for multi-tag views became
  `tp_tags_slot()`.
- **Why:** Tags are a core tam-pane feature, and this model has held up
  since dwm.

### Layer-shell protocol file

- **tam-pane file:** `protocols/wlr-layer-shell-unstable-v1.xml`
- **Source:** MangoWM `protocols/` (a wlroots protocol file), commit
  `5ba74a95`. Copyright 2017 Drew DeVault, under a permissive notice kept
  in the file.
- **How:** Copied unchanged.
- **Why:** Panels, bars and launchers need it (used from 0.0.5).

## Compared with, not taken

- The overall shape of `src/server.c`, `src/monitor.c`, `src/client.c` and
  `src/input/` follows wlroots' own `tinywl` example (public domain), as
  every wlroots compositor does. It was checked against dwl and MangoWM's
  `src/main.c`, but none of their structure was copied.
