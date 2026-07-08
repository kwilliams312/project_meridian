<!-- SPDX-License-Identifier: Apache-2.0 -->
# Project Meridian — Subagent Guide

Rules for a subagent implementing a delegated task. Also read **`CLAUDE.md`**
(branch workflow — dev-first — and the orchestration loop) and **`CONTRIBUTING.md`**
(binding clean-room + asset-provenance policies) before writing server code or
adding assets.

## Task & PR rules

- **Every task must have a story.** You are dispatched against a GitHub issue (the
  "story"). If you were handed work with no story, stop and ask the orchestration
  agent for one — do not implement untracked work.
- **Your PR must link to its story.** Open the PR with `Closes #<story>` (and
  `part of #<epic>` when the story belongs to an epic) so the work is traceable
  from the tracker back to the diff. One story ↦ one PR unless told otherwise.
- **PR into `dev`, never `main`/`ptr`** (`gh pr create --base dev`). Do **not**
  merge your own PR — the orchestration agent independently verifies and merges.
- **Verify before you open the PR** and paste the evidence (build + test output)
  into the PR body — claims without fresh command output are not accepted.
- Commits are **unsigned** (`commit.gpgsign=false`); use conventional-commit
  messages.
