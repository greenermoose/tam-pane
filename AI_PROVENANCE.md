# AI collaboration and provenance

This repository follows the [Tamlinux AI provenance standard](https://github.com/greenermoose/tamlinux/blob/main/docs/ai-provenance-standard.md).
Records describe AI-assisted changes from their introduction onward.
Transcripts remain private. Each session has one record in `work/`, named
`W<number>-<name>.md` as `work/README-work` describes. Since 2026-11-17,
`work/` replaces `docs/ai/` for this repository, as a trial; the two
earlier `docs/ai/` records stay as history.

## How to read this record

- **Toolchain** lists each AI tool and model that worked on this
  repository, with the version checked on the machine at the time.
- **Milestones** lists what each session delivered, linking its record.
- Commits made with AI help carry `AI-Tool:` and `AI-Model:` trailers.

## Toolchain

| Tool | Verified version | Model | Date |
| --- | --- | --- | --- |
| Claude Code | 2.1.326 | claude-opus-5-5 | 2026-11-17 |
| Codex CLI | 0.169.0 | gpt-6.1-terra | 2026-11-17 |

## Milestones

| Date | Milestone | Record |
| --- | --- | --- |
| 2026-11-17 | 0.0.1: repository, documentation folders, MangoWM and Hyprland evaluations, and a framework that compiles and passes headless tests | [Session](docs/ai/2026-11-17-tam-pane-scaffold.md) |
| 2026-11-17 | Headless synthetic input device setup checks | [Session](docs/ai/2026-11-17-headless-input.md) |
| 2026-11-17 | Lua 5.5 planned in place of 5.4; `work/` records replace `docs/ai/` | [W2](work/W2-lua-5-5-and-work-folder.md) |
