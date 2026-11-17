# herbstluftwm

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C++. **License:** BSD 2-Clause.
- **Base:** X11.
- **Scope:** manual frame tiling, with tags shared between monitors (any
  monitor can show any tag), driven by `herbstclient`.

## Ideas worth taking

- **Tags not bound to one monitor.** consider (P1, 0.0.5).
  - A tag can move to whichever monitor you are looking at.
- **An attribute tree over IPC** (`attr`, `watch`) for scripts. consider
  (P3, 0.2.3).

## Things to avoid

- **Manual frames as the only model.**

## Where to look

- `src/monitormanager.cpp`
- `src/tag.cpp`
