# MangoWM

## What it is

- **Language and size:** C, about 40,000 lines.
- **License:** GPL v3. It derives from dwl, which in turn derives from dwm
  and tinywl.
- **Base:** wlroots 0.20, with scenefx 0.5 for effects. It also depends on
  libinput, xkbcommon, pixman, pango and cairo (for title bars), cJSON
  (IPC), pcre2 (window rules) and xcb (XWayland). It builds with meson.
- **Scope:** dwl-style tags, many layouts (scroller, vertical scroller,
  master-stack, monocle, dwindle, overview), animations and effects.
- **Notes from source:** clone `~/Code/mango`, read at commit `5ba74a95`.

## Ideas worth taking

- **Tags with one layout tree per tag.** adopt (P1, 0.0.1).
  - `struct Pertag` keeps a dwindle root, layout and factors for each tag,
    plus a slot for multi-tag views.
  - Source: `include/mango/manage/monitor.h`.
- **The dwindle tree.** adopt (P1, 0.0.1).
  - Small, correct binary-tree insert, remove and assign.
  - Source: `src/layout/dwindle.c`; see `UPSTREAM.md`.
- **Proportional resize on insert and remove.** consider (P1, 0.0.4).
  - With `dwindle_manual_split`, adding a window takes 1/N of a
    same-direction run, and removing one hands its share back
    proportionally.
  - Source: `dwindle_remove` and `dwindle_insert_with_config`.
- **Scroller layout.** adopt (P3, 0.2.4).
  - Niri-style columns that scroll horizontally, with per-window
    proportions.
  - Source: `src/layout/scroll.c`.
- **Overview with jump labels.** consider (after P3).
  - Shows every window at once, with easymotion-style keys to jump.
  - Source: `src/overview/`.
- **Session target.** adopt (P4, 0.3.0).
  - Ships `mango-session.target` and activates it, so portals and user
    services start with the compositor.
  - Source: `assets/mango-session.target`, `set_activation_env()` in
    `src/main.c`.
- **Hot reload of the configuration.** adopt (P2, 0.1.4).
  - Watches the configuration file and reapplies it.
  - Source: `src/config/watcher.c`, `src/config/reapply.c`.
- **Swallow.** consider.
  - A terminal hides while the GUI program it started is open.

## Things to avoid

- **One huge client file.** `src/manage/client.c` is 5,027 lines and
  `struct Client` has well over a hundred fields.
- **A giant options table.** `src/config/option_defs.c` is 3,019 lines,
  and `src/dispatch/bind.c` is 2,802.
- **Global state.**
  - `extern struct MangoServer server` and `extern Config config` are
    used about 4,000 times in `src/`.
  - Even the dwindle tree reads `config.` and `server.` directly.
- **Effects woven into everything.** scenefx types appear in 26 files,
  including core window management.
- **Dependencies for small jobs.** cJSON only to format IPC replies,
  pcre2 only to match rules, and pango and cairo only for title text.

## Where to look

- `src/layout/` (all layouts and `arrange.c`)
- `src/manage/monitor.c` (per-tag state)
- `src/ipc/ipc.c` (the IPC protocol)
- `docs/` (user documentation, a good model for ours)
