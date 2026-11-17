# P5: Visuals (0.4.x)

**Status:** not started.

## Goal

The visual polish Fred expects from Hyprland and MangoWM, written in our
own small renderer module on Mesa, without scenefx.

## Versions

- **0.4.0** Window borders (focused and unfocused colours) drawn as scene
  rectangles.
- **0.4.1** Rounded corners and shadows, using our own GLES shaders.
- **0.4.2** Blur behind transparent windows and panels, with damage
  tracking so idle screens cost nothing.
- **0.4.3** Animations for open, close, move and tag change, driven by
  the frame clock and off by default on battery.

## Acceptance

- An idle desktop draws no frames (check the frame counter over a minute).
- Every effect can be turned off from Lua, and with all of them off, the
  render path is the plain scene path.

## Dependencies

- Uses: Mesa GLES (already present through wlroots' renderer).
- Does not add scenefx. Mango wove scenefx through 26 of its files; ours
  stays inside one module.

## Risks

- **Energy.** Blur and animations cost GPU time. They are off when idle
  and can be off for good.

## Open points

- None.
