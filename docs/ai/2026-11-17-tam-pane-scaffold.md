# tam-pane scaffold and 0.0.1 framework

- **Date:** 2026-11-17
- **CLI tool:** Claude Code (`claude`) `2.1.326`
- **Model:** `claude-opus-5-5`
- **Commit:** `d840afb`
- **Transcript:** Retained privately by the author.
- **Authorship:** AI-assisted design, implementation, documentation and
  verification, directed by Fred.

## Prompts

The opening request, verbatim:

> I'd like to replace hyprland with tam-pane, my own window manager /
> compositor. But I'd like to base tam-pane on MangoWM. I've got a clone of
> MangoWM at ~/Code/mango. Help me do the following:
>
> 1) Create an empty repo at ~/Code/tamlinux/tam-pane and get git working in it.
> 2) In tam-pane, create a plain text README that explains the project: create our own Wayland windowing system, taking the best ideas from MangoWM, Hyprland, sway, and every other window manager we can find
> 3) Create an ideas folder with a README-ideas plain text file explaining that this is where we keep the ideas we find from other window managers.
> 4) In ideas, create a file for each of the window managers we're tracking for ideas, including hyprland and MangoWM
> 5) Create an arch folder with a README-arch plain text file explaining that this is where we document the system architecture, including data structures and algorithms, we use for tam-pane
> 6) Carefully evaluate the mango repo to understand its dependencies and how mango works
> 7) Port over the code from that repo into tam-pane so that we can build our own window manager
> 8) A markdown ROADMAP.md file that sets milestones with semvers attached, e.g. 0.0.1 initial framework established and code compiles
> 9) Create a plans folder, including a plaintext README-plans file, with work phases to achieve each milestone. Each plan should be labeled P1-<name>.md, P2-<name>.md, etc. A phase can include several semvers, e.g. Phase 1 might be all 0.0.x versions.
> 10) Create a bugs folder, including plaintext README-bugs, for bug tracking. Each bug should be labeled B1-<name>.md, etc.
>
> The goal of tam-pane is to have a window manager that works on an Arch Linux rootstock to provide equivalent functionality to MangoWM and hyprland, but that has no other dependencies besides standard C libraries and libtam. A few things that tam-pane should provide:
>
> 1) Dynamic tiling like hyprland
> 2) tags on windows like MangoWM
> 3) lua scripting built in like hyprland

Later directions from Fred in the same session, verbatim:

> LICENSE should GPL v3

> I don't want all of mango's code in tam-pane. I just want to use mango for ideas. We can copy over only what we need. Keep the rest of it out of tam-pane.

> The idea is to build a very clean system from the ground up. I want to learn from mango and hyprland, not duplicate their whole code structure. Copy what they have working well; ignore what they don't do well. Our clean should be tight, clean, and excellent.

> We can refactor and build our code exactly as we want. We want to jetison unnecessary dependencies. I want to use Makefiles, not drag in a whole 'nuther build system. Make sure the plan understands the goal here: I'm not forking and cloning, I'm starting fresh but building on the code base mango and hyprland have created.

> Pull changes in repos for other window managers so we stay current with their latest ideas, bug fixes, etc.

Fred also chose: the end state is libc, libtam and Mesa for the GPU; Lua
5.4 sources kept in the tree; a branch named `dev`; default install prefix
`$HOME/.local`. Tamlinux timing: tam-pane is developed under Tamlinux 0.6.x
and dogfooded in 0.7.x, replacing the earlier plan to move to Sway.

## Key decisions and implementation notes

- **Fresh code, not a fork.** Only these were taken, rewritten and listed in
  `UPSTREAM.md`:
  - MangoWM's dwindle tree and tag view model;
  - Hyprland's dwindle split rules;
  - the layer-shell protocol file.
- **No globals.** One `tp_server` is passed everywhere. Each module owns
  and removes its own listeners, so teardown never trips wlroots' destroy
  assertions.
- **The dwindle tree is pure.** It uses opaque items, keeps its settings
  in the tree and places windows through a callback. This makes it unit
  testable, and Lua layouts can reuse it later.
- **Bindings match the unshifted key symbol,** so "Meta+Shift+1" is
  written with "1" on any keyboard layout.
- **Configures only on a real size change,** which saves client redraws.
- **The event loop never blocks.** `tp_spawn` double-forks, so the
  compositor never waits on a child.
- **Dependencies:** scenefx, pango, cairo, cJSON, pcre2, xcb, meson and
  ninja are dropped. The direct dependencies are wlroots 0.20,
  wayland-server, xkbcommon, libtam (static), libc and libm.
- **Reference clones** of MangoWM and Hyprland are kept current with
  `tools/pull-references`, from the list in `ideas/sources`.

## Verification

- `make clean && make`: no warnings, with `-Wall -Wextra -Wpedantic`.
- `make test`:
  - `test_tags` (28 checks) and `test_dwindle` (52 checks) pass.
  - The headless smoke test passes. The compositor starts, opens its
    socket and exits 0 on SIGTERM. With foot clients it maps two windows,
    survives one closing, and exits 0 with the other still connected.
- `readelf -d build/tam-pane` lists libwlroots-0.20, libwayland-server,
  libxkbcommon, libm and libc only.
- Not yet tested: interactive use, nested or on a real session. That
  happens at 0.0.2, run by Fred.
