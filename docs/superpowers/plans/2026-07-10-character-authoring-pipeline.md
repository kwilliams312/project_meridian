# Character Authoring Pipeline (④) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Orchestration:** each Task maps 1:1 to a GitHub story under the ④ epic; subagents implement on branches, PR into `dev` (NEVER commit to dev directly — standing owner rule), lead verifies (including `gh pr checks` — CI green is a merge gate) and merges.

**Spec:** [2026-07-10-character-authoring-pipeline-design.md](../specs/2026-07-10-character-authoring-pipeline-design.md)

**Goal:** Build the character authoring toolchain — canonical rig generator, geoset conventions, export/import conformance validation, greybox blockout body, and Meshy.ai intake with rig conversion.

**Architecture:** Pure-Python single-source bone table (`bones.py`) consumed by a headless-Blender rig generator, the export addon's new E-rules, and `validate_imports.py`'s new I020+ rules; committed `.glb` artifacts validated structurally in CI via pygltflib (CI never runs Blender); a mocked-API Meshy CLI automates TD-09 AI provenance and converts Meshy auto-rigs onto the canonical armature.

**Tech Stack:** Python 3.12 (pyyaml, jsonschema, + NEW: pygltflib, httpx), Blender (pinned, local-only) `--background --python`, existing `meridian_export` addon pattern (pure module + bpy shell), Git LFS for `.glb`.

## Global Constraints

