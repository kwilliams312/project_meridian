# Character Content Foundation (⑤) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
> **Orchestration:** each story → a GitHub issue under the ⑤ epic; subagents branch from `origin/dev`, PR into `dev` (never commit to dev), lead verifies (rerun + `gh pr checks` + **engine/GPU gate for client+content visuals** — implementer sandboxes lack Godot/Blender reliably, so the LEAD runs verify scripts and captures GPU renders) and merges.

**Spec:** [2026-07-11-character-content-foundation-design.md](../specs/2026-07-11-character-content-foundation-design.md)

**Goal:** Replace the greybox blockout with real M1 character content — ardent body, customization v1, Warden's Kit armor, working dyes — end to end on screen.

**Architecture:** Deterministic scripted-original generators (`tools/art/`, stdlib GLB + texture writers, the `generate_pickaxe_blockout.py` pattern) for hard-surface + procedural assets; Meshy→restyle for organics (gated on a rotated key + #525); all assets flow through `meridian_export` E-rules → `validate_imports` I-rules → mcc `pack.data.json`+staged-art (drift-gated) → ContentDB → the assembler. Dyes complete via a mask-tint shader + codec channel decode.

**Tech Stack:** Python 3.12 stdlib (GLB/PNG writers), Blender 4.7-pinned (restyle only), mcc (C++), GDScript (Godot 4.7 shader + assembler), `tools/meshy` CLI, existing golden/staged-pack gates.

## Global Constraints

- Budgets (Art PRD §2.1, enforced by L070–L072 + validator): body 45–60k LOD0 with LOD0–3 chain; armor 3–8k per slot, full outfit ≤40k added; ≤4 skin influences, normalized.
- Geoset regions (the 8): head, hands, forearms, torso, waist, hips_legs, lower_legs, feet. Body meshes `geo_<region>_lod<N>`; gear meshes bind by canonical bone name, NOT geoset-named.
- Provenance: scripted assets `source_tier: original`; Meshy assets `source_tier: ai` + full `ai` block + `restyle_status: done` (the #525 lint blocks `pending`). Meshy stories MUST NOT dispatch until #525 is merged AND the operator confirms `MESHY_API_KEY` is set (lead checks presence with `test -n "$MESHY_API_KEY" && echo present` — never echo the value) AND TD-09 commercial terms confirmed (`--terms-verified`).
- Preset ids stay the stable ints the server validates — ⑤ swaps content behind existing ids; no id renumbering. idmap append-only; `scripts/check-golden.sh --update-golden` regenerates golden + staged pack in lockstep; smoke.c id_count updated.
- Dye masks: one RGB texture per dyeable piece, R/G/B = primary/secondary/accent (① §6). Shader is a parameterized variant of the Character master material (≤12 masters, Art PRD §2.3 — no new master).
- GDScript rules (epic-hardened): no `:=` on Variant exprs; verify scripts use `preload(...).instance()` not bare autoload names; `--import` once on fresh `.godot`; staged `.glb` are LFS (`git lfs pull`).
- Gates per PR: `uv run pytest -q`, `uv run tools/validate_content.py`, `uv run tools/check_traceability.py`, mcc build + `mcc check ./content` + `scripts/check-golden.sh` when content/mcc touched; client-cores ctest when codec touched; lead runs the Godot verify suites + GPU capture for visual stories. `gh pr checks --watch` pasted; all green except #465 content-build.

---

### Story S1: Face/skin procedural textures + generator

**Files:** Create `tools/art/generate_face_skin.py` (stdlib PNG writer, deterministic); `content/core/assets/art/char/ardent/male/face_{1..4}.png` + `skin_{1..3}.png` (staged) + sidecars (`texture_set`, `source_tier: original`); Test: `tests/test_art_gen.py`.

**Interfaces:** Produces 4 face textures + 3 skin palettes as `art.char.ardent.male.face_N` / `skin_N` asset ids, referenced by the catalog (S6). Deterministic — byte-identical regeneration (a test asserts SHA-256 stability across two runs).

- [ ] Failing test: `generate_face_skin.py --out <tmp>` produces 7 PNGs; re-run → identical bytes; each is a valid PNG (magic bytes + IHDR dims within the §2.3 texture cap).
- [ ] Implement the stdlib PNG writer (zlib + CRC, no PIL — follow the deterministic posture of `tools/art/generate_pickaxe_blockout.py`); stylized flat/painterly palettes (face = subtle feature variation, skin = tone ramps).
- [ ] Author sidecars (class `texture_set`, `source_tier: original`, authors `meridian-contributors`, budget within §2.3 cap); stage under the client pack layout; `--update-golden`.
- [ ] GREEN: pytest + `validate_content` (L071 texture cap) + golden gate. Commit `feat(art): procedural face/skin customization textures (⑤/S1)`.

---

### Story S2: Warden's Kit plate generators + 6 item@2 + dye masks

**Files:** Create `tools/art/generate_warden_kit.py` (stdlib GLB, one skinned plate per slot, sized from the rig's BoneSpec spans like `generate_pickaxe_blockout.py`/`generate_blockout.py`); `content/core/assets/art/item/armor/warden_{head,shoulders,chest,hands,legs,feet}.glb` + `_mask.png` + sidecars; `content/core/items/warden_{...}.item.yaml` (6 `item@2` files); Test: `tests/test_art_gen.py` + `tests/test_validate_content.py`.

**Interfaces:** Produces 6 `item@2` items with `worn.models` (skinned plate), `worn.hides` (regions covered: e.g. chest→torso, feet→feet+lower_legs), `worn.dye_channels: [primary, secondary]`, and per-piece RGB dye masks. Consumed by S3 (a masked piece to tint) and S6 (full-kit equip). item_template numeric ids allocated append-only.

- [ ] Failing tests: each generated plate .glb binds only to canonical bones (reuse the I021 checker directly, no Blender); outfit total LOD0 tris ≤40k, each piece 3–8k; each `warden_*.item.yaml` passes item@2 schema + L080/L081 (armor → worn present, no attach, hides valid) + budget lints; a dye mask PNG exists per dyeable piece.
- [ ] Implement the plate generators (deterministic, skinned to the canonical armature via the shared `bones.py` construction) + dye masks (RGB regions) + the 6 item files (worn blocks, icons — reuse/point at existing icon assets or generate simple ones; hides per slot).
- [ ] Sidecars `source_tier: original`; idmap append; `--update-golden`; smoke.c count.
- [ ] GREEN: pytest + validate_content + mcc golden gate. Commit `feat(art): Warden's Kit armor set — 6 slots, dye masks, item@2 (⑤/S2)`.

---

### Story S3: Dye mask-tint shader + codec channel decode

**Files:** Create `client/project/characters/dye_tint.gdshader` (or `.tres` material variant); Modify `client/project/characters/assembled_character.gd` (mask-tint apply path), `client/net/src/codec.cpp` + `codec.h` + `client/net/test/clientnet_test.cpp` (dye channel decode), `client/gdextension/.../meridian_net_thread.cpp` (pass channel through), `client/project/characters/assembled_character_verify.gd`; Depends: S2 (a masked warden piece to test).

**Interfaces:** Codec yields `equipment[].dyes: [{channel:int, dye_id:int}]` (currently flattened to `[int]` — additive restore; the wire always carried channel per ②/T1). Assembler applies mask-tint: per equipped piece with a dye mask + dye_channels, a material variant multiplies each RGB-mask channel by its chosen dye color, preserving unmasked albedo.

- [ ] Failing clientnet test: an EntityEnter/equipment buffer with `DyeChoice{channel:1, dye_id:78}` decodes to `{channel:1, dye_id:78}` (not a bare int). Implement decode + pump pass-through. GREEN clientnet ctest.
- [ ] Failing verify check (`assembled_character_verify.gd`): equip a warden piece with a mask + `dyes:[{channel:0, dye_id:<russet>}]` → the piece's material samples the mask and the primary-channel region reads the russet color; unknown dye → authored colors (unchanged). Lead runs this on the real engine.
- [ ] Implement the `.gdshader` mask-tint (samples mask.rgb, `albedo = base * mix(...)` per channel) + the assembler apply path (replaces the M1 whole-piece `material_override` tint from ②/T3); keep master-material ≤12 (parameterized variant).
- [ ] GREEN: clientnet ctest + pytest + validate_content; lead engine gate. Commit `feat(client): mask-tint dye shader + codec dye-channel decode (⑤/S3)`.

---

### Story S4 *(Meshy — gated on key + #525 merged)*: Ardent body

**Files:** `content/core/assets/art/char/ardent/male/base.glb` (replaces blockout) + sidecar (`source_tier: ai`, `restyle_status: done`); the Meshy prompt/task provenance under the sidecar's `ai.prompts_file`; restyle procedure notes; Test: `tests/test_meridian_rig.py` structural checks on the committed body.

**Interfaces:** Replaces `art.char.ardent.male.base` (same asset id + idmap id — content swap; golden + staged regen). Must pass I020 (63 joints exact) / I022 (8 geosets at lod0) / I023 (authored LOD chain, no `single` exemption).

- [ ] **Precondition gate (lead-owned):** #525 merged; `MESHY_API_KEY` present (lead-confirmed); TD-09 terms confirmed. The subagent receives the key via process env only — never in the prompt.
- [ ] `meshy generate --image/--text ... --terms-verified` → base sculpt; restyle in Blender per the §5 procedure (budget to 45–60k, LOD0–3, geoset cut, rig-skin ≤4 infl); export via `meridian_export` (E100–E105 gate); this run also verifies the #524 bone map if conversion is used.
- [ ] Failing structural tests (committed .glb): 8 geoset meshes, joints ⊆ canonical 63, ≤4 influences, LOD chain present. Sidecar `restyle_status: done` (the #525 lint would fail `pending`).
- [ ] idmap reuse (same id as blockout); `--update-golden`; GREEN full suite; lead GPU-render capture of the real body. Commit `feat(art): ardent male body — Meshy base, restyled to rig (⑤/S4)`.

---

### Story S5 *(Meshy — gated)*: Hair set (4)

**Files:** `content/core/assets/art/char/ardent/male/hair_{1..4}.glb` + sidecars (`source_tier: ai`, restyled); Depends: #525, S4 (body head region to fit). Structural tests in `tests/test_meridian_rig.py`.

- [ ] Same precondition gate as S4.
- [ ] 4 hair meshes: Meshy volumes → restyle (head-region fit, low-poly, skinned to head bone); export via `meridian_export`; sidecars restyled.
- [ ] Failing tests: each hair .glb head-region mount + budget; idmap append; `--update-golden`.
- [ ] GREEN; lead engine check (hair mounts on the body preset). Commit `feat(art): ardent hair customization set (⑤/S5)`.

---

### Story S6: Catalog upgrade + full-kit E2E/GPU proof

**Files:** Modify `content/core/appearance/appearance.ardent.male.*` (preset lists → real S1/S4/S5 assets); a `warden_set` fixture for the preview/E2E; extend `assembled_character_verify.gd` / `world_verify.gd` / `char_select_verify.gd`; Depends: S1–S5.

**Interfaces:** Catalog preset ids unchanged (stable ints) — the referenced asset ids swap from blockout/placeholder to real. The char-preview and world E2E now show the real assembled ardent.

- [ ] Upgrade the catalog preset entries to the real hair/face/skin asset ids (S1/S5) and body (S4); `validate_content` green; `--update-golden`.
- [ ] Extend verify scripts: assemble ardent + full Warden's Kit + a dye → real body visible, all 6 slots' meshes present with correct hides, mask-tint applied. Lead runs on the real engine.
- [ ] **Definition of done:** lead GPU-render capture — ardent in the Warden's Kit with a dye applied, no grey boxes; the two-session E2E still green. Paste evidence.
- [ ] GREEN full suite + golden. Commit `feat(content): upgrade ardent catalog to real assets + full-kit proof (⑤/S6)`.

---

## Dependencies / dispatch order

```
S1 (face/skin) ─┐
S2 (warden kit) ─┼─► S3 (dye shader; needs S2) ─┐
                 │                                ├─► S6 (catalog + proof)
#525 merged + KEY ─► S4 (body) ─► S5 (hair) ─────┘
```
S1, S2 dispatch immediately (∥, disjoint); S3 after S2. S4 gated (key+#525); S5 after S4. S6 last.
Out of scope (spec §1): animations, dye acquisition (#551), second race, transmog, morphs.
