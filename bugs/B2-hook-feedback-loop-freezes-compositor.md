# B2: a hook that re-triggers itself freezes the compositor

- **Status:** Verified
- **Severity:** critical
- **Affected versions:** 0.1.2 to 0.1.5
- **Fixed in:** 0.1.6
- **Reported:** 2026-11-21, by Claude Code (claude-opus-5-5)
- **Found by:** R1 (finding 2)

## What happens

A hook whose action raises its own event again never lets the event loop
return. Two examples:

- a `tag.change` hook that views another tag;
- a `window.focus` hook that calls `tp.dsp.focus('next')()`.

Each hook call stays within its watchdog budget, so nothing cuts it off.
Frames, input and SIGTERM all stop, so the session is lost.

## What should happen

The compositor keeps handling input, and the runaway stops on its own
with a warning.

## Steps to reproduce

1. In `init.lua`:
   `tp.on('tag.change', function(ev)
   tp.dsp.tag('view', ev.tag == 6 and 7 or 6)() end)`
2. Press Meta+6.

Automated: the cascade case in `tests/test_input_headless.c`. Built
against 0.1.5, the test's alarm fires with "hung (event loop never
returned)".

## Cause

`drain_events_idle` in `src/events.c` re-added itself with
`wl_event_loop_add_idle` when hooks had queued more events. libwayland's
`wl_event_loop_dispatch_idle` keeps running idle sources until none are
left, so a newly added source runs in the same pass and epoll is never
reached.

## Fix and verification

The first batch still drains on the idle loop. Events raised while hooks
run are queued without a new idle source and drained by a 1 ms timer, so
epoll runs between batches. Each queued event carries its cascade depth.
Past `TP_EVENT_CASCADE_MAX` (16) rounds it is dropped, and one warning is
logged. Regression test: the cascade case above. `make test` passes, and
the ASan build of the headless test is clean. Fred checked 0.1.6
nested with `tools/demo-0.1.6` on 2026-11-21 (commit 0735f35, binary
SHA-256
`4f292cbbb9933cab6370cf81d2c3c74815ea6fe205de9d63320e6f5bdd898080`): the tag
ping-pong and focus spin each stopped with one warning, and input kept
working.
