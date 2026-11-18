# AI collaboration and provenance

This repository follows the [Tamlinux AI provenance standard](https://github.com/greenermoose/tamlinux/blob/main/docs/ai-provenance-standard.md).
Work is handed from session to session in `work/`, as
`work/README-work` describes:

- `work/WHATNOW` says what was just done and what comes next, in detail.
  A new session reads it first.
- Each work session has one plain-text record, `work/W<number>-<name>`.
  It names the AI harness and model with the version checked at the
  time, quotes the prompts that steered the work, lists its commits, and
  says what failed.
- Commits made with AI help carry `Co-authored-by:`, `AI-Tool:` and
  `AI-Model:` trailers.
- Transcripts are kept privately by the author.
