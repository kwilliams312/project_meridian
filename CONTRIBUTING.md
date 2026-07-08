# Contributing to Project Meridian

Thanks for helping build an open-source MMORPG. Two things make this project legally viable —
the **clean-room policy** and the **provenance policy** — so please read this before your first PR.

## Licensing

| What | License |
|---|---|
| Code (server, client GDExtension/GDScript, tools, schemas) | [Apache-2.0](LICENSE) |
| Content data (`/content` YAML) | Apache-2.0 |
| Original art & music assets | CC-BY 4.0 |
| Third-party assets | CC0 or CC-BY **only** (no NC/ND/SA; no engine-locked marketplace content — see TD-09 in the [Baseline](docs/00-GAME-DESIGN-BASELINE.md)) |

By contributing you agree your contribution is licensed under the terms above (inbound = outbound).

## ⛔ Clean-room policy (binding)

Meridian's server is *architecturally inspired by* CMaNGOS and TrinityCore, which are **GPL-licensed**.
No GPL code may enter this Apache-2.0 codebase:

- **Never copy, port, or transcribe code** from CMaNGOS, TrinityCore, or any other GPL project —
  not a function, not a table layout, not a protocol constant.
- Contributors who have recently worked in those codebases must implement from the written specs
  in `/docs`, not from source recall.
- No Blizzard assets, names, file formats, or extracted data — and no recognizable lookalikes
  (gear sets, creature designs, zone layouts, UI trade dress). Playstyle inspiration only.
- Any contribution suspected of derivation is rejected in review. Architecture-heavy PRs should
  include a short provenance statement ("designed from the server SAD §x", "informed by public
  CMaNGOS documentation, no source consulted").

## Asset provenance (TD-09)

Every art/music/SFX asset ships with an IF-8 sidecar (`meridian/asset@1`,
[schema](schema/content/asset.schema.yaml)) recording its license and origin. No sidecar, no merge:

- `source_tier: original | ai | cc0 | cc_by` — AI-generated assets require the `ai` block
  (tool + auditable prompts file; no franchise/artist terms in prompts).
- Licenses are pinned to a URL + verification date. CC-BY attribution is aggregated into CREDITS
  automatically — never skip the `attribution` field.
- Raw AI output never ships: every tier-`ai`/`cc0`/`cc_by` asset passes the restyle checklist
  (Art PRD §3.4) before `restyle_status: done`.

## Content contributions

Game content lives in `/content` as YAML (one entity per file) validated against
[`/schema/content`](schema/content/README.md). Before opening a PR:

```bash
uv run tools/validate_content.py    # schemas + lints (L001–L062); CI runs this on every PR
uv run pytest -q                    # validator test suite
```

- IDs are permanent — never rename a shipped ID; use `deprecated: true` + `superseded_by:`.
- Reference other entities/assets by ID only, never by file path.
- Keep diffs semantic: no reordering, no timestamps, no machine-specific data.

## Documentation contributions

The PRDs/SADs are contracts, not prose. If you change the Baseline feature matrix or a PRD's
scope, run the sync check — CI enforces it:

```bash
uv run tools/check_traceability.py  # every matrix ● must be claimed; PRDs must cite the current baseline
```

Cross-track changes (feature matrix, `/schema`, interface contracts IF-1…IF-10) require sign-off
from every consuming track, recorded in [docs/01-SYNC-DECISIONS.md](docs/01-SYNC-DECISIONS.md).

## Branch workflow

Meridian uses a three-branch promotion model that maps 1:1 to the hosted realms
(see the [CD design](docs/superpowers/specs/2026-07-07-cd-hosted-realms-design.md)):

| Branch | Realm | Advances via |
|---|---|---|
| `dev` | Dev (`meridian-dev`) | day-to-day work — **branch from and PR into `dev`** |
| `ptr` | PTR (`meridian-ptr`) | promotion only: merge `dev → ptr` |
| `main` | Prod (`meridian-prod`) | promotion only: merge `ptr → main` |

- **`dev` is the integration branch.** All feature branches start from `dev` and
  open their PR against `dev`. Never open a PR or commit directly against `ptr` or
  `main` — those advance only through promotion, which ArgoCD rolls out to the PTR
  then Prod realms.
- Each branch's green CI publishes images tagged `:<short-sha>` plus a moving
  per-branch tag (`:dev`, `:ptr`, `:prod`); a promotion is simply a branch merge
  ArgoCD picks up. Rollback is `git revert` on the affected branch.

## Git etiquette

- Binary assets go through Git LFS (patterns in [.gitattributes](.gitattributes)); never force-add
  a binary that LFS should own.
- One logical change per PR; content PRs separate from code PRs where practical.
- Target `dev` with every PR (see **Branch workflow** above); `ptr` and `main` are
  promotion-only.
