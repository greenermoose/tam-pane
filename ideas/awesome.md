# awesome

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C core with Lua. **License:** GPL v2.
- **Base:** X11 (XCB).
- **Scope:** a framework window manager where the bar, widgets, layouts
  and rules are all written in Lua. It uses dwm-style tags.

## Ideas worth taking

- **The closest model for tam-pane's Lua side:** tags, clients and screens
  exposed as Lua objects with signals. adopt (P2).
- **`rc.lua` with sane defaults shipped as a file the user copies and
  edits.** adopt (P2, 0.1.0).
- **Rules as a Lua table matched on properties.** adopt (P2, 0.1.3).

## Things to avoid

- **Drawing the bar inside the window manager.** Our bar is Quickshell, a
  separate program.
- **Lua API churn between major versions.** We will version the `tp` API.

## Where to look

- `lib/awful/` (rules, tags, layouts)
- `objects/` (C objects exposed to Lua)