- Bone contract: the 56 Godot `SkeletonProfileHumanoid` bones (exact names/hierarchy — VERIFY the list in Task 1 against the Godot 4.7 `SkeletonProfileHumanoid` class reference before committing; the table in Task 1 is from memory and the docs win) + 7 sockets: `socket_main_hand`→RightHand, `socket_off_hand`→LeftHand, `socket_shield`→LeftHand, `socket_back`→Chest, `socket_ranged`→Chest, `socket_hip_l`→Hips, `socket_hip_r`→Hips. 63 total.
- T-pose rest matching the Godot profile reference; 1 unit = 1 m; -Z forward, Y up on export (art-prd §4.2).
- `skeleton.defs.yaml` `bones:` is populated from `bones.py` — additive-only evolution; drift-guard test mandatory.
- Geosets: body meshes named `geo_<region>_lod<N>`, regions = the 8 in skeleton.defs; LOD0 must cover all 8; ≤4 influences/vertex, normalized.
- Gear (armor_model) meshes are NOT geoset-named; they bind only to canonical bones.
- Structural determinism, not byte determinism: tests assert bone names/hierarchy/rest transforms via pygltflib on committed `.glb`s. Blender version pinned in `tools/blender/README.md` (pin whatever current stable is installed, e.g. 4.5 LTS; record exactly).
- CI never runs Blender and never calls the Meshy API (all HTTP mocked; `MESHY_API_KEY` env only; CLI refuses without `--terms-verified`).
- AI intake sidecars: `source_tier: ai`, `ai.tool: "meshy@<model-ver>"`, auto `prompts_file`, `origin_url` = task URL, `restyle_status: pending` (existing lint quarantines).
- Addon pattern (binding): pure logic in bpy-free modules (unit-testable, bpy mocked at import like `tests/test_meridian_export.py`); thin bpy shells `# pragma: no cover`.
- New deps land in `pyproject.toml` in the FIRST task that uses them (pygltflib → Task 1's tests? no — Task 2; httpx → Task 6), `uv lock` refreshed in that task.
- Python suites: `uv run pytest -q`; content validation `uv run tools/validate_content.py`; one negative fixture per new rule (repo convention).
- LFS: `.glb` is already an LFS pattern (`.gitattributes`); committed artifacts go under `content/core/assets/art/char/` with pack-root-relative `source`.
- Blender-requiring tasks (2, 5, 7) MUST check `blender --version` first and report BLOCKED if absent — do not fake artifacts.

---

### Task 1: `bones.py` — canonical bone table + skeleton.defs population + drift guard

**Files:**
- Create: `tools/blender/meridian_rig/__init__.py` (empty package marker), `tools/blender/meridian_rig/bones.py`
- Modify: `schema/content/skeleton.defs.yaml` (fill `bones:`)
- Test: `tests/test_meridian_rig.py`

**Interfaces:**
- Produces: `bones.py` module, importable WITHOUT Blender:
  - `PROFILE_BONES: list[BoneSpec]` — 56 entries; `BoneSpec = dataclass(name: str, parent: str | None, head_m: tuple[float,float,float], tail_m: tuple[float,float,float])` in the default (ardent male, 1.8 m) proportion profile, T-pose.
  - `SOCKET_BONES: list[BoneSpec]` — 7 entries per Global Constraints.
  - `ALL_BONES = PROFILE_BONES + SOCKET_BONES` (63).
  - `bone_names() -> list[str]`, `hierarchy() -> dict[str, str | None]`, `for_profile(profile: str) -> list[BoneSpec]` (v1: only `"ardent_male"`; unknown key → `ValueError` naming valid profiles).
- Consumed by: Tasks 2, 3, 4, 5, 7.

- [ ] **Step 1: Verify the 56-name list** against the Godot 4.7 `SkeletonProfileHumanoid` class reference (docs.godotengine.org). Expected set (memory — the docs win): Root, Hips, Spine, Chest, UpperChest, Neck, Head, LeftEye, RightEye, Jaw; per side: Shoulder, UpperArm, LowerArm, Hand, ThumbMetacarpal, ThumbProximal, ThumbDistal, Index/Middle/Ring/Little × Proximal/Intermediate/Distal, UpperLeg, LowerLeg, Foot, Toes.
- [ ] **Step 2: Write failing tests** in `tests/test_meridian_rig.py`:

```python
"""Tests for meridian_rig.bones — the canonical bone table (spec ④ §2)."""
import sys
from pathlib import Path
import yaml

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "blender" / "meridian_rig"))
import bones  # noqa: E402


def test_bone_count_is_63():
    assert len(bones.ALL_BONES) == 63
    assert len(bones.PROFILE_BONES) == 56
    assert len(bones.SOCKET_BONES) == 7

def test_hierarchy_is_wellformed_single_root():
    h = bones.hierarchy()
    roots = [n for n, p in h.items() if p is None]
    assert roots == ["Root"]
    names = set(h)
    assert all(p in names for p in h.values() if p is not None)

def test_socket_parents_match_spec():
    parents = {b.name: b.parent for b in bones.SOCKET_BONES}
    assert parents == {
        "socket_main_hand": "RightHand", "socket_off_hand": "LeftHand",
        "socket_shield": "LeftHand", "socket_back": "Chest",
        "socket_ranged": "Chest", "socket_hip_l": "Hips", "socket_hip_r": "Hips"}

def test_skeleton_defs_bones_matches_table():
    """Drift guard: schema/content/skeleton.defs.yaml bones == bones.py names."""
    defs = yaml.safe_load((REPO / "schema/content/skeleton.defs.yaml").read_text())
    assert defs["$defs"]["boneName"]["enum"] == bones.bone_names()

def test_unknown_profile_raises():
    import pytest
    with pytest.raises(ValueError, match="ardent_male"):
        bones.for_profile("dwarf_female")
```

- [ ] **Step 3: Verify FAIL** — `uv run pytest tests/test_meridian_rig.py -q` → import error.
- [ ] **Step 4: Implement `bones.py`** — dataclass + the two tables with T-pose head/tail coordinates for the 1.8 m default profile (symmetric L/R; hands out along ±X at shoulder height for T-pose; finger segments a few cm each). Coordinates need anatomical plausibility, not artistry — the blockout body (Task 5) is fitted to them.
- [ ] **Step 5: Fill `skeleton.defs.yaml`** — `boneName:` gains `enum: [Root, Hips, ...all 63 in bones.py order...]` (keep the existing description; note "populated by ④, source: meridian_rig/bones.py"). NOTE: `validate_imports`/export checks read this enum; empty-means-skip behavior ends here.
- [ ] **Step 6: Verify GREEN** — targeted pytest, then full `uv run pytest -q` and `uv run tools/validate_content.py` (all existing suites must stay green — the enum fill must not break any current content).
- [ ] **Step 7: Commit** — `git commit -m "feat(rig): canonical bone table (56 profile + 7 sockets) + skeleton.defs population (④/T1)"`

---

### Task 2: rig generator + committed rig artifact

**Files:**
- Create: `tools/blender/meridian_rig/generate_rig.py`
- Create (generated, LFS): `content/core/assets/art/char/sk_ardent_male_skeleton.glb`, `content/core/assets/art/char/ardent_male_skeleton.asset.yaml`
- Modify: `tools/blender/README.md` (Blender version pin + regeneration command), `pyproject.toml` (+`pygltflib`), `uv.lock`
- Test: `tests/test_meridian_rig.py` (structural tests on the committed .glb)

**Interfaces:**
- Consumes: `bones.for_profile("ardent_male")`, `bones.ALL_BONES`.
- Produces: the committed rig `.glb` (asset id `art.char.ardent.male.skeleton`, class `character_model`) that Tasks 5 and 7 instantiate; regeneration command `blender --background --python tools/blender/meridian_rig/generate_rig.py -- --profile ardent_male --out <path>`.

- [ ] **Step 1: Check Blender** — `blender --version`; absent → report BLOCKED (do not fake the artifact). Record the exact version in `tools/blender/README.md` as the pin.
- [ ] **Step 2: Write failing structural tests** (pygltflib; add the dependency now):

```python
def _load_rig_gltf():
    from pygltflib import GLTF2
    return GLTF2().load(str(REPO / "content/core/assets/art/char/sk_ardent_male_skeleton.glb"))

def test_rig_glb_bone_names_match_table():
    g = _load_rig_gltf()
    node_names = {n.name for n in g.nodes}
    assert set(bones.bone_names()) <= node_names

def test_rig_glb_hierarchy_matches_table():
    g = _load_rig_gltf()
    idx = {n.name: i for i, n in enumerate(g.nodes)}
    child_to_parent = {}
    for i, n in enumerate(g.nodes):
        for c in (n.children or []):
            child_to_parent[g.nodes[c].name] = n.name
    for name, parent in bones.hierarchy().items():
        if parent is not None:
            assert child_to_parent.get(name) == parent, name
```

- [ ] **Step 3: Verify FAIL** (no .glb yet), then **implement `generate_rig.py`**: bpy script — parse `--` args, build an armature object from `bones.for_profile(...)` (edit bones: head/tail from BoneSpec), export via `bpy.ops.export_scene.gltf` with the addon's canonical settings (axis conversion default, no animations, `export_yup=True`). Keep ALL logic that can be pure (arg parsing, path shaping) in importable functions at module top so pytest covers them with bpy absent.
- [ ] **Step 4: Generate + write the sidecar** — run the command; author `ardent_male_skeleton.asset.yaml` by hand following an existing `character_model` sidecar as template: `id: core:art.char.ardent.male.skeleton`, `class: character_model`, `source: assets/art/char/sk_ardent_male_skeleton.glb`, `license: CC-BY-4.0`, `provenance: {source_tier: original, authors: [Project Meridian contributors]}`.
- [ ] **Step 5: Verify GREEN** — structural tests pass; `uv run tools/validate_content.py` (sidecar valid, L020 resolves); full pytest.
- [ ] **Step 6: Commit** (glb via LFS — confirm `git check-attr filter content/core/assets/art/char/sk_ardent_male_skeleton.glb` says lfs) — `git commit -m "feat(rig): deterministic rig generator + committed ardent_male skeleton (④/T2)"`

---

### Task 3: geoset conventions + export-addon E-rules

**Files:**
- Create: `tools/blender/meridian_export/rig_checks.py` (PURE — no bpy)
- Modify: `tools/blender/meridian_export/__init__.py` (call rig checks for skeletal classes at export; thin shell only), `tools/blender/README.md` (E-rule table)
- Test: `tests/test_meridian_export.py` (extend)

**Interfaces:**
- Consumes: `bones.bone_names()` (import via the same sys.path pattern) and `schema/content/skeleton.defs.yaml` geoset regions.
- Produces: `rig_checks.check_rig(data: RigData) -> list[str]` where `RigData = dataclass(asset_class: str, bone_names: list[str], socket_names: list[str], mesh_names: list[str], max_influences: int, weights_normalized: bool)` — the bpy shell builds `RigData` from the scene; pure module returns `"E1xx ..."` error strings:
  - E100 bone names ⊆ skeleton.defs bones; E101 all 7 sockets present on skeleton export; E102 geoset naming/LOD0 coverage on `character_model` bodies (`geo_<region>_lod<N>`, all 8 at lod0); E103 ≤4 influences + normalized; E104 unknown `geo_*` region name.
- Gear rule: `armor_model` meshes must NOT be `geo_*`-named (part of E104).

- [ ] **Step 1: Failing tests** — pure-module tests, one negative per rule (construct `RigData` literals: extra bone → E100; missing socket_back → E101; body missing geo_waist_lod0 → E102; 5 influences → E103; `geo_tail_lod0` → E104; armor mesh named `geo_torso_lod0` → E104). Positive: a conforming RigData → `[]`.
- [ ] **Step 2: Verify FAIL → implement `rig_checks.py`** — reads skeleton.defs via yaml at module load (same repo-relative resolution `sidecar.py` uses for budgets.json); pure functions; error strings carry the offending names.
- [ ] **Step 3: Wire the bpy shell** — in `MERIDIAN_OT_export_asset`, for classes `character_model`/`armor_model`, build `RigData` from the armature/meshes (`# pragma: no cover`) and abort export listing errors, matching the existing convention-check flow.
- [ ] **Step 4: GREEN + full suite + commit** — `git commit -m "feat(export): rig/geoset E-rules for skeletal classes (④/T3)"`

---

### Task 4: import-validator I020–I023

**Files:**
- Modify: `tools/validate_imports.py` (new rules + pygltflib glb inspection), `client/import-presets/presets.json` (ensure `character_model`/`armor_model` presets exist with `lod_policy` permitting `single` for blockouts — follow IPRESET structure)
- Test: `tests/test_validate_imports.py` (extend; build tiny .glb fixtures programmatically with pygltflib in a tmp_path fixture — no Blender)

**Interfaces:**
- Consumes: `bones.bone_names()` via skeleton.defs.yaml (read the YAML — validators must not import Blender-adjacent modules), pygltflib.
- Produces: rules that gate Tasks 5/7 outputs:
  - I020 `character_model` skeleton assets: joint-node name set is exactly the canonical 63 (missing and extra bones each listed by name); I021 skins in `armor_model` reference only canonical joint names; I022 `character_model` body meshes: geoset naming + all 8 regions at lod0; I023 skeletal LOD chain present unless sidecar `import_hints.lod_policy: single`.
  - Empty/absent `bones:` enum → rules skip (preserves contract-① behavior on branches lacking T1; assert this in a test).

- [ ] **Step 1: Failing tests** — programmatic fixtures: `_make_glb(nodes=[...], skins=..., meshes=...)` helper via pygltflib writing to tmp_path; one negative per rule + a passing skeleton fixture; the skip-when-enum-empty test monkeypatches the loaded defs.
- [ ] **Step 2: FAIL → implement** — follow the existing rule-function style (`check_sidecar`-adjacent; add `check_gltf_rig(sidecar, glb_path, defs) -> list[str]`); wire into the same in-process path `validate_content.py` already invokes; `--imports error` posture unchanged.
- [ ] **Step 3: GREEN + full suite** — including `uv run tools/validate_content.py` against real content (the committed rig from T2 must pass I020 — run after rebasing on T2's merge; coordinate via story dependency).
- [ ] **Step 4: Commit** — `git commit -m "feat(imports): I020-I023 rig/geoset/LOD conformance rules (④/T4)"`

---

### Task 5: blockout body

**Files:**
- Create: `tools/blender/meridian_rig/generate_blockout.py`
- Create (generated, LFS): `content/core/assets/art/char/sk_ardent_male_base.glb`, `content/core/assets/art/char/ardent_male_base.asset.yaml` (`import_hints: {lod_policy: single}`)
- Test: `tests/test_meridian_rig.py` (structural: 8 geoset meshes present, skinned to canonical joints, ≤4 influences)

**Interfaces:**
- Consumes: Task 2's generator module (instantiates the armature from `bones.py`), Task 3 E-rule names, Task 4 I-rules as the objective gate.
- Produces: `art.char.ardent.male.base` — the body spec ② will assemble and spec ⑤ will replace.

- [ ] **Step 1: Check Blender (BLOCKED if absent).**
- [ ] **Step 2: Failing structural tests** (committed-glb assertions mirroring Task 2's style: mesh names == {geo_head_lod0,…,geo_feet_lod0}; every skin joint ∈ canonical names).
- [ ] **Step 3: Implement `generate_blockout.py`** — primitives per region (box/capsule via `bpy.ops.mesh.primitive_*`), sized from the corresponding BoneSpec head/tail spans, parented with automatic weights then clamped to ≤4 influences (`bpy.ops.object.vertex_group_limit_total(limit=4)` + normalize), named per geoset convention, joined per region, exported with the same settings as T2.
- [ ] **Step 4: Generate; write sidecar** (class `character_model`, `source_tier: original`, `import_hints.lod_policy: single`); run the FULL chain as the end-to-end proof: export → `uv run tools/validate_content.py` (I020–I023 incl.) → mcc build + `mcc check ./content` + emit (worn/pck path) — paste outputs in the PR.
- [ ] **Step 5: GREEN + commit** — `git commit -m "feat(rig): greybox blockout body, geoset-cut + skinned (④/T5)"`

---

### Task 6: Meshy intake CLI (generate/fetch/provenance)

**Files:**
- Create: `tools/meshy/__init__.py`, `tools/meshy/client.py` (API), `tools/meshy/intake.py` (normalize/land/sidecar), `tools/meshy/__main__.py` (CLI), `tools/meshy/README.md`
- Modify: `pyproject.toml` (+`httpx`), `uv.lock`
- Test: `tests/test_meshy.py`

**Interfaces:**
- Consumes: nothing from other tasks (pure Python; budget pre-check reuses `validate_content.check_budget` + pygltflib like `validate_imports`).
- Produces: `python -m meshy generate --text "..." [--image ref.png] --ns core --name <asset_name> --class <asset_class> --terms-verified` → downloads glb to `content/<ns>/assets/art/<name>/`, writes sidecar + prompts file; `client.MeshyClient(api_key).text_to_3d(prompt) -> TaskHandle`, `.poll(task_id) -> TaskStatus`, `.download(task_id, dest) -> Path`. Task 7 extends this CLI.

- [ ] **Step 1: Failing tests (httpx fully mocked — `@patch`/transport mock, NEVER live):** happy path (generate→poll SUCCEEDED→download→sidecar exists and passes `validate_content` schema + `check_provenance` with `source_tier: ai`, `restyle_status: pending`, prompts_file content == exact request payload + task id); refusal paths (`MESHY_API_KEY` unset → exit 2 before any HTTP; missing `--terms-verified` → exit 2, message cites TD-09); API error and poll-timeout paths; budget pre-check failure path (mock glb over class cap → nonzero exit, sidecar NOT written).
- [ ] **Step 2: FAIL → implement** — `client.py` thin httpx wrapper (base URL constant, bearer auth, versioned model param recorded into `ai.tool` as `meshy@<model>`); `intake.py` pure functions for sidecar/prompts-file shaping (mirror `meridian_export/sidecar.py` structure so the sidecar dict passes the same schema/lints); CLI argparse in `__main__.py`.
- [ ] **Step 3: GREEN + full suite + commit** — `git commit -m "feat(meshy): AI-asset intake CLI with TD-09 provenance automation (④/T6)"`

---

### Task 7: Meshy rig conversion

**Files:**
- Create: `tools/meshy/bone_map.yaml` (versioned: `meshy_model_version → {meshy_bone: canonical_bone}`), `tools/meshy/convert_rig.py` (bpy headless), `tools/meshy/mapping.py` (PURE: load/validate map, resolve unmapped→ancestor merge plan)
- Modify: `tools/meshy/__main__.py` (`convert-rig` subcommand), `tools/meshy/README.md`
- Test: `tests/test_meshy.py` (mapping logic), `tests/test_meridian_rig.py` (converted-fixture gate)

**Interfaces:**
- Consumes: `bones.py` (canonical armature via Task 2's generator), Task 4 I-rules (the objective gate), Task 6 CLI frame.
- Produces: `python -m meshy convert-rig <glb> --meshy-version <v> --out <glb>`; `mapping.plan(meshy_bones: list[str], version: str) -> ConversionPlan` where `ConversionPlan = dataclass(renames: dict[str,str], merges: dict[str,str], unmapped: list[str])` — non-empty `unmapped` is a hard error listing names.

- [ ] **Step 1: Failing pure tests** — `mapping.plan()`: known map version resolves renames; helper/twist bones absent from the map but descending from mapped bones → `merges` into nearest mapped ancestor; truly unknown names → `unmapped` (and CLI exits nonzero listing them); unknown `--meshy-version` → error naming known versions.
- [ ] **Step 2: FAIL → implement `mapping.py` + `bone_map.yaml`** (seed the map for the current Meshy rig naming — obtain one real sample locally OR mark the initial table `# UNVERIFIED — populate from first live sample` and gate `convert-rig` behind the map's `verified: true` flag so the seed cannot silently mis-convert; the story is DONE_WITH_CONCERNS until a live sample verifies it).
- [ ] **Step 3: Implement `convert_rig.py`** (bpy): import glb → apply `ConversionPlan` (rename mapped bones; for merges transfer vertex-group weights into the target then delete the group) → re-pose mapped bones' rest to canonical T-pose (rotation deltas from `bones.py` rest orientations, applied with mesh) → delete old armature, bind mesh to a fresh canonical armature (from Task 2's generator function) preserving transferred weights → limit 4 + normalize → export.
- [ ] **Step 4: Fixture gate** — build a mini "Meshy-style" rigged glb fixture programmatically (pygltflib, ~6 bones with Meshy-style names + a twist helper); run convert-rig locally (Blender required — BLOCKED if absent); commit the converted fixture output under `tests/fixtures/meshy/` and assert it passes I021 (binds only to canonical bones) via the Task-4 checker invoked directly.
- [ ] **Step 5: GREEN + full suite + commit** — `git commit -m "feat(meshy): auto-rig conversion onto the canonical skeleton (④/T7)"`

---

## Task dependencies / story mapping

```
T1 (bones) ──► T2 (rig glb) ──► T5 (blockout, e2e proof)
     ├──────► T3 (E-rules)        ▲
     └──────► T4 (I-rules) ───────┘ (gate)
T6 (meshy intake) — independent
T2 + T4 + T6 ──► T7 (rig conversion)
```

Out of scope (spec §11): animations, face bones/morphs, real bodies/catalogs (⑤), client assembler (②), Meshy retexture/creature rigs, mcc L080+ port.
