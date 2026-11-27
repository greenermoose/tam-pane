# IPC protocol (`0.2.3`, updated `0.2.6`)

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
  - `version\tversion=<ver>\tsocket=<WAYLAND_DISPLAY>\tdisplay=<DISPLAY>` (`display` is empty when XWayland is disabled or unavailable)
- `monitors`
  - One line per connected output:
    `monitor\tname=<n>\tx=<x>\ty=<y>\tw=<w>\th=<h>\tphys_w=<w>\tphys_h=<h>\trefresh=<mHz>\ttransform=<0..7>\tusable_x=<x>\tusable_y=<y>\tusable_w=<w>\tusable_h=<h>\tscale=<f>\ttags=<mask>\tprev_tags=<mask>\tfocused=<0|1>\tlayout=<dwindle|master|scroller|monocle>\tdpms=<0|1>\tdescription=<d>\tenabled=<0|1>`
- `windows`
  - One line per mapped toplevel:
    `window\tmonitor=<n>\tid=0x<id>\tpid=<pid>\ttags=<mask>\tfocused=<0|1>\tfloating=<0|1>\tfullscreen=<0|1>\tpinned=<0|1>\tscratchpad=<0|1>\txwayland=<0|1>\tx=<x>\ty=<y>\tw=<w>\th=<h>\tapp_id=<id>\ttitle=<t>`
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
- `chord`
  - Current one-handed chord composer state:
    `chord\tmode=<idle|pluripotent|building|caps_lock>\tchord=<str>\tfilter=<str>\tmatches=<N>\tselected=<idx>`
- `lock_state` (0.2.16)
  - Current session-lock state:
    `lock\tlocked=<0|1>\tactive=<0|1>\tlocker_running=<0|1>\tsurfaces=<N>\toutputs=<M>`
- `gamma` (0.2.16)
  - Current compositor color temperature:
    `gamma\ttemperature=<K>\tenabled=<0|1>` (`6500` and `enabled=0` at identity)
- `freeze` (0.2.16)
  - Whether native screen freeze is active:
    `freeze\tactive=<0|1>`
- `pixel [x y]` (0.2.16)
  - Samples the rendered RGB pixel at layout coordinates `(x, y)` (defaulting to
    the current pointer coordinates). Refused with `ERR session is locked` while
    locked:
    `pixel\tx=<x>\ty=<y>\tr=<0..255>\tg=<0..255>\tb=<0..255>\thex=#rrggbb`
- `devices` (0.2.17)
  - Lists all connected input devices (allowed while locked):
    `device\ttype=<keyboard|pointer|touch|tablet|tablet_pad|switch>\tname=<name>`

### Actions

- `reload`
  - Reloads `~/.config/tam-pane/init.lua` (or the `-c` config path)
    synchronously via `tp_server_reload()`. Returns `OK\nEND\n` on success
    or `ERR <diagnostic>\n` on failure (keeping the last good config).
- `dispatch <action> [args...]`
  - Runs a compositor dispatcher:
    - `terminal`, `quit`, `reload`, `toggle_float`, `toggle_fullscreen`
    - `close [0xID]` (0.2.17: optional window ID; defaults to focused window)
    - `float [on|off|toggle] [0xID]` (0.2.17)
    - `pin [on|off|toggle] [0xID]` (0.2.17: pins a floating window across all tags on its monitor)
    - `resize_exact <w> <h> [0xID]` (0.2.17: exact floating dimensions in pixels, `1..16384`)
    - `move_exact <x> <y> [0xID]` (0.2.17: floating position relative to monitor `usable` area)
    - `center [0xID]` (0.2.17: center floating window on monitor `usable` area)
    - `raise [0xID]` (0.2.17: raise window to top of its scene layer)
    - `focus next|prev|left|right|up|down`
    - `swap left|right|up|down`
    - `move left|right|up|down`
    - `resize left|right|up|down [step]` (`step` in `1..4096`)
    - `tag view|toggle_view|move|toggle_client <1..9>`
    - `tag back`
    - `monitor focus|move next|prev|<output-name>`
    - `monitor enable|disable <output|focused>` (0.2.17): logically enables or
      disables a connected output (clamshell / laptop panel control), displacing
      windows to an enabled heir on disable and reclaiming them on enable.
    - `monitor mode <output|focused> <W>x<H>[@<Hz>]` (0.2.14): sets one
      of the output's own modes (refresh within 10 mHz; without `@<Hz>`,
      the highest). Replies `OK` only when the commit went through, which
      on a real display is a full modeset; an output that is off, or has
      no such mode, gets `ERR`. Every modeset, whoever asks for it, logs
      `output <name>: modeset <W>x<H>@<Hz>Hz` (refresh to two decimals),
      which display recovery scripts read as proof of a retrain.
    - `layout set dwindle|master|scroller|monocle`
    - `layout next|prev`
    - `layout nmaster inc|dec`
    - `scratchpad toggle|move`
    - `chord_toggle`, `chord_cancel`
    - `gamma <1000..25000|reset|off>` (0.2.16): sets the compositor's native
      color temperature LUT (`6500`, `reset`, or `off` restores identity).
    - `freeze on|off` (0.2.16): snapshots all enabled outputs onto a freeze
      layer below `TP_LAYER_OVERLAY` (so `slurp` renders on top and `grim`
      captures the frozen frame), or clears the freeze. Automatically cleared on
      session lock, output removal, or GPU renderer loss.
    - `spawn <command...>`

### Event subscription

- `subscribe [event ...]`
  - Subscribes the connection to compositor events. Empty arguments or
    `all` subscribe to all subscribable events:
    - `window.open`
    - `window.change` (tag assignment or output membership updated)
    - `window.close`
    - `window.focus`
    - `tag.change`
    - `monitor.add`
    - `monitor.remove`
    - `session.lock`, `session.unlock` (0.2.16; `locked=<0|1>`)
    - `gamma.change` (0.2.16; `temperature=<K>\tenabled=<0|1>`)
    - `chord.start`, `chord.update`, `chord.end`, `chord.cancel` (or shorthand `chord`)
  - Events are delivered even when Lua has no hooks registered for them.

## Session-lock policy

While `s->locked` is true (`ext-session-lock-v1` / `tam-hide`):

- Read-only queries (`version`, `monitors`, `windows`, `focused`, `tags`,
  `cursor`, `binds`, `keyboards`, `devices`, `chord`, `lock_state`, `gamma`,
  `freeze`), `subscribe`, and `reload` remain allowed.
- `pixel` is refused with `ERR session is locked`.
- `dispatch dpms on|off <output>` remains allowed while locked (matching
  `zwlr_output_power_manager_v1`); all other `dispatch` actions are refused with
  `ERR session is locked`.

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


`dispatch send_key_if_focused 0xID CHORD` sends the chord only when the
focused window still has the supplied ID. It returns an error for a
different target or when the requested chord cannot be sent.
