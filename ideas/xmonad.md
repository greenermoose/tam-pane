# xmonad

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** Haskell. **License:** BSD 3-Clause.
- **Base:** X11.
- **Scope:** a tiling window manager configured, and extended, in Haskell
  itself.

## Ideas worth taking

- **Layouts as composable values** (combinators such as `Tall ||| Full`,
  and modifiers such as `smartBorders`). consider (P2, Lua layouts).
- **Pure layout functions,** given a rectangle and windows, returning
  rectangles. adopt. Our dwindle tree already works this way.

## Things to avoid

- **Configuration that needs a compiler toolchain.**

## Where to look

- `XMonad/Layout.hs`
- xmonad-contrib's `Layout/` modules
