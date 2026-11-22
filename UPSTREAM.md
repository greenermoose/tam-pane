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
  `dwindle_remove`, `dwindle_assign`, `dwindle_swap_clients`,
  `dwindle_move_next_to`, `get_block_path_and_ratios`,
  `count_block_items`, and `dwindle_manual_split` proportional insert and
  remove arithmetic), commit `5ba74a95`, GPL-3.0-or-later.
- **How:** Rewritten. It keeps the same binary tree: inner nodes split,
  leaves hold windows, and a removed leaf's sibling takes its parent's
  place. Added directional move (`tp_dwindle_move_dir`) and optional
  proportional split rebalancing (`proportional_split`) as pure functions
  without compositor state. Removed: the globals `server` and `config`,
  the static locked-node pointers, direct `Client` and `wlr_box` use, and
  resize calls inside the layout pass. Items are opaque pointers and
  placement goes through a callback, so the tree is tested without a
  display. The gap arithmetic is simplified.
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

### wlroots protocol files

- **tam-pane files:** `protocols/wlr-layer-shell-unstable-v1.xml`,
  `protocols/wlr-foreign-toplevel-management-unstable-v1.xml`,
  `protocols/wlr-output-power-management-unstable-v1.xml`
- **Source:** MangoWM `protocols/` (wlroots protocol files), commit
  `5ba74a95`. Permissive notices kept in each file.
- **How:** Copied unchanged.
- **Why:** Panels/bars/launchers (`layer-shell`, 0.0.5), window lists
  (`foreign-toplevel`, 0.2.0), and DPMS output power (`output-power`,
  0.2.0) need them.

### Lua 5.5.1 interpreter

- **tam-pane files:** `third_party/lua/*.[ch]`
- **Source:** Lua 5.5.1 release tarball
  (`https://www.lua.org/ftp/lua-5.5.1.tar.gz`), SHA-256
  `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`,
  MIT License (notice kept in `third_party/lua/lua.h`).
- **How:** Copied from `lua-5.5.1/src/`, excluding the standalone CLI
  entry points (`lua.c`, `luac.c`, `onelua.c`). Every file is unchanged
  except `lstrlib.c`, which carries one tam-pane patch (below). Built
  statically with `-fvisibility=hidden`, `-DLUA_USE_POSIX` and
  `-DLUAI_PATTERNCHECK=tp_lua_pattern_check` (without `-DLUA_USE_DLOPEN` or
  `-rdynamic`), compiling only the core VM and safe standard libraries
  (omitting `linit.c`, `liolib.c`, `loslib.c`, `loadlib.c`, `ldblib.c`).
- **Patch (0.1.8, R1 finding 8):** Lua's pattern matcher (`string.find`,
  `match`, `gmatch`, `gsub`) runs in C, where the watchdog's count hook
  never fires, so a pathological pattern could run for hours. In
  `lstrlib.c`:
  - `MatchState` gains a `steps` counter, which `prepstate` zeroes;
  - `match()` calls `luai_patterncheck(L)` every 4096 steps;
  - `luai_patterncheck` expands to the embedder's `LUAI_PATTERNCHECK`
    when that is defined, and to nothing otherwise.

  tam-pane defines it as `tp_lua_pattern_check` (`src/lua.c`), which
  applies the watchdog's wall-clock deadline. Every patched line is
  marked `tam-pane patch`. To update Lua, copy the new release and
  re-apply these few lines. Retire the patch if Lua ever gains its own
  way to interrupt the matcher.
- **Why:** In-process Lua 5.5 scripting for configuration, bindings,
  hooks, rules, and layouts with no external library dependency and no
  symbol clash with `libinput`'s `liblua 5.4`.

### Lua watchdog pattern

- **tam-pane file:** `src/lua.c` (`watchdog_hook`, `guarded_pcall`)
- **Source:** Hyprland `src/config/lua/LuaCaller.cpp` (`guardedPCall`,
  `watchdogHook`), commit `f3e0768`, BSD-3-Clause.
- **How:** Rewritten in C11 for `struct tp_lua` with no globals, saving
  and restoring hook state across nested calls, and enforcing both an
  instruction budget and a `CLOCK_MONOTONIC` wall-clock deadline. tam-pane
  adds two things of its own:
  - since 0.1.8 the deadline also reaches into the pattern matcher,
    through the Lua patch above (`tp_lua_pattern_check`);
  - a counting allocator of tam-pane's own (`tp_lua_alloc`) caps a Lua
    state at 64 MiB inside guarded calls.
- **Why:** Prevents runaway Lua loops, runaway pattern matching and
  runaway allocation from stalling or killing the compositor.

## Compared with, not taken

- The overall shape of `src/server.c`, `src/monitor.c`, `src/client.c` and
  `src/input/` follows wlroots' own `tinywl` example (public domain), as
  every wlroots compositor does. It was checked against dwl and MangoWM's
  `src/main.c`, but none of their structure was copied.
