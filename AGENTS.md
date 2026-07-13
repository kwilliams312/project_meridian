<!-- SPDX-License-Identifier: Apache-2.0 -->
# Project Meridian — Subagent Guide

Rules for a subagent implementing a delegated task. Also read **`CLAUDE.md`**
(branch workflow — dev-first — and the orchestration loop) and **`CONTRIBUTING.md`**
(binding clean-room + asset-provenance policies) before writing server code or
adding assets.

You run on **Opus 4.8**. Work as autonomously as the story allows; **do not
assume — when a requirement is ambiguous, ask the orchestration agent, and when in
doubt consult the docs** (`docs/`, the SADs/PRDs, the specs under
`docs/superpowers/`).

## Workspace isolation

- **Work in your own fresh git worktree branched from a freshly-fetched `dev`** —
  never in the lead's working tree. This keeps concurrent sub-agents from
  corrupting each other. Fetch `dev` first, then branch from it.
- **Name your branch with the story's issue number**, e.g.
  `story-142-character-loadout`.

## Task & PR rules

- **Every task must have a story.** You are dispatched against a GitHub issue (the
  "story"). If you were handed work with no story, stop and ask the orchestration
  agent for one — do not implement untracked work.
- **Your PR must link to its story.** Open the PR with `Closes #<story>` (and
  `part of #<epic>` when the story belongs to an epic) so the work is traceable
  from the tracker back to the diff. One story ↦ one PR unless told otherwise.
- **Write a detailed PR** — a clear list of the changes you made, plus the fresh
  build + test evidence (see below). This is what QA and the lead review against.
- **PR into `dev`, never `main`/`ptr`** (`gh pr create --base dev`). Do **not**
  merge your own PR. After you open it, a **QA sub-agent** independently rebuilds,
  runs the full suite, runtime-verifies, and code-reviews your diff; then the
  **orchestration agent** reviews and merges. Expect to be re-dispatched to fix any
  findings.
- **Verify before you open the PR** and paste the evidence (build + test output)
  into the PR body — claims without fresh command output are not accepted.
- **Flag UI / client changes explicitly** in the PR body. UI/client work needs a
  human-confirmed E2E test before it can merge, so the lead must know to ask. To
  make that E2E test runnable, set it up **inside your own worktree** so the human
  exercises exactly this PR's code, and record the recipe in the PR body:
  - Bring up a **local copy of the server** from the worktree with
    `scripts/dev/run-local.sh` (throwaway MariaDB + authd/worldd — this is the
    canonical local run loop; requires a prior `scripts/dev/build.sh`).
  - Give the **CLI commands to build and run the client** against it —
    `scripts/dev/build-client.sh` (rebuild the GDExtension after any client C++
    change; GDScript-only changes need no rebuild) then `scripts/dev/run-client.sh`.
  - **Share the worktree path** (and these commands) in the hand-off so the E2E
    user can point the client at your local server and drive it themselves.
- Commits are **unsigned** (`commit.gpgsign=false`); use conventional-commit
  messages.
- **Do not post development updates to Discord.** Announcing delegation-loop
  progress is the orchestration lead's job (`scripts/dev/post-update.sh`);
  sub-agents never post.
