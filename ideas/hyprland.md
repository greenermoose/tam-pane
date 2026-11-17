# Hyprland

## What it is

- **Language and size:** C++, about 126,000 lines in `src/`.
- **License:** BSD 3-Clause.
- **Base:** its own backend library (Aquamarine) and helper libraries
  (hyprutils, hyprlang and others), no longer wlroots.
- **Scope:** dynamic tiling with heavy visual effects. Since 0.56 it has a
  Lua configuration, and it has a C++ plugin system.
- **Notes from source:** clone `~/Code/hyprland`, read at commit
  `f3e0768`. Fred runs 0.56.2 every day.

## Ideas worth taking

- **Dwindle split rules.** adopt (P1, 0.0.1).
  - Split along the longer side, with `split_width_multiplier` as a bias.
  - With `smart_split`, the pointer's direction from the window's centre
    picks the edge.
  - With `preserve_split`, a split keeps its first orientation.
  - Source: `src/layout/algorithm/tiled/dwindle/DwindleAlgorithm.cpp`.
- **Lua dispatchers as values.** adopt (P2, 0.1.1).
  - `hl.dsp.window.close()` returns a dispatcher that a binding stores, so
    a binding is either a dispatcher or a plain Lua function.
  - Source: `src/config/lua/bindings/LuaBindingsDispatchers.cpp`.
- **Lua watchdog.** adopt (P2, 0.1.0).
  - `guardedPCall` installs a `lua_sethook` count hook every 10,000
    instructions, with a wall-clock deadline, so a runaway script errors
    out instead of hanging the compositor.
  - Source: `src/config/lua/ConfigManager.cpp`, around line 458.
- **Event hooks.** adopt (P2, 0.1.2).
  - `hl.on("event", fn)`, with each callback run under the watchdog.
  - Source: `src/config/lua/LuaEventHandler.cpp`.
- **Layouts written in Lua.** consider (after P2).
  - `LuaLayoutProvider` lets a script place windows. Our dwindle tree is
    already pure, so a Lua layout could reuse it.
  - Source: `src/config/lua/layout/`.
- **Special workspaces.** consider (P3, as the scratchpad).
  - A per-monitor overlay workspace that you toggle.
- **The `hyprctl` IPC.** adopt the idea (P3, 0.2.3).
  - A request socket plus an event socket. We use a line format rather
    than JSON, so no JSON library is needed.

## Things to avoid

- **Plugins locked to the exact build.**
  - `src/plugins/PluginSystem.cpp:100` refuses a plugin whose API version
    differs, and plugins also carry a build hash.
  - Every Hyprland update breaks them, and Fred keeps a recovery procedure
    just for that.
  - tam-pane extends through Lua and IPC, not loadable C++ objects.
- **Blocking calls in Lua.**
  - `luaL_openlibs` (ConfigManager.cpp:494) opens `io` and `os`, so
    `io.popen` and `os.execute` can block the compositor. That is how the
    2026-10-31 freeze happened.
  - The watchdog counts instructions and cannot interrupt a blocked C
    call.
  - tam-pane exposes only non-blocking calls.
- **Weight.** C++ templates, a large number of helper libraries, and
  configuration values fetched through static `CConfigValue` lookups
  scattered through the code. We keep settings in one small struct.
- **Lua 5.5.**
  - Hyprland requires 5.5 (`CMakeLists.txt:291`); tam-pane uses 5.4, by
    Fred's choice.
  - Not a fault, but configurations won't carry across unchanged.

## Where to look

- `src/layout/` (layouts, the drag controller, targets)
- `src/config/lua/` (Lua configuration, bindings, layouts)
- `src/managers/` (input, keybinds, animations)
- `src/render/` (render pass, shaders, blur)
- `src/protocols/` (each protocol in its own file, a good habit)
