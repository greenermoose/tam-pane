# IPC protocol (`0.2.3`, updated `0.2.5`)

`tam-pane` serves a line-based IPC protocol over a same-UID Unix domain
socket, formatted with `libtam`'s `tam_str_*` helpers (no JSON library).
`tam-pane msg` is the built-in CLI client.

## Socket location and security

- **Path:** `$XDG_RUNTIME_DIR/tam-pane/<socket>.sock`, where `<socket>` is
  the compositor's `WAYLAND_DISPLAY` name (for example `wayland-0`).
- **Environment:** `tp_server_start` exports `TAM_PANE_SOCKET` to the full
  socket path alongside `WAYLAND_DISPLAY`, before running `startup` hooks.
- **Permissions:** `$XDG_RUNTIME_DIR/tam-pane` is created with mode `0700`
  and verified to be a directory owned by `getuid()`; the socket is bound
  with mode `0600`.
- **Peer authentication:** On every `accept4(..., SOCK_NONBLOCK |
  SOCK_CLOEXEC)`, the server checks `SO_PEERCRED` and rejects any peer
  whose `uid != getuid()`.
- **Cleanup:** `tp_ipc_finish` disconnects all clients and unlinks the
  socket on clean exit.
- **Non-blocking:** All server sockets are non-blocking. Per-client
  outbound buffers are capped at `65536` bytes (`TP_IPC_MAX_OUTBUF`); a
  stalled subscriber that exceeds the cap is disconnected so the compositor
  event loop never blocks. Concurrent connections are capped at `64`.

## Wire framing

All messages are UTF-8 text terminated by `\n`. NUL bytes are rejected.
Request lines are capped at `4096` bytes (`TP_IPC_MAX_LINE`).

- **Request:** `<command> [args...]\n`
- **Reply (queries and actions):**
  - Success: `OK\n`, zero or more record lines, then `END\n`.
  - Error: `ERR <message>\n`.
- **Event stream (`subscribe`):**
  - Success: `OK\n`, followed by one `event\t<kind>\t...` line per
    compositor event until the client disconnects or the server stops.
  - Error: `ERR <message>\n`.

### Record format and escaping

Every data and event line is tab-separated:

```
<type>\t<key>=<value>\t<key>=<value>...
```

In string values (`app_id`, `title`, `monitor`, `name`, `cmd`, `key`,
error messages), `\`, `\t`, `\n`, and `\r` are escaped as `\\`, `\t`,
`\n`, and `\r`, and other ASCII control bytes (`< 0x20` or `0x7f`) are
replaced with a space. Every record is guaranteed to occupy one line.

## Commands

### Read-only queries

- `version`
  - `version\tversion=<ver>\tsocket=<WAYLAND_DISPLAY>`
- `monitors`
  - One line per connected output:
    `monitor\tname=<n>\tx=<x>\ty=<y>\tw=<w>\th=<h>\tusable_x=<x>\tusable_y=<y>\tusable_w=<w>\tusable_h=<h>\tscale=<f>\ttags=<mask>\tprev_tags=<mask>\tfocused=<0|1>\tlayout=<dwindle|master|scroller|monocle>`
- `windows`
  - One line per mapped toplevel:
    `window\tmonitor=<n>\ttags=<mask>\tfocused=<0|1>\tfloating=<0|1>\tfullscreen=<0|1>\tscratchpad=<0|1>\tx=<x>\ty=<y>\tw=<w>\th=<h>\tapp_id=<id>\ttitle=<t>`
- `focused`
  - Same `window\t...` record for `tp_focused(s)`, or zero lines if no
    window is focused.
- `tags`
  - One line per monitor:
    `tag\tmonitor=<n>\tview=<mask>\tprev_view=<mask>\toccupied=<mask>\tfocused=<0|1>`
- `cursor`
  - `cursor\tx=<x>\ty=<y>\tmonitor=<name>`
- `binds`
  - One line per active key binding:
    `bind\tmods=<Meta+Shift|none>\tkey=<keysym>\tlocked=<0|1>\taction=<desc|lua>`

### Actions

- `reload`
  - Reloads `~/.config/tam-pane/init.lua` (or the `-c` config path)
    synchronously via `tp_server_reload()`. Returns `OK\nEND\n` on success
    or `ERR <diagnostic>\n` on failure (keeping the last good config).
- `dispatch <action> [args...]`
  - Runs a compositor dispatcher:
    - `terminal`, `close`, `quit`, `reload`, `toggle_float`,
      `toggle_fullscreen`
    - `focus next|prev`
    - `swap left|right|up|down`
    - `move left|right|up|down`
    - `resize left|right|up|down [step]` (`step` in `1..4096`)
    - `tag view|toggle_view|move|toggle_client <1..9>`
    - `tag back`
    - `monitor focus|move next|prev|<output-name>`
    - `layout set dwindle|master|scroller|monocle`
    - `layout next|prev`
    - `layout nmaster inc|dec`
    - `scratchpad toggle|move`
    - `spawn <command...>`

### Event subscription

- `subscribe [event ...]`
  - Subscribes the connection to compositor events. Empty arguments or
    `all` subscribe to all subscribable events:
    - `window.open`
    - `window.close`
    - `window.focus`
    - `tag.change`
    - `monitor.add`
    - `monitor.remove`
  - Events are delivered even when Lua has no hooks registered for them.

## Session-lock policy

While `s->locked` is true (`ext-session-lock-v1` / `tam-hide`):

- Read-only queries (`version`, `monitors`, `windows`, `focused`, `tags`,
  `cursor`, `binds`), `subscribe`, and `reload` remain allowed.
- `dispatch` is refused with `ERR session is locked`.

## `tam-pane msg` client

```sh
tam-pane msg [-S socket] <command> [args...]
```

Socket resolution order:

1. `-S <socket>` option
2. `$TAM_PANE_SOCKET`
3. `$XDG_RUNTIME_DIR/tam-pane/$WAYLAND_DISPLAY.sock`

Exit status is `0` on `OK`, `1` on `ERR` or connection failure, and `2` on
CLI usage errors. For `subscribe`, each event line is flushed to `stdout`
as it arrives.
