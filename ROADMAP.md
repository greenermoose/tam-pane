# tam-pane roadmap

`tam-pane` is Tamlinux's tiling Wayland compositor with Lua configuration,
written in C17 on top of `libtam`.

- **Current version:** `0.4.2` (branch `dev`)
- **Progress:** 3 of 7 phases done (P1–P3 done; P4 hardware Test pending; P5 in progress)
- **Versioning:** Semantic versioning; during 0.x, minor marks a phase and
  patch marks milestones or fixes within it.

## Phase summary

| Phase | Versions | Goal | Plan | Status |
| --- | --- | --- | --- | --- |
| **P1** | 0.0.x | Foundation: headless/nested compositor, tags, dwindle tiling, layer-shell, multi-monitor | [P1-foundation.md](plans/P1-foundation.md) | Done |
| **P2** | 0.1.x | Embedded Lua 5.5 config, bindings, hooks, window rules, live reload, and R1 hardening | [P2-lua.md](plans/P2-lua.md) | Done (2026-11-22) |
| **P3** | 0.2.x | Daily-driver parity: shell/capture/text protocols, IPC, layouts, chords, monitor/input polish | [P3-daily-parity.md](plans/P3-daily-parity.md) | Done (2026-11-26) |
| **P4** | 0.3.x | Dogfood in Tamlinux 0.7 with Quickshell, `fred.*` plugins, and headless desktop tests | [P4-dogfood-tamlinux-0.7.md](plans/P4-dogfood-tamlinux-0.7.md) | In progress |
| **P5** | 0.4.x | Own GLES visuals on Mesa: borders, rounded corners, shadows, blur, animations | [P5-visuals.md](plans/P5-visuals.md) | In progress |
| **P6** | 0.5.x–0.7.x | Own the stack: replace libinput/xkbcommon, libwayland-server, and wlroots via `libtam` + Mesa | [P6-own-the-stack.md](plans/P6-own-the-stack.md) | Planned |
| **P7** | 1.0.0 | Retire Hyprland; `tam-pane` becomes Tamlinux's sole compositor (only on Fred's word) | [P7-retire-hyprland.md](plans/P7-retire-hyprland.md) | Planned |

## Decided

Fred decided these on 2026-11-17:

- `tam-pane` is developed while the workstation runs Tamlinux 0.6.x on
  Hyprland.
- Dogfooding starts with Tamlinux 0.7.x, using `tam-pane` 0.3.x.
- This replaces the earlier plan to move Tamlinux 0.7 to Sway.
- Removing dependencies (Phase P6) continues while `tam-pane` is in daily
  use.

## Phases and milestones

### Phase P1: Foundation (0.0.x) — [P1-foundation.md](plans/P1-foundation.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.0.1** | Plain Make build, headless backend, client mapping, tag and dwindle unit tests | Done |
| **0.0.2** | First nested run under Hyprland: focus, keyboard, pointer, compiled-in Meta bindings | Done |
| **0.0.3** | Interactive tags: view, toggle, move window to tag, previous view | Done |
| **0.0.4** | Dynamic dwindle tiling, keyboard/mouse resize, floating windows, fullscreen | Done |
| **0.0.5** | `layer-shell` and multiple monitors (Quickshell bar and cross-monitor moves) | Done |

### Phase P2: Lua and R1 hardening (0.1.x) — [P2-lua.md](plans/P2-lua.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.1.0** | In-tree Lua 5.5, `~/.config/tam-pane/init.lua` settings, instruction budget guard | Done |
| **0.1.1** | Key bindings and dispatchers from Lua (compiled-in table removed) | Done |
| **0.1.2** | Event hooks: window open/close/focus, tag change, monitor add/remove | Done |
| **0.1.3** | Window rules matching app id and title for tags, floating, size, and monitor | Done |
| **0.1.4** | Live config reload with error reporting and last-good config preservation | Done |
| **0.1.5–0.1.19** | Review R1 hardening (`reviews/R1-code-review-0.1.4.md`): B1–B9 fixes, privileged-protocol filter, explicit focused window | Done (2026-11-22) |

