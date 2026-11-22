# B9: Meta+V floats a window at the layout's top-left, at its first size

- **Status:** Verified (one monitor; the multi-monitor part waits for P4)
- **Severity:** medium
- **Affected versions:** 0.0.4 to 0.1.16
- **Fixed in:** 0.1.17
- **Reported:** 2026-11-21, by Fred (step 2 of his sandbox/demo-0.1.16
  check)
- **Found by:** Fred's nested check; the headless tests never hit it

## What happens

A window that opened tiled (a foot terminal) floats with Meta+V at the
size it first asked for (about 700x500), at the top-left corner, over
the other windows. On more than one monitor, a window on the second or
third monitor floats onto the first one, while tam-pane still counts it
as on its own monitor. A tiled window moved to another monitor floats
back on the old one.

## What should happen

Fred chose on 2026-11-21: the first float is half the window's monitor,
centred on that monitor. Later floats return to the window's last
floating place, which follows the window when it moves to another
monitor.

## Steps to reproduce

1. Open a terminal; it tiles.
2. Press Meta+V. It floats at its own size in the top-left corner.

Automated: the "B9" case in the multi-monitor part of
`tests/test_input_headless.c`, with a 200x150 window mapped tiled on
the second monitor. On 0.1.16 its first float is 200x150 at (0, 0),
which is on the first monitor.

## Cause

`on_map` set `float_geom` to (0, 0, first size) and moved it onto the
monitor only for windows that open floating. `tp_client_set_floating`
replaced it only when it was smaller than 64x64, so the headless tests,
whose windows are 2x2, always got the half-size default and never saw
this. `tp_client_carry_float` moved only floating windows' places
between monitors.

## Fix and verification

- `src/client.c`: `on_map` gives a window that opens tiled a floating
  place of half its monitor, centred (`half_monitor_size`,
  `center_float`), and centres every window's floating place unless a
  rule gives coordinates. `tp_client_carry_float` carries tiled
  windows' places too. `tp_client_set_floating` treats a place that is
  off the window's monitor (a window that mapped with no monitor) as a
  first float.
- Regression test: the case above checks the first float on the second
  monitor, the return to a moved place, a float after a tiled move to
  the first monitor, and an off-monitor place. Its first two checks
  fail on 0.1.16. With only the `on_map` change, the move check fails,
  which shows the carry change is needed.
- Fred's check: he passed `sandbox/demo-0.1.17` on 2026-11-21 (commit
  3425435, binary SHA-256
  `e6690cd33e9bddd9d847dbd7930180096e50231ad16237a1bcd6e4e9547be785`).
  The demo asks whether the first float was centred at half size and a
  later float came back where he had dragged it. The multi-monitor part
  is the headless test and the P4 hardware checks.
