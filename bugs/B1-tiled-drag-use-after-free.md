# B1: tiled resize drag uses freed memory when another window leaves

- **Status:** Verified
- **Severity:** critical
- **Affected versions:** 0.0.4 to 0.1.4
- **Fixed in:** 0.1.5
- **Reported:** 2026-11-21, by Claude Code (claude-opus-5-5)
- **Found by:** R1

## What happens

During a Meta+Right-drag on a tiled window, another window in the same
tree closes, floats or changes tags. The split the drag was holding is
freed, and the next pointer motion reads and writes the freed node. The
result can be silent layout corruption or a crash of the session.

## What should happen

The drag carries on against the tree that is left, or stops cleanly.

## Steps to reproduce

1. Tile two windows, then open a third so it splits one of them.
2. Meta+Right-drag the window it split.
3. Close the third window, for example with `foot -e sleep 3`, and keep
   dragging.

Automated: the "tiled drag survives another window leaving the tree"
case in `tests/test_input_headless.c`. Built with
`-fsanitize=address,undefined`, it reports `heap-use-after-free` in
`tp_dwindle_drag_update` without the fix.

## Cause

`struct tp_dwindle_drag` keeps raw `tp_dwindle_node` pointers.
`tp_pointer_cancel_grab` cancels only when the grabbed window itself
goes away, but `tp_dwindle_remove` frees the parent split of any window
that leaves.

## Fix and verification

The tree's generation counter (`tp_dwindle.gen`) changes whenever it frees a
node. A drag records the tree and generation it began on.
`tp_dwindle_drag_valid` checks both, `tp_dwindle_drag_update` refuses to
write a stale drag, and `update_grab` begins the drag again from the
pointer, or cancels it when no split is left. Regression tests: the
headless case above and a unit case in `tests/test_dwindle.c`. `make
test` passes, and the ASan build of both tests is clean. Fred checked
0.1.5 nested with tools/demo-0.1.5 on 2026-11-21 (commit 3b808fd, binary
SHA-256 c22466f328a30ba9bcbd62dd4f373c77cbb334550c5b71f63187852ae08e7b99): drags held while windows opened and closed kept resizing,
and tam-pane did not crash.
