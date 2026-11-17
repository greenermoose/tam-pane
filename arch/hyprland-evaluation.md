# Hyprland evaluation

- **Read:** `~/Code/hyprland` at commit `f3e0768` (main branch, 0.57.0
  development), 2026-11-17. Fred runs 0.56.2.
- **Purpose:** the same as for MangoWM. The ideas list lives in
  `ideas/hyprland.md`.

## Shape

- About 126,000 lines of C++ in `src/`.
- Hyprland no longer uses wlroots. Its backend library is Aquamarine
  (`CMakeLists.txt:166`), with hyprutils, hyprlang, hyprcursor and
  hyprgraphics alongside.
- Each protocol has its own file in `src/protocols/`. That habit is good,
  and tam-pane follows it in P3.

## Dwindle layout

- **File:** `src/layout/algorithm/tiled/dwindle/DwindleAlgorithm.cpp`
  (866 lines).
- **Tree:** nodes have two children, a `splitTop` flag (stacked when
  true) and a `splitRatio` from 0.1 to 1.9, where 1.0 is an even split
  and the first child gets `w / 2 * ratio`.
- **Orientation:** unless `preserve_split` or `smart_split` is set, every
  relayout recomputes it as `box.h * split_width_multiplier > box.w`.
- **Insert:**
  - New windows split the focused window.
  - With the pointer over its first half, the new window goes first,
    unless `force_split` says otherwise.
  - `smart_split` compares the pointer's slope from the centre against
    the box's proportions, which picks one of the four edges.
- **Directional override:** `m_overrideDirection` lets the user pick
  where the next window opens. This is bspwm's preselection.
- **Taken:** the orientation rule, the width bias, smart split and
  preserve split. Our ratio is the first child's share from 0.05 to 0.95,
  which is clearer.

## Lua configuration

- **Files:** `src/config/lua/` (ConfigManager 1,400 lines, plus bindings,
  layouts, objects and types).
- **Version:** Lua 5.5 (`CMakeLists.txt:291`).
- **Dispatchers are values:** `hl.dsp.window.close()` returns something a
  binding stores (`LuaBindingsDispatcherUtils.cpp:120`), and the
  `hl.dsp` table is built in `LuaBindingsDispatchers.cpp:1420`.
- **Events:** `hl.on("name", fn)` (`LuaEventHandler.cpp:30`).
- **Layouts written in Lua:** `LuaLayoutProvider`, `LuaLayoutContext` and
  `LuaLayoutTarget` let a script position windows.
- **Watchdog:**
  - `guardedPCall` (`ConfigManager.cpp:458`) sets a `LUA_MASKCOUNT` hook
    every 10,000 instructions (`ConfigManager.hpp:107`) and checks a
    steady-clock deadline.
  - A runaway loop becomes a Lua error.
- **Libraries:** `luaL_openlibs` (`ConfigManager.cpp:494`) opens every
  standard library, including `io` and `os`.

## Plugins

- `src/plugins/PluginSystem.cpp:100` rejects plugins whose
  `HYPRLAND_API_VERSION` differs, and `__hyprland_api_get_hash()` ties
  each plugin to one exact build.
- Plugins reach into C++ internals through function hooks
  (`HookSystem.cpp`).

## What works well

- **The dwindle behaviour.** It is the reason Fred uses dynamic tiling.
  Taken.
- **Dispatchers as values in Lua.** It gives one way to describe an action
  for bindings, IPC and scripts. Planned (P2, 0.1.1).
- **A watchdog on every Lua call.** Planned (P2, 0.1.0).
- **Protocol per file.** Planned (P3).
- **The `hyprctl` request and event sockets.** Planned (P3, 0.2.3), with a
  line format.

## What not to repeat

- **Plugins locked to the exact build.** Every Hyprland update breaks
  them, and Fred keeps a recovery procedure just for that.
  tam-pane extends through Lua and IPC only.
- **Blocking calls reachable from Lua.**
  - With `io.popen` and `os.execute` open, a script can block the event
    loop.
  - The instruction watchdog cannot interrupt a blocked C call. This is
    what froze the session on 2026-10-31.
  - tam-pane opens only safe libraries and offers a non-blocking
    `tp.spawn`.
- **Weight.** It needs many helper libraries and C++ machinery, and its
  configuration values are fetched through static lookups spread across
  the code.
