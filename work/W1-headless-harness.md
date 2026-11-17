# W1: 0.0.2 nested test and headless harness

Date: 2026-11-17. AI harness: Codex CLI 0.169.0; model: gpt-6.1-terra.

## Guidance

Fred asked to plan and implement tam-pane 0.0.2, then asked: “How much of this can you automate? Do you really need me to do all this or can you create a testing script for yourself?” He subsequently directed the agent to create the harness and commit the work.

## Results

- Fred ran commit `7e8185aa53cbd2a14a310672486ca91da2b7c68b` nested under Hyprland. A foot terminal opened, accepted typing and closed using its X. Two terminals opened and tiled. The tested binary had SHA-256 `99f773e4d96f684da145cd1bdb48b41b34fd077fa6a2e0818ef83f883de5dac9`.
- The first nested launch showed only a black window because no client was started. The temporary home launcher was changed to start foot; its undo script is `~/pane-undo.sh`.
- A forced rebuild completed without warnings and produced the same binary hash. `make test` passed: 28 tag checks, 52 dwindle checks, and both headless smoke checks. The sandboxed smoke test could not create its Wayland socket; it passed outside the sandbox.
- Commit `9f4ee0f` added a partial headless input test. It starts a private compositor, attaches synthetic keyboard and pointer devices, and checks repeat settings and seat capabilities. All six new checks passed with `make test` outside the sandbox.

## Handoff

The requested integration harness is unfinished. Build a real Wayland test client and inject keyboard and pointer events into the test compositor; verify window geometry and closing, focus, pointer and keyboard event delivery, and bindings. The current test only checks device setup; it does not test event delivery. Investigate the possible unmatched release after a consumed binding. Fix defects found and rerun the suite.

0.0.2 is not accepted or version-bumped. Fred has not confirmed focus cycling, repeat, pointer behavior across windows, or Meta shortcuts; Hyprland may intercept those shortcuts in a nested session. After automated checks, a brief final nested check and Fred's acceptance of the exact build remain necessary.
