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

## Policies

`CONTRIBUTING.md` holds the **binding** clean-room and asset-provenance policies —
read them before writing server code or adding assets.
