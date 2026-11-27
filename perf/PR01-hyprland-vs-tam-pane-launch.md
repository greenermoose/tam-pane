# PR01: Hyprland vs tam-pane session launch

- **Date:** 2026-11-26
- **Builds:** `tam-pane` 0.2.15 (`41fec70`, with earlier session on 0.2.14) vs `hyprland` 0.56.2-4.2
- **Tool:** `tam-session-bench` 0.1.0 (`compare` and `startup`)
- **Source:** user journal, boot `dd10d33d16604f518b6743620267ff72`, `2026-11-25T19:10:49Z` – `2026-11-27T03:26:28Z`

## Summary

`tam-pane` starts faster and uses substantially less compositor memory than Hyprland on the same UWSM/Quickshell session stack. Across this boot's sessions, `tam-pane` reached compositor-ready **51.5% faster** (-497 ms median), reached full desktop **25.9% faster** (-584 ms median), and peaked at **60.8% less compositor memory** (84.5 MiB vs 215.7 MiB median).

## Median comparison

Measured across 5 completed Hyprland sessions (1 failed Hyprland session excluded) and 2 `tam-pane` sessions (1 completed, 1 active at time of capture).

| Metric | Hyprland | `tam-pane` | Delta (`tam-pane` vs Hyprland) |
| --- | ---: | ---: | ---: |
| Env Preload | 167 ms [164–169] (n=5) | 170 ms [169–170] (n=2) | +3 ms (+1.6%) |
| Compositor Ready | 964 ms [956–1,389] (n=5) | 467 ms [453–482] (n=2) | -497 ms (-51.5%) |
| Session Target | 362 ms [352–434] (n=5) | 353 ms [340–366] (n=2) | -9 ms (-2.6%) |
| Shell Ready | 768 ms [755–1,020] (n=5) | 685 ms [593–777] (n=2) | -83 ms (-10.8%) |
| Total to Desktop | 2,258 ms [2,245–3,008] (n=5) | 1,674 ms [1,580–1,769] (n=2) | -584 ms (-25.9%) |
| Compositor Memory Peak | 215.7 MiB [144.2–390.7] (n=5) | 84.5 MiB (n=1) | -131.2 MiB (-60.8%) |
| Compositor Swap Peak | 0.0 MiB [0.0–105.3] (n=5) | 0.0 MiB (n=1) | — |
| Compositor Avg CPU | 2.39% [1.35–17.07] (n=5) | 1.59% (n=1) | -0.80 pp (-33.5%) |
| Shell Memory Peak | 1,381.8 MiB [1,144.8–2,973.7] (n=5) | 1,368.5 MiB (n=1) | -13.3 MiB (-1.0%) |
| Shell Avg CPU | 3.75% [3.15–161.0] (n=5) | 3.49% (n=1) | -0.26 pp (-6.9%) |

## Per-session breakdown

| Started (UTC) | Compositor | Env | WM Ready | Target | Shell | Total | WM Peak RAM | Duration |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2026-11-25T19:10:49Z | Hyprland | 165 ms | 1,389 ms | 434 ms | 1,020 ms | 3,008 ms | 390.7 MiB | 71.4 min |
| 2026-11-25T20:25:31Z | Hyprland | 167 ms | 964 ms | 358 ms | 763 ms | 2,252 ms | 144.2 MiB | 48.8 min |
| 2026-11-25T21:14:46Z | Hyprland | 169 ms | 956 ms | 362 ms | 771 ms | 2,259 ms | 295.6 MiB | 118.4 min |
| 2026-11-25T23:14:49Z | Hyprland | 164 ms | 964 ms | 362 ms | 755 ms | 2,245 ms | 215.7 MiB | 745.1 min |
| 2026-11-27T00:20:31Z | `tam-pane` | 170 ms | 482 ms | 340 ms | 777 ms | 1,769 ms | 84.5 MiB | 152.4 min |
| 2026-11-27T03:26:09Z | Hyprland | 167 ms | 971 ms | 352 ms | 768 ms | 2,258 ms | 144.8 MiB | 0.2 min |
| 2026-11-27T03:26:28Z | `tam-pane` | 169 ms | 453 ms | 366 ms | 593 ms | 1,580 ms | *(active)* | *(active)* |

## Observations

- **Back-to-back warm launch (`03:26:09Z` vs `03:26:28Z`):** `tam-pane` reached compositor-ready in **453 ms** vs Hyprland's **971 ms** (2.1× faster, -519 ms) and reached full desktop in **1,580 ms** vs **2,258 ms** (-679 ms, -30.0%).
- **Internal `tam-pane` startup phases:** On the `03:26:28Z` launch, `tam-pane`'s internal startup log reported **201.1 ms** total (`lua=1.1 ms`, `init=53.5 ms`, `start=146.5 ms`). The remaining ~251 ms of the `Compositor Ready` stage is UWSM and systemd service readiness handoff.
- **Shared launch overhead:** Env preload (~167–170 ms), `graphical-session.target` (~353–362 ms), and `tamlinux-shell.service` (~685–768 ms; ~1.37 GiB peak memory) are shared across both compositors. Almost all startup and memory savings come directly from the compositor unit (`wayland-wm@*.service`) and from `tam-pane` launching directly under UWSM without `start-hyprland`.
- **Memory footprint:** Even Hyprland's 12-second launch-and-exit session (`03:26:09Z`) peaked at **144.8 MiB**, whereas `tam-pane` peaked at **84.5 MiB** (0 swap) across a 2.5-hour session (`00:20:31Z`).

## Limits

- `tam-pane` peak memory and CPU averages currently have **n=1** completed session; systemd records `MEMORY_PEAK` in the journal only when a unit stops, so the active `03:26:28Z` session is not included in the memory peak aggregate.
- Live PSS, RSS, and VRAM snapshots (`tam-session-bench now` / `sample`) were not captured for this report; future reports can add timed idle and workload samples.
