# dwm

*Starter notes from the project's known design. Check them against the
source before acting on them.*

## What it is

- **Language:** C, about 2,000 lines. **License:** MIT/X.
- **Base:** Xlib (X11).
- **Scope:** the original tag-based dynamic tiling window manager from
  suckless.

## Ideas worth taking

- **Tags as bitmasks.** adopt (P1, 0.0.1).
  - A window can be on several tags and a view can show several tags.
    This is the root of tam-pane's tag model.
- **The previous tag set.** adopt (P1, 0.0.1). Two slots, swapped by
  `view(0)`; this is our `tp_tagview_back`.
- **Master area count and factor per monitor.** adopt (P3, master-stack
  layout).

## Things to avoid

- **Configuration only by recompiling.** We use Lua instead.

## Where to look

- `dwm.c` (`view`, `toggleview`, `tag`, `toggletag`, `tile`)
