# AI collaboration and provenance

This repository follows the [Tamlinux AI provenance standard](https://github.com/greenermoose/tamlinux/blob/main/docs/ai-provenance-standard.md),
with one change under trial since 2026-11-17: each work session has one
plain-text record in `work/`, as `work/README-work` describes, instead of
a record in `docs/ai/`.

- Each record names the AI harness and model with the version checked at
  the time, quotes the prompts that steered the work, and lists its
  commits.
- Commits made with AI help carry `Co-authored-by:`, `AI-Tool:` and
  `AI-Model:` trailers.
- Transcripts are kept privately by the author.
