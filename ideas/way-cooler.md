# Way Cooler

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C and Rust. **License:** MIT.
- **Base:** wlroots (in its later versions).
- **Scope:** an attempt to bring awesome's Lua model to Wayland. It is
  discontinued.

## Ideas worth taking

- **The lesson of why it stalled:** it tried to reimplement awesome's
  entire Lua API at once. adopt the lesson (P2). Ship a small `tp` API and
  grow it with real needs.

## Things to avoid

- **Copying another project's API wholesale.**

## Where to look

- The project's archived README and blog posts on its end.
