# Design principles

tam-pane is built fresh, from the ground up. We learn from MangoWM,
Hyprland and the rest, and take their best code, but we do not inherit
their structure. These rules decide how code is written here.

## Our own structure

- Modules, names and data flow are designed for tam-pane. Other
  projects' layouts are references, not templates.
- Code taken from another project is rewritten into our modules and our
  style, and recorded in `UPSTREAM.md` with the reason it earned its
  place.

## Take what works; leave what doesn't

- **Taken so far:**
  - MangoWM's dwindle tree and tag model;
  - Hyprland's dwindle split rules;
  - Hyprland's Lua design: dispatchers as values, and a watchdog on every
    call.
- **Left out on purpose:**
  - MangoWM's 5,000-line client file and 3,000-line options table;
  - its global `server` and `config` state;
  - scenefx woven through core code;
  - Hyprland's plugins locked to an exact build;
  - blocking calls reachable from Lua.

## Small modules, explicit state

- One job per file. No source file over about 800 lines.
- No global variables. Every function is handed the `tp_server` (or the
  object) it works on.
- Each module owns the listeners it adds and removes them in its own
  `finish` function.
- Pure logic, such as tags and layout trees, has no wlroots in it and is
  unit tested.

## The event loop never blocks

- Child processes are double-forked and never waited on (`tp_spawn`).
- Lua (from P2) runs under an instruction budget and a deadline, and it
  gets no blocking calls: no `io.popen`, no `os.execute`. The deadline
  also covers pattern matching, and Lua memory is capped (0.1.8).
- Events that Lua hooks cause are drained on a timer, never by
  re-adding an idle source, so epoll always runs between batches (0.1.6).
- Files are read with a size cap.

## Every dependency earns its place

- `arch/dependencies.md` lists each library, why it is needed, and the
  version that removes it.
- The goal is libc, the kernel's headers, libtam and code in this
  repository, with Mesa allowed for the GPU.
- Small jobs never pull in a library. We format IPC without a JSON
  library and match rules with Lua patterns, not pcre2.
- Plain Make only.

## Energy is a feature

- An idle desktop draws no frames.
- Clients get a configure only when their size really changes.
- Effects and animations can be turned off entirely, and the plain path
  is the default.

## Effects and scripting stay behind narrow interfaces

- Visual effects (P5) live in one module. `tp_client` never grows effect
  flags.
- Lua (P2) calls the same actions the compiled-in bindings call today. It
  never reaches into structures directly.
