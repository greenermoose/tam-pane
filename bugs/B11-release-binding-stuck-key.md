# B11: Release-bound shortcuts leave keys held in clients

- **Status:** Fix available
- **Severity:** high
- **Affected versions:** Confirmed on 0.4.0. Release bindings date to 0.2.8.
- **Fixed in:** 0.4.1
- **Reported:** 2026-11-27, Fred

## Reproduction and cause

With a release-bound Meta + Alt + slash macro in a terminal, press the
chord and release slash first. The compositor forwards the slash press
and consumes the release. The terminal can keep repeating slash after
the modifiers are lifted. A headless client received one slash press,
zero slash releases, and both events of the synthetic paste chord.

## Fix and verification

Reserve the key when a release binding matches at press time. Run the
action once on the physical release and omit the held shortcut key
from focus-enter events. The headless input suite covers both release
orders, ordinary slash, focus changes, config reload, lock transitions,
and paired dictation bindings. Physical verification awaits Tamlinux Test.
