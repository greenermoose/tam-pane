# B7: menus open off-screen after the program repositions them

- **Status:** Verified
- **Severity:** medium
- **Affected versions:** 0.0.1 to 0.1.14
- **Fixed in:** 0.1.15
- **Reported:** 2026-11-21, by Fred (his sandbox/demo-0.1.14 check)
- **Found by:** R1 finding 5 (first fix in 0.1.13 was incomplete)

## What happens

In Files (nautilus 51.1, GTK 4.24.1), nested, a right-click menu near
the screen edge runs off the screen. 0.1.13 keeps a popup on its
monitor when it first appears, but that was not enough.

## What should happen

Menus flip or slide so that they stay wholly on the monitor.

## Steps to reproduce

1. Open a popup on a window near the right edge of the monitor, with
   slide and flip allowed, that fits.
2. Send `xdg_popup.reposition` with rules that would put it past the
   edge.
3. The configure that answers the reposition puts it past the edge.

Automated: the reposition case after the edge-popup case in
`tests/test_input_headless.c`. It fails on 0.1.14.

## Cause

`xdg_popup.reposition` (xdg-shell version 3) gives a mapped popup new
rules. wlroots stores them, emits the popup's `reposition` signal and
configures on the next idle. A compositor must unconstrain the new
rules in that signal, as MangoWM (`handle_popup_reposition`) and
Hyprland do. tam-pane listened only to the first commit, for window
popups (`src/client.c`) and layer-surface popups (`src/layer.c`) alike.

GTK 4 uses reposition to move a popover once it knows its size. That
GTK took this path in Fred's run is inferred, not yet seen: nothing in
0.1.14 logged popup placements. 0.1.15 logs one debug line per placement
(`popup on <output>: box ..., placed ...`), and `sandbox/demo-0.1.15`
prints them.

## Fix and verification

- `src/client.c` and `src/layer.c` listen to each popup's `reposition`
  signal and unconstrain again, with the same box as on the first
  commit.
- Regression test: the case above. The test client now binds
  xdg_wm_base version 3 to send reposition. Layer-surface popups have
  no reposition case yet.
- Fred's check: he passed `sandbox/demo-0.1.15` on 2026-11-21 (commit
  c5ec224, binary SHA-256
  `8f1a2fec892aeedab09cc2d9030de8e82782a59a604fe415844faf937a94f163`):
  Files opened on the right and its menu near the bottom-right corner
  stayed on screen. That confirms the inferred cause, as 0.1.14 differed
  only in the reposition handling.
