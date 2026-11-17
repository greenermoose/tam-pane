# river

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Zig. **License:** GPL v3.
- **Base:** wlroots.
- **Scope:** a dynamic tiling compositor with tags, where the layout is
  computed by a separate client program through a Wayland protocol.

## Ideas worth taking

- **Layouts outside the compositor.** consider (after P2).
  - A layout generator receives window counts and returns boxes. Our Lua
    layouts aim at the same freedom without another process.
- **Tags as 32-bit masks, also used for "focused" and "occupied" in
  status protocols.** adopt (P3, ext-workspace mapping).
- **Configuration as an executable `init` script** that sends commands
  over IPC. consider.
  - It is simple and makes the IPC complete by necessity.

## Things to avoid

- **A custom protocol for every extension.** We prefer the standard
  ext-* and wlr-* protocols that bars already speak.

## Where to look

- `river/` (Zig sources)
- `protocol/river-layout-v3.xml`
