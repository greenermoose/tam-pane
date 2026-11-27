# PR02 · Recent tam-desktop launch performance

- **Date:** 2026-11-27
- **Builds:** tam-pane 0.2.14 (version reported at startup, source revision `79b36fe`) and Hyprland 0.56.2-4.2 (installed package).
- **Tool:** `tam-session-bench` 0.1.0, using its journal session parser and comparison metrics.
- **Source:** User journal, boot `0da62c9687ac4e4a99d81a06672e82db`, 2026-11-27 14:14:48 through 14:16:14 UTC.

## Summary

Across two launches per compositor, tam-pane reached the desktop in a median of **1,567 ms** versus **2,591 ms** for Hyprland (39.5% less time). Compositor readiness took **463 ms** versus **1,194 ms** (61.2% less time). In the later consecutive pair, tam-pane reached the desktop **589 ms sooner** (27.4%).

## Startup

Time to desktop runs from the UWSM session envelope to the Tamlinux shell’s IPC-ready marker. Compositor ready runs from the systemd compositor unit starting to that unit reporting ready.

| Started (UTC) | Compositor | Env preload (ms) | Compositor ready (ms) | Session target (ms) | Shell ready (ms) | Total (ms) | At cutoff |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| 14:14:48 | Hyprland | 168.2 | 1,405.0 | 417.0 | 1,041.4 | 3,031.7 | Stopped |
| 14:15:27 | tam-pane | 169.8 | 480.4 | 346.7 | 575.6 | 1,572.6 | Stopped |
| 14:15:51 | Hyprland | 161.2 | 982.7 | 344.1 | 662.6 | 2,150.6 | Stopped |
| 14:16:11 | tam-pane | 166.8 | 444.9 | 376.8 | 573.0 | 1,561.5 | Active |

| Stage (ms) | Hyprland median [range], n=2 | tam-pane median [range], n=2 |
| --- | ---: | ---: |
| Env preload | 164.7 [161.2–168.2] | 168.3 [166.8–169.8] |
| Compositor ready | 1,193.85 [982.7–1,405.0] | 462.65 [444.9–480.4] |
| Session target | 380.55 [344.1–417.0] | 361.75 [346.7–376.8] |
| Shell ready | 852.0 [662.6–1,041.4] | 574.3 [573.0–575.6] |
| Time to desktop | 2,591.15 [2,150.6–3,031.7] | 1,567.05 [1,561.5–1,572.6] |

| tam-pane launch (UTC) | Lua (ms) | Init (ms) | Start (ms) | Internal total (ms) |
| --- | ---: | ---: | ---: | ---: |
| 14:15:27 | 2.2 | 67.8 | 149.4 | 219.4 |
| 14:16:11 | 1.1 | 54.4 | 141.2 | 196.8 |

## Resources

Systemd records peak memory and CPU at unit stop. CPU is averaged over the unit’s measured lifetime, with 100% corresponding to one logical CPU. Both units had zero peak swap in all three stopped sessions.

| Started (UTC) | Compositor | Compositor duration (s) | Peak memory (MiB) | Avg CPU | Shell duration (s) | Shell peak memory (MiB) | Shell avg CPU |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 14:14:48 | Hyprland | 32.507 | 388.57 | 6.97% | 30.136 | 2,347.82 | 61.71% |
| 14:15:27 | tam-pane | 15.477 | 81.05 | 6.14% | 14.522 | 1,091.62 | 102.42% |
| 14:15:51 | Hyprland | 14.079 | 143.91 | 14.11% | 12.258 | 1,159.25 | 129.31% |
| 14:16:11 | tam-pane | — | — | — | — | — | — |

| Metric | Hyprland median [range], n=2 | tam-pane single observation, n=1 |
| --- | ---: | ---: |
| Compositor peak memory | 266.24 [143.91–388.57] MiB | 81.05 MiB |
| Compositor avg CPU | 10.54% [6.97–14.11%] | 6.14% |
| Shell peak memory | 1,753.53 [1,159.25–2,347.82] MiB | 1,091.62 MiB |
| Shell avg CPU | 95.51% [61.71–129.31%] | 102.42% |

## Observations and limits

- Compositor readiness had the largest gap between stage medians at **731 ms**, followed by the shell-ready stage at **278 ms**. The gap in median time to desktop was **1,024 ms**.
- The prior 0.2.14 launch reached the desktop in **1,579.7 ms** at 03:26:28 UTC, 13 ms above this report’s tam-pane median. Startup banners for [PR01](PR01-hyprland-vs-tam-pane-launch.md) identify its 00:20:31 and 03:26:28 UTC sessions as 0.2.10 and 0.2.14 respectively, on boot `dd10d33d16604f518b6743620267ff72`.
- Hyprland launched first after boot. This sample has two launches per compositor in a fixed order, with no controlled cache state or workload. The stopped sessions lasted 14–33 seconds, so their CPU and memory figures describe short sessions that include startup. The active session had no stop-time resource figures at the cutoff. No idle or GPU measurements were collected.
