# W2: Lua 5.5, and work records replace docs/ai

Date: 2026-11-17. AI harness: Claude Code 2.1.326; model: Claude Opus 5.5 (`claude-opus-5-5`).

## Guidance

Fred: "Use lua 5.5 for tam-pane. Update that repo. Make a note of this decision."

Fred, after the agent read `work/` and asked how it relates to `docs/ai/`: "Yes, write W2 and add work/ to the README. work/ supersedes docs/ai/. I want to try out this new procedure in tam-pane. If it is better than docs/ai/, then I want to roll it out across Tamlinux. But let's see how it works for developing tam-pane first."

## Results

- Commit `e8a8f38` changed the Lua plan from 5.4 to 5.5 in `README`, `ROADMAP.md` (0.1.0), `plans/P2-lua.md`, `arch/dependencies.md` and `ideas/hyprland.md`. No Lua code exists yet; P2 has not started. The libinput line in `arch/dependencies.md` still says liblua 5.4, because that describes Arch's libinput, not tam-pane.
- Added a P2 risk: while wlroots remains, Arch's libinput loads liblua 5.4 into the compositor process. tam-pane's static Lua 5.5 must not export its `lua_*` symbols. The Makefile does not use `-rdynamic` today.
- `make test` passed after the change, including both headless smoke checks and the six input setup checks.
- Fault: the `e8a8f38` trailer reads `AI-Tool: claude-code` without the version. It is pushed, so it stays; this record gives the version.
- `work/` now replaces `docs/ai/` for tam-pane as a trial. `README` lists `work/`; `README-work`, `AI_PROVENANCE.md` and `docs/ai/README` say that new session records go here and that the two earlier `docs/ai/` records stay as history. Commit trailers continue as before.

## Handoff

- P2 0.1.0: link without `-rdynamic`, and check that `nm -D build/tam-pane` shows no `lua_*` symbols.
- After a few more tam-pane sessions, Fred compares `work/` records with `docs/ai/` and decides whether to roll the procedure out across Tamlinux. Until then, other repositories keep `docs/ai/`.
