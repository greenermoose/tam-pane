# B6: GTK 4 programs crash on a nested output (no presentation-time)

- **Status:** Fix available
- **Severity:** medium
- **Affected versions:** 0.0.1 to 0.1.13
- **Fixed in:** 0.1.14
- **Reported:** 2026-11-21, by Fred (his sandbox/demo-0.1.13 check)
- **Found by:** Fred's nested check of 0.1.13: Files opened and closed at
  once

## What happens

Nested under Hyprland, a GTK 4 program (Files, nautilus 51.1 with GTK
4.24.1) shows its window for a moment and dies with SIGFPE. The core
dump puts it in `gdk_wayland_surface_frame_callback`
(`gdk/wayland/gdksurface-wayland.c:306`) with `monitor_refresh = 0`.
The system journal for 2026-11-21, 21:57 to 21:59, has the two dumps.

## What should happen

GTK 4 programs run nested as they do on Hyprland.

## Steps to reproduce

1. Run tam-pane 0.1.13 on the Wayland backend (nested).
2. Start nautilus inside it. It crashes within a second or two.

Without a screen: run tam-pane on the Wayland backend inside a headless
tam-pane, both with `WLR_RENDERER=pixman`, and start nautilus in the
inner one on a session bus of its own. On 0.1.13 nautilus exits with
status 136 (SIGFPE); on 0.1.14 it is still running when a 12-second
timeout ends it.

## Cause

tam-pane did not offer `wp_presentation` (presentation-time). Without
it, GTK 4 times frames from the refresh rate of the output its window
is on, and divides by that rate. A nested output has no real mode.
wlroots' Wayland backend gives it custom modes with refresh 0, and
refuses a mode with any other refresh. The wl_output protocol allows 0
for virtual outputs, so GTK should not divide by it either. That is a
GTK bug; reporting it upstream is for Fred to decide. Hyprland offers
presentation-time, so GTK never takes this path there, and DRM outputs
have real refresh rates.

A first attempt reported a nominal 60 Hz for refresh-less outputs. It
passed the headless test, but nested the backend refused the mode and
the output never enabled. It was reverted before any commit.

## Fix and verification

- `src/server.c` creates the presentation-time global
  (`wlr_presentation_create`, version 2). The wlroots scene sends the
  feedback, as it does for viewporter and fractional-scale.
- Regression test: `tests/test_input_headless.c` checks that a client
  sees `wp_presentation`. It failed on 0.1.13.
- End-to-end, headless, as in Steps to reproduce: nautilus mapped at
  1248x688 on the inner output and kept running on 0.1.14; on 0.1.13 it
  died with SIGFPE.
- Fred's check: `sandbox/demo-0.1.14`, which also covers R1 finding 5.
