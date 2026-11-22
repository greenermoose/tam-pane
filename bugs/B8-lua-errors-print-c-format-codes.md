# B8: Lua config errors print C format codes instead of numbers

- **Status:** Verified
- **Severity:** low
- **Affected versions:** 0.1.0 to 0.1.15
- **Fixed in:** 0.1.16
- **Reported:** 2026-11-21, by Claude Code (claude-opus-5-5)
- **Found by:** the test written for R1 finding 14

## What happens

A config value out of range gives an error with the numbers missing or
in the wrong places. `tp.config{ gaps = 999 }` reports

    tp.config.gaps: %lld out of range [999, 0]

and `tp.dsp.tag('view', 10)` reports `tag out of range [1, %u]`. Rules,
too-long strings and float settings are affected the same way.

## What should happen

`tp.config.gaps: 999 out of range [0, 256]`.

## Steps to reproduce

Load a config with `tp.config{ gaps = 999 }` and read the error in the
log. Automated: the integer-check cases near the top of
`tests/test_lua.c`; on 0.1.15, 13 of their checks fail.

## Cause

`luaL_error` formats with `lua_pushfstring`, which knows only `%d`,
`%I` (lua_Integer), `%f`, `%s`, `%c`, `%p`, `%U` and `%%`, and no
widths or precisions. The host used `%lld`, `%u`, `%zu` and `%.2f` in
seven messages in `src/lua.c`, `src/lua_rules.c` and
`src/lua_bindings.c`. Nothing checked the text of these errors.

## Fix and verification

- Integers use `%I` with a `lua_Integer`; sizes use `%d` with an `int`
  cast; the float range is formatted with `snprintf` first and passed
  as `%s`.
- The three integer validators are now one, `tp_lua_check_int`
  (`src/lua.c`, R1 finding 14), so the messages are the same
  everywhere: `<ctx>: expected integer` and
  `<ctx>: <n> out of range [lo, hi]`. `tp.dsp.tag`'s wording changed to
  match ("tag must be an integer" became "expected integer").
- Regression test: the cases above check each message and that no `%`
  is left in any of them.
- Fred's check: `sandbox/demo-0.1.16` on 2026-11-21 (commit 845b2c2,
  binary SHA-256
  `5ae4b804de0fa5d431ab296967ec413945411cf3ef0cb52cd7dd357873a8c288`);
  the launcher's check of the logged reload error passed.
