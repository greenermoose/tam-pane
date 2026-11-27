# B10: A display that returns after a suspend forgets its tags and layout

- **Status:** Fix available
- **Severity:** medium
- **Affected versions:** 0.0.5 to 0.2.12
- **Fixed in:** 0.2.13
- **Reported:** 2026-11-26, by Fred ("I lost my window placements")
- **Found by:** Fred, after a suspend on bosgame

## What happens

A suspend that drops and re-adds a display (on bosgame, the HP on
HDMI-A-1 and the Dell on DP-1) brings each returning display back on
tag 1, with no former tag for Meta+Tab, whatever it showed before. Its
tiled layouts start again from nothing, so split sizes and per-tag
layout choices are lost, and windows that float come back moved and
sometimes smaller. The windows themselves keep their tags and return to
their display, so a window can seem to be gone: it is on a tag the
display is no longer showing.

## What should happen

A display that returns shows the tags it showed before, with Meta+Tab
going to the same former tag, the same tiled layouts, and its windows'
floating places where they were.

## Steps to reproduce

1. With two or more displays, show tag 3 on one of them, then tag 1,
   with windows tiled there and one floating.
2. Suspend so that the display is dropped and re-added (the journal
   shows "output NAME: WxH at X,Y" for it after resume).
3. The display shows tag 1 with no former tag; the floating window has
   moved.

Journal: boot of 2026-11-26, resume at 20:33:27; tam-pane re-added
HDMI-A-1 at 20:33:28 and DP-1 at 20:33:35.

Automated: in `tests/test_input_headless.c`, the hot-plug part checks
that the second output returns on its former tags, with the same
layout tree and the floating place it had. Its tag and layout checks
fail on 0.2.12.

## Cause

`on_destroy` in `src/monitor.c` freed the monitor's tag view and
cleared its layouts. `on_new_output` started the returning monitor at
tag 1 with fresh layouts. Floating places were fitted onto the monitor
that took the windows in the meantime and then carried back, so a
smaller stand-in shrank them.

## Fix and verification

- `src/monitor.c`: a removed monitor's tag view and layouts are kept
  under its output name (`struct tp_parked_monitor`, at most 8) and
  given back when an output with that name returns. Windows that left
  for good are dropped from the kept layouts on return. Removing an
  output is now logged.
- `src/client.c`: a window that closes, or is moved to another monitor
  on purpose, is dropped from every kept layout, so a kept layout never
  holds a freed window.
- Each displaced window remembers its floating place relative to its
  home output, and gets it back there.
- Regression tests: the return case above, and a window that closes
  while its monitor is away. Turning off each part of the fix makes its
  check fail. The headless suite is clean under AddressSanitizer.
- Fred's check: not yet.
