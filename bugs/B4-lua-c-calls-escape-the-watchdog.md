# B4: Lua work inside C library calls escapes the watchdog

- **Status:** Verified
- **Severity:** critical
- **Affected versions:** 0.1.0 to 0.1.7
- **Fixed in:** 0.1.8
- **Reported:** 2026-11-21, by Claude Code (claude-opus-5-5)
- **Found by:** R1 (finding 8)

## What happens

The watchdog's count hook fires only between Lua VM instructions, so
work done inside one C library call is invisible to it:

- `string.rep("x", 2^31)`, or a string doubled in a loop, allocates
  gigabytes. The allocator had no limit, so memory grows until the
  system's OOM killer takes the session.
- Lua's pattern matcher can backtrack for hours on a pattern such as
  `.-.-.-.-.-.-b`, with the compositor frozen. That pattern can be in a
  window rule (matched against a client-chosen title) or in any hook
  that calls `string.find`, `match`, `gmatch` or `gsub`.

## What should happen

Both are stopped like any runaway Lua: the call fails with an error, and
the compositor keeps running.

## Steps to reproduce

1. Bind a key to `function() string.find(string.rep('a', 255),
   '.-.-.-.-.-.-b') end` and press it: tam-pane stops responding.
2. Bind a key to `function() local s = 'x' while true do s = s .. s end
   end` and press it: memory climbs into gigabytes.

Automated: `test_memory_and_pattern_limits` in `tests/test_lua.c`. Run
against 0.1.7 code with the memory cases left out, its alarm fires with
"hung in Lua pattern matching". The memory cases were not run against
0.1.7, because they would really allocate gigabytes.

## Cause

`luaL_newstate` installs an unbounded allocator, and `lstrlib.c`'s
matcher runs in C with no way for the embedder to interrupt it.

## Fix and verification

- **Pattern matching:** one marked patch to `third_party/lua/lstrlib.c`
  (recorded in `UPSTREAM.md`) calls `LUAI_PATTERNCHECK` every 4096
  matcher steps. tam-pane defines it as `tp_lua_pattern_check`, which
  applies the watchdog's wall-clock deadline: 100 ms in callbacks and
  1.5 s for config. Long legitimate matching within the deadline still
  runs; the test does a 100,000-letter `gsub`.
- **Memory:** `tp_lua_alloc` counts the memory a Lua state holds. Inside
  guarded calls it refuses growth past `TP_LUA_MEMORY_MAX` (64 MiB),
  which Lua turns into a catchable "not enough memory" error. It refuses
  nothing outside guarded calls, so the host's own unprotected pushes can
  never make Lua panic.
- **Rules:** they now match only the first 255 bytes of `app_id` and
  `title`, the same as event hooks.

Tests: the case above. `make test` passes, and test_lua is clean under
ASan and LeakSanitizer. Fred checked 0.1.8 nested with
`sandbox/demo-0.1.8` on 2026-11-21 (commit 9b5f27b, binary SHA-256
`a1704c534ad3f0c8cab4085648deb7f79191bd98e942b3b1f6498ab11feda819`). The
runaway pattern was cut off, the memory bomb hit the cap, the long gsub
ran, and the long-titled window stayed on its tag.
