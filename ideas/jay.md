# Jay

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Rust. **License:** GPL v3.
- **Base:** its own implementation of the Wayland protocol and backend,
  with no wlroots or libwayland.

## Ideas worth taking

- **Proof that a compositor can own its whole stack,** including the wire
  protocol, KMS, input and a Vulkan or GL renderer. consider as a
  reference (P6).
- **Its own Wayland wire-protocol implementation.** consider (P6, 0.6.x).

## Things to avoid

- Nothing recorded yet.

## Where to look

- `src/wire/`, `src/backends/`