### Phase P3: Daily-driver parity (0.2.x) — [P3-daily-parity.md](plans/P3-daily-parity.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.2.0** | Shell protocols: foreign-toplevel, ext-workspace, session-lock, idle notify/inhibit, output power | Done (2026-11-22) |
| **0.2.1** | Screens and capture: screencopy, image-copy, output management, gamma, fractional scale, cursor-shape | Done (2026-11-22) |
| **0.2.2** | Text and activation: text-input, input-method, xdg-activation, primary selection, decorations | Done (2026-11-22) |
| **0.2.3** | IPC socket, `tam-pane msg` client, and `SIGHUP` shutdown | Done (2026-11-22) |
| **0.2.4** | Deferred per-monitor arrange on idle (R1 finding 15) | Done (2026-11-23) |
| **0.2.5** | Master-stack, scroller, monocle layouts, and scratchpad | Done (2026-11-23) |
| **0.2.6** | Opt-in XWayland behind `XWAYLAND=1` (default `XWAYLAND=0`, zero-`xcb`) | Done (2026-11-23) |
| **0.2.7** | One-handed chord composer/overlay, shell IPC queries, DPMS/app/keyboard dispatches and events | Done (2026-11-25) |
| **0.2.8** | Monitor, XKB, include, mouse/keycode/release bind, `send_key`, and capture support | Done (2026-11-26) |
| **0.2.9** | Start programs and serve IPC through `libtam` | Done (2026-11-26) |
| **0.2.10** | Recover from GPU renderer loss (R1 finding 11) | Done (2026-11-26) |
| **0.2.11** | Draw focused window border and launch configured locker | Done (2026-11-26) |
| **0.2.12** | Log per-phase startup timings and explicit keyboard chords with exact modifiers | Done (2026-11-26) |
| **0.2.13** | Restore monitor configuration after suspend (B10) | Done (2026-11-26) |
| **0.2.14** | Dispatch monitor mode and log every modeset | Done (2026-11-26) |
| **0.2.15** | Pointer focus, cross-display drag, and guarded macro paste | Done (2026-11-26) |
| **0.2.16** | `lock_state`/events, DPMS-while-locked, native gamma LUTs, screen freeze, and pixel sampling | Done (2026-11-27) |
| **0.2.17** | Monitor enable/disable, `devices` IPC, window pinning/targeting/pop geometry, and themeable focused/unfocused borders | Done (2026-11-27) |

### Phase P4: Dogfood in Tamlinux 0.7 (0.3.x) — [P4-dogfood-tamlinux-0.7.md](plans/P4-dogfood-tamlinux-0.7.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.3.0** | P4 dogfood baseline & R1-17 hardening (keyboard grouping, full Lua pattern validation, UTF-8 truncation, async nested scale, NULL checks); packaged with login session entry and Hyprland fallback in Tamlinux 0.7 | Done (2026-11-27) |
| **0.3.1** | Quickshell and `fred.*` plugins operate through `tam-pane` IPC; headless desktop tests switch from Sway (wired in Tamlinux 0.7.x; hardware Test pending) | In progress |
| **0.3.x** | Fixes from daily use, recorded in `bugs/` | Planned |

### Phase P5: Visuals (0.4.x) — [P5-visuals.md](plans/P5-visuals.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.4.0** | Own Mesa GLES2 visuals module (`src/visuals.c`): rounded corners, rounded borders, and analytical SDF drop shadows (plain scene path when off, zero idle frames; scene-rect borders already landed in `0.2.11`/`0.2.17`) | Done (2026-11-27) |
| **0.4.1** | Keep release-bound shortcut keys out of client input (B11) | Fix available |
| **0.4.2** | Confirm or cancel one-hand chords with the opening key | Fix available |
| **0.4.2** | Blur behind transparent windows and panels, with damage tracking | Planned |
| **0.4.3** | Animations for open, close, move, and tag change, off by default on battery | Planned |

### Phase P6: Own the stack (0.5.x–0.7.x) — [P6-own-the-stack.md](plans/P6-own-the-stack.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **0.5.x** | `libtam` input (evdev and keymaps) replaces libinput and xkbcommon | Planned |
| **0.6.x** | `libtam` Wayland wire protocol replaces libwayland-server | Planned |
| **0.7.x** | `libtam` KMS/session backend plus Mesa GBM/EGL replaces wlroots (goal: libc, `libtam`, Mesa) | Planned |

### Phase P7: Retire Hyprland (1.0.0) — [P7-retire-hyprland.md](plans/P7-retire-hyprland.md)

| Version | Milestone | Status |
| --- | --- | --- |
| **1.0.0** | `tam-pane` becomes the only Tamlinux compositor and Hyprland is removed (only on Fred's word) | Planned |

## How a milestone is done

A milestone is done when its acceptance checks in the phase plan pass and
Fred has tried the build. When a milestone lands, bump `VERSION`, update
the phase plan's status, and update this roadmap in the same change.
