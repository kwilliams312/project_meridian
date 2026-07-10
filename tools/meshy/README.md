# `tools/meshy` — Meshy.ai intake CLI

`python -m meshy` automates TD-09 provenance for AI-generated art: it drives
the [Meshy.ai](https://www.meshy.ai/) text-to-3D / image-to-3D API, lands the
finished `.glb`, and writes a policy-compliant IF-8 sidecar + an auditable
prompts file — so no AI-generated asset can merge without complete, correct
provenance (spec ④ §7.1/§7.2; `CONTRIBUTING.md` "Asset provenance (TD-09)").

## Structure

```
tools/meshy/
  __init__.py    package marker.
  client.py      thin httpx wrapper around the Meshy REST API — task creation,
                 polling, download. No policy, no filesystem writes beyond the
                 downloaded .glb bytes. Fully mockable (transport injection).
  intake.py      PURE Python (no network, no bpy) — sidecar/prompts-file
                 shaping, budget pre-check doc-building, triangle counting via
                 pygltflib. Mirrors tools/blender/meridian_export/sidecar.py's
                 "pure module + thin shell" split.
  __main__.py    argparse CLI: refusal gates, orchestrates client.py + intake.py,
                 writes the landed artifacts.
```

`tests/test_meshy.py` mocks **every** HTTP call via `httpx.MockTransport` — CI
never talks to the real Meshy API, matching the repo's mandatory-mocking
testing rule.

## Usage

```bash
export MESHY_API_KEY=...      # never committed; operator-supplied

PYTHONPATH=tools uv run python -m meshy generate \
  --text "a weathered orc warrior, T-pose" \
  --ns core --name orc_warrior --class character_model \
  --terms-verified
```

or from an image reference:

```bash
PYTHONPATH=tools uv run python -m meshy generate \
  --image https://example.com/ref.png \
  --ns core --name orc_warrior --class character_model \
  --terms-verified
```

Lands under `content/<ns>/assets/art/<name>/`:

- `<prefix><name>.glb` — the downloaded model (`sk_`/`sm_` prefix per the
  asset class, same table as `meridian_export/budgets.json`)
- `<name>.asset.yaml` — the IF-8 sidecar
- `<name>.prompts.yaml` — the exact generation request + Meshy task id(s)

`--class` accepts any `asset.schema.yaml` art class (`character_model`,
`armor_model`, `weapon_model`, `kit_piece`, `prop`, `foliage`,
`hero_landmark`, `creature_model`); the budget ceiling and filename prefix
come from the shared `budgets.json` table.

`convert-rig` (task ④/T7, story #506) is **not implemented here** — a
separate story extends this CLI with a `convert-rig` subcommand that maps a
Meshy auto-rig onto the canonical skeleton. `_build_parser` in `__main__.py`
is structured so that subcommand slots in without touching `generate`.

## Refusal gates (TD-09)

Both checked **before any network call**, exit code `2`:

- `MESHY_API_KEY` unset.
- `--terms-verified` not passed — Meshy's commercial-terms check must be
  operator-confirmed at time of use; the flag is the auditable record of that
  confirmation (spec §7.1).

Other failure modes (API error, poll timeout, task ending in a non-`SUCCEEDED`
terminal status, over-budget generation) exit `1`. The **budget pre-check**
runs pygltflib triangle counting against the downloaded `.glb` and calls
`validate_content.check_budget` (the same lint CI runs) *before* any sidecar
or prompts file is written — an over-budget generation fails immediately,
never lands, and the oversized `.glb` is cleaned up. Contributor-visible
review only ever sees on-budget candidates.

## Provenance shape

Every sidecar this CLI writes is structurally quarantined pending a restyle
pass — no new policy, just the existing lints applied automatically:

```yaml
schema: meridian/asset@1
id: core:art.orc_warrior
class: character_model
source: assets/art/orc_warrior/sk_orc_warrior.glb
license: CC-BY-4.0
provenance:
  source_tier: ai
  authors: [meridian-contributors]
  origin_url: https://api.meshy.ai/openapi/v2/text-to-3d/<refine-task-id>
  ai:
    tool: meshy@meshy-5
    prompts_file: orc_warrior.prompts.yaml
budget:
  lod0_tris: 41230
restyle_status: pending
```

`restyle_status: pending` blocks the asset from merging as-is (existing
`validate_content.py` L021/schema lints, Art SAD §3.2) until a human restyle
pass marks it `done` (Art PRD §3.4) — raw Meshy output never ships.

## API notes (docs.meshy.ai, verified 2026-07-10)

- Base URL `https://api.meshy.ai`. Bearer auth (`Authorization: Bearer
  <key>`).
- Text-to-3D is two-stage on `/openapi/v2/text-to-3d`: `mode: preview`
  (untextured mesh) → poll to `SUCCEEDED` → `mode: refine` (keyed by
  `preview_task_id`, adds texture). `client.MeshyClient.text_to_3d()` drives
  both stages and returns a `TaskHandle` for the refine task.
- Image-to-3D is single-stage on `/openapi/v1/image-to-3d` (note the `v1` —
  a real API quirk, not a typo).
- Both poll at `GET <path>/{task_id}`; terminal statuses are `SUCCEEDED`,
  `FAILED`, `CANCELED`. `model_urls.glb` on a `SUCCEEDED` task is the
  presigned download URL.
- `DEFAULT_MODEL_VERSION = "meshy-5"` (`client.py`) is a **pinned** release,
  never `"latest"` — provenance must name an exact, reproducible tool
  version. Bump deliberately when adopting a new Meshy model.

These endpoint shapes were fetched from the public docs at implementation
time rather than exercised against a live account (CI/tests never call the
real API per policy) — verify against a live key before the first production
generation; see the PR for this task for the concern flagged at merge time.
