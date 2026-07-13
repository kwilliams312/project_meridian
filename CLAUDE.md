# Project Meridian — Agent Guide

## ⛔ Branch workflow — commit to `dev`, never `main`

This repo uses a three-branch promotion model that maps 1:1 to the hosted realms
(see the CD design in
[docs/superpowers/specs/2026-07-07-cd-hosted-realms-design.md](docs/superpowers/specs/2026-07-07-cd-hosted-realms-design.md)):

| Branch | Realm | Who writes to it |
|--------|-------|------------------|
| `dev`  | Dev (`meridian-dev`)   | **you** — all day-to-day development lands here first |
| `ptr`  | PTR (`meridian-ptr`)   | promotion only (`dev → ptr`) |
| `main` | Prod (`meridian-prod`) | promotion only (`ptr → main`) |

**Rules:**

- **`dev` is the integration branch.** Branch feature work from `dev` and open
  PRs back into `dev`. If you commit directly, commit to `dev`.
- **Never commit or open a PR directly against `main` or `ptr`.** Those branches
  advance only through promotion (`dev → ptr → main`), which ArgoCD rolls out to
  the PTR then Prod realms.
- The default checkout is `main`. **Switch before you start work:** `git switch dev`
  (create it once from `main` if it does not exist: `git switch -c dev`).
- Rollback at any tier is `git revert` on that branch.
- Follow the repo git rule: do not run destructive or history-rewriting git
  commands, and get explicit human approval before any push.

## Local development

Bring the stack up locally with `scripts/dev/run-local.sh` (throwaway MariaDB in
`.dev-run/`). This local build/run loop is the canonical local workflow and is
**unchanged** by the hosted-realm CD work.

## Orchestration & story tracking

**Whatever agent is driving this session is the technical lead (the orchestration
agent).** The lead never implements tasks itself — it decomposes work, delegates
each piece to a sub-agent, gates the result through QA, then reviews and merges.
Run the loop as autonomously as possible; **when a decision is genuinely
ambiguous, ask the human rather than assume, and when in doubt consult the docs**
(`docs/`, the SADs/PRDs, and the specs under `docs/superpowers/`).

### The delegation loop

1. **Decompose into stories.** Break the work into GitHub issues ("stories").
   **No task without a story, and every task is delegated — the lead implements
   nothing directly.** Group stories under an epic and keep its checklist current.
2. **Dispatch a sub-agent per story.** Every sub-agent runs **Opus 4.8**. Hand it
   exactly one story and point it at `AGENTS.md`. Sub-agents work in **isolation**
   (see below) so parallel tasks never corrupt each other's tree.
3. **Sub-agent implements and opens a PR into `dev`.** The PR links its story,
   lists the changes, and pastes fresh build + test evidence. Sub-agents **never
   self-merge**.
4. **QA gate (before the lead reviews the PR).** The lead dispatches a separate
   **QA sub-agent (Opus 4.8)** against the PR. QA independently, from a clean
   checkout: **rebuilds, runs the full test suite, runtime-verifies by actually
   executing the affected program/flow, and does an independent code review** of
   the diff (correctness + clean-room/provenance compliance per `CONTRIBUTING.md`).
   QA does **not** run UI E2E — that is the separate human-gated step below.
5. **UI / client changes → set up E2E and hand it to the human.** If the PR
   touches UI or client code, the lead **tells the human a UI E2E test is needed
   and waits for explicit confirmation** before proceeding past QA. Do not merge
   UI/client work without it. Before handing off, **stand the E2E environment up
   in that PR's own worktree** so the human tests exactly the PR's code:
   - Bring up a **local copy of the server** from the worktree
     (`scripts/dev/run-local.sh` — throwaway MariaDB + authd/worldd; see
     [Local development](#local-development)).
   - Give the human the **worktree path** plus the exact **CLI commands to build
     and run the client** against that local server —
     `scripts/dev/build-client.sh` (rebuild the GDExtension after any client C++
     change) then `scripts/dev/run-client.sh`.
   Share the worktree path and these commands in the hand-off so the E2E user can
   drive the running client themselves.
6. **Lead review & disposition.** Only after QA passes (and UI E2E is confirmed
   when applicable) does the lead review the PR itself — never trusting the
   sub-agent's or QA's claims blindly. Then either **approve and merge into `dev`**,
   or **spawn a fix sub-agent** (new story or the same one) to address findings and
   re-run the loop from step 3.
7. **Close the story on merge.** Because PRs merge into `dev` (not the default
   branch), `Closes #N` does not auto-fire — the lead closes the story manually on
   merge and ticks the parent epic's checklist.

### Sub-agent isolation

Each sub-agent works in its **own fresh git worktree branched from a
freshly-fetched `dev`** (never the lead's working tree), so concurrent sub-agents
can't clobber one another. The branch name **includes the story's issue number**
(e.g. `story-142-character-loadout`). Fetch `dev` at the start of the session so the
worktree starts from current `dev`.

See `AGENTS.md` for the rules sub-agents must follow.

## Policies

`CONTRIBUTING.md` holds the **binding** clean-room and asset-provenance policies —
read them before writing server code or adding assets.
