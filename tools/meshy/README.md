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
  mapping.py     PURE Python (no network, no bpy) — loads the versioned
                 bone_map.yaml and resolves a flat list of Meshy bone names into
                 a ConversionPlan (renames / helper-merges / unmapped).
  bone_map.yaml  versioned Meshy→canonical bone table (keyed by Meshy model
                 version). Ships verified: false — see "convert-rig" below.
  convert_rig.py headless-Blender pass (# pragma: no cover): import → rename +
                 merge vertex groups → re-bind to the canonical armature (from
                 meridian_rig.generate_rig) → limit ≤4 + normalize → export.
  __main__.py    argparse CLI: refusal gates, orchestrates client.py + intake.py
                 (generate) and mapping.py + convert_rig.py (convert-rig).
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

## Machine-readable job protocol

Codex and other process orchestrators use `generate --json-events`. In this
mode stdout contains **JSON Lines only**: one UTF-8 JSON object per line,
flushed immediately. Human-readable diagnostics are suppressed. The versioned
JSON Schema is [`job-event.schema.json`](job-event.schema.json); consumers must
check both `schema: meridian/meshy-job-event@1` and `protocol_version: 1` and
must reject unsupported major versions.

```bash
PYTHONPATH=tools uv run python -m meshy generate \
  --text "an original weathered stone marker" \
  --ns core --name stone_marker --class prop \
  --terms-verified --model-version meshy-5 --json-events
```

Every event has monotonically increasing `sequence`, `event`, `schema`, and
`protocol_version` fields. The successful text-to-3D sequence is:

1. `validation.started`, `validation.passed`
2. `preview.submitted`, then one or more `poll.progress` events
3. `refine.submitted`, then one or more `poll.progress` events
4. `download.started`, `download.completed`
5. `budget.started`, `budget.passed`
6. `provenance.started`, `provenance.written`
7. `completed`

Image-to-3D uses `generation.submitted` instead of the preview/refine pair.
Failures terminate with `error`; its stable `error_code` distinguishes
`invalid_request`, `payment_required` (HTTP 402), `rate_limited` (HTTP 429),
`status_drift`, `timeout`, `budget_failed`, `provider_error`, and
`internal_error`. Provider HTTP status is included as `http_status` where
available. Submitted remote IDs are carried on submission events and repeated
as `task_ids` on terminal `completed`, `cancelled`, and runtime `error` events.

### Cancellation and exit codes

Send SIGINT (normally Ctrl-C) to cancel. The CLI removes only that job's staged
download, sidecar, and prompts before emitting `cancelled`; it never removes a
final asset path during failure cleanup. A provider task may continue remotely;
`task_ids` lets the caller audit or manage it through provider tooling without
hiding that fact. The final asset directory is created atomically as a
per-target reservation before client setup, so concurrent jobs for the same
namespace/name cannot both proceed. Output is staged under job-unique names
containing `.partial` and is moved into its final names only after budget and
provenance checks pass. Existing assets are never overwritten.

| Exit | Meaning | Terminal JSON event |
|---:|---|---|
| `0` | Asset landed successfully | `completed` |
| `1` | Provider, timeout, status-drift, budget, or internal runtime failure | `error` |
| `2` | Invalid/refused request; no network call was made | `error` |
| `130` | Operator cancellation (SIGINT/KeyboardInterrupt) | `cancelled` |

`MESHY_API_KEY` remains environment-only. It is never accepted as an argument
or included in an event. As defense in depth, event and human error strings are
recursively scrubbed for the exact key and bearer-token patterns. The model
release is explicit in validation/submission events and provenance; `latest` is
refused before client construction.

or from an image reference — an http(s) URL or a local `.png`/`.jpg`/`.jpeg`
file (local files are base64-encoded into a `data:` URI, the documented
alternative Meshy's `image_url` field accepts):

```bash
PYTHONPATH=tools uv run python -m meshy generate \
  --image ./refs/orc_warrior.png \
  --ns core --name orc_warrior --class character_model \
  --terms-verified
```

For a local image, the prompts file records the submitted data URI's byte
length + SHA-256 digest instead of the raw base64 blob — still pins exactly
which image was submitted, without megabytes of base64 in a YAML companion.

Lands under `content/<ns>/assets/art/<name>/`:

- `<prefix><name>.glb` — the downloaded model (`sk_`/`sm_` prefix per the
  asset class, same table as `meridian_export/budgets.json`)
- `<name>.asset.yaml` — the IF-8 sidecar
- `<name>.prompts.yaml` — the exact generation request + Meshy task id(s).
  Auxiliary companion, not a content entity: `validate_content.py` and mcc
  discovery exclude `*.prompts.yaml` from envelope checks, exactly like the
  `*.render.yaml` strudel manifests (issue #410 precedent).

`--class` accepts any `asset.schema.yaml` art class (`character_model`,
`armor_model`, `weapon_model`, `kit_piece`, `prop`, `foliage`,
`hero_landmark`, `creature_model`); the budget ceiling and filename prefix
come from the shared `budgets.json` table.

## `convert-rig` — Meshy auto-rig → canonical skeleton (spec ④ §7.3)

Converts a Meshy auto-rigged humanoid `.glb` onto the canonical Meridian rig so
it passes the same import rules as hand-made gear (I021: binds only canonical
bones — conversion has no private definition of "good enough").

```bash
PYTHONPATH=tools uv run python -m meshy convert-rig raw_meshy.glb \
  --meshy-version meshy-5 --out canonical.glb \
  --blender /Applications/Blender.app/Contents/MacOS/Blender
```

**How it works.** The map/plan gates run in **pure Python first** (CI never runs
Blender and never needs a live Meshy account):

1. Load `bone_map.yaml` for `--meshy-version` (unknown version → exit 2, naming
   the known versions).
2. Read the input rig's joint names with pygltflib and resolve a `ConversionPlan`
   (`mapping.plan`): mapped bones → **renames**; helper/twist bones named as
   CamelCase/underscore descendants of a mapped bone (e.g. `LeftForeArmTwist`) →
   **merges** into that ancestor's canonical target; anything else → **unmapped**
   (exit 1, listing the names — the table grows deliberately, never silently).
3. Refuse an **unverified** map (see below) unless `--allow-unverified-map`.

Only then is a headless Blender spawned (`convert_rig.py`): rename + merge vertex
groups onto canonical names, drop the Meshy armature, re-bind the mesh to a fresh
canonical armature built from `meridian_rig.bones` (same source as the reference
rig), limit to ≤4 influences + normalize, and re-export. The resolved plan
crosses into Blender as JSON — Blender's bundled Python has no PyYAML, so the map
is only ever loaded in system Python.

### ⚠️ `bone_map.yaml` is an UNVERIFIED seed (DONE_WITH_CONCERNS — issue #524)

Meshy does **not** publish the bone names its rigging emits (checked 2026-07-10:
`docs.meshy.ai/en/api/rigging`, the auto-rigging tutorial, and the ComfyUI/fal.ai
node docs all omit them). The seed therefore assumes the widely-used Mixamo-style
naming with the `mixamorig:` prefix stripped, and ships `verified: false`.
`convert-rig` **refuses** an unverified version unless `--allow-unverified-map`
(a development-only escape hatch used to exercise the pipeline on the committed
fixtures). Reconcile the map against one real Meshy sample via
`mapping.joint_names_from_glb`, then flip `verified: true` — tracked in **#524**.
`verified:` must be a bare YAML boolean; a quoted `"false"` string is refused
outright, never coerced.

The converter binds at the imported rest pose; a geometric **re-pose** to the
canonical T-pose (per-bone rotation deltas) lands with the #524 map verification
— the committed fixture is authored at canonical rest so this is a no-op for it.
The objective gate (I021, names-only) does not depend on the re-pose.

### Fixture gate (Blender local, CI structural)

`tests/fixtures/meshy/build_fixture.py` builds a mini Meshy-style rig
(`meshy_rig_input.glb`, six Mixamo-named bones + a `LeftForeArmTwist` helper
whose box vertices carry a 0.6 helper / 0.4 parent multi-influence split);
`convert-rig` produces `canonical_rig_output.glb`. Both are committed (Git LFS).
`tests/test_meshy.py::test_converted_fixture_binds_only_canonical_bones_I021`
invokes the Task-4 checker (`validate_imports.check_gltf_rig`) directly on the
converted output and asserts every bound joint is a canonical bone;
`test_converted_fixture_conserves_weight_mass` additionally asserts per-vertex
weight sums of 1.0 and per-bone weight-mass conservation through the merge.
Blender runs locally only; CI validates the committed artifacts structurally
(pygltflib), and skips on unsmudged LFS-pointer checkouts.

Regenerate both fixtures (from the repo root; `--allow-unverified-map` is
required because the seed map is unverified):

```bash
BLENDER=/Applications/Blender.app/Contents/MacOS/Blender

"$BLENDER" --background --factory-startup -noaudio \
  --python tests/fixtures/meshy/build_fixture.py -- \
  --out tests/fixtures/meshy/meshy_rig_input.glb

PYTHONPATH=tools uv run python -m meshy convert-rig \
  tests/fixtures/meshy/meshy_rig_input.glb \
  --meshy-version meshy-5 --out tests/fixtures/meshy/canonical_rig_output.glb \
  --allow-unverified-map --blender "$BLENDER"
```

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

**Budget pre-check limitation:** only `lod0_tris` (triangle count) is checked
at intake. `texture_max_px` and `vram_mb` are **not** inspected — that would
require decoding the glb's embedded images (an image-decode dependency this
tool deliberately doesn't carry). If you know those figures, declare them by
hand in the sidecar's `budget:` block; the existing L071 lints then enforce
them in CI. An intake-time texture/VRAM check is a documented follow-up.

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
- Both poll at `GET <path>/{task_id}`; the documented status vocabulary is
  `PENDING`, `IN_PROGRESS`, `SUCCEEDED`, `FAILED`, `CANCELED`. A status
  outside that set makes `poll()` fail **immediately** naming the unexpected
  value (API-shape drift must surface loudly, not decay into a generic
  timeout). `model_urls.glb` on a `SUCCEEDED` task is the presigned download
  URL.
- Image-to-3D's `image_url` accepts a public URL **or** a base64 data URI —
  the CLI's local-file support uses the data-URI form.
- `DEFAULT_MODEL_VERSION = "meshy-5"` (`client.py`) is a **pinned** release,
  never `"latest"` — provenance must name an exact, reproducible tool
  version. Bump deliberately when adopting a new Meshy model.

These endpoint shapes were fetched from the public docs at implementation
time rather than exercised against a live account (CI/tests never call the
real API per policy) — verify against a live key before the first production
generation; see the PR for this task for the concern flagged at merge time.
