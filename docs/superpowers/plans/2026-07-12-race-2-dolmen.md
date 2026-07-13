# Race #2 (Dolmen) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).
> **Orchestration:** each story → a GitHub issue under the Dolmen epic; subagents branch from `origin/dev`, PR into `dev` (never commit to dev); lead verifies (rerun + `gh pr checks` + **GPU render** for D2/D4) and merges.

**Spec:** [2026-07-12-race-2-dolmen-design.md](../specs/2026-07-12-race-2-dolmen-design.md)

**Goal:** A playable second race (Dolmen) that renders its own body on the shared 63-bone skeleton and wears the existing Ardent-authored Warden's Kit + sword with no re-authoring — proving "a model per race."

**Architecture:** A new `dolmen_male` PROPORTION PROFILE in `bones.py` (same 56+7 bone names/hierarchy, stockier/shorter rest transforms) drives a Dolmen skeleton via `generate_rig`; a Meshy body is restyled onto it; a per-race catalog is added; then the crux — assemble Dolmen + Ardent's gear (bound by bone name) and confirm it deforms, using item@2 `race_overrides` only where the fit-check fails.

**Tech Stack:** Python (bones.py), Blender 5.0-pinned (generate_rig/restyle_body), Meshy CLI, mcc/golden/staged, Godot assembler (unchanged), the GPU render gate.

## Global Constraints
- Dolmen = shorter/broader/stockier ("stone folk") — express ONLY as rest transforms; bone NAMES + hierarchy identical to `ardent_male` (the shared-skeleton invariant; a test asserts identical names).
- Skeleton per race/sex: `art.char.dolmen.male.skeleton` (original tier). Body `art.char.dolmen.male.base` (ai tier, restyle_status done). Catalog `appearance.dolmen.male`.
- Roster: Dolmen = frozen race id 2 (roster.h + client MeridianRoster) — already present; NO wire/roster change, just confirm selectable.
- Reuse the proven pipelines (generate_rig, restyle_body, catalog schema, assembler) — no new systems, no assembler/contract logic changes.
- Meshy stories (D2): key in env (never printed/committed), --terms-verified, restyle_status done (L024). Determinism/golden/staged gates; check_staged_models covers Dolmen assets. Blender pin 5.0.0.
- I020 (63 joints exact) / I022 (8 geosets) / I023 (LOD chain) on the Dolmen skeleton + body.

---

### D1: Dolmen proportion profile + skeleton
**Files:** Modify `tools/blender/meridian_rig/bones.py` (add `dolmen_male` to `VALID_PROFILES` + its BoneSpec table); Create (LFS) `content/core/assets/art/char/sk_dolmen_male_skeleton.glb` + sidecar; Modify `schema/content/skeleton.defs.yaml`? NO — bones enum is name-only, unchanged. Test: `tests/test_meridian_rig.py`.

**Interfaces:** Produces `bones.for_profile("dolmen_male")` — the 63 canonical bones with Dolmen rest transforms (same names as ardent_male). Consumed by D2 (restyle target) + D4.

- [ ] Failing tests: `for_profile("dolmen_male")` returns 63 bones; `bone_names("dolmen_male") == bone_names("ardent_male")` and `hierarchy()` identical (names/parents same, only transforms differ — the shared-skeleton invariant); at least one transform differs (proportions actually changed, e.g. shorter total height, wider shoulders).
- [ ] Implement the `dolmen_male` BoneSpec table: derive from ardent_male, scale to a stockier build (≈0.86–0.90× height, ~1.1× shoulder/hip width, shorter legs, thicker via the body mesh not bones). Keep every bone name/parent identical.
- [ ] Generate the skeleton: `blender … generate_rig.py -- --profile dolmen_male --out content/core/assets/art/char/sk_dolmen_male_skeleton.glb` (pin 5.0.0, bounded timeout, --factory-startup). Sidecar class character_model, source_tier original. Stage; `--update-golden`; smoke.c.
- [ ] Structural tests on the committed .glb (pygltflib): exactly the 63 canonical joint names; hierarchy matches. I020 passes.
- [ ] GREEN pytest + validate_content + validate_imports + golden gate. Commit `feat(rig): dolmen_male proportion profile + skeleton (D1)`.

---

### D2 *(Meshy)*: Dolmen body
**Files:** Create (LFS) `content/core/assets/art/char/sk_dolmen_male_base.glb` + sidecar + prompts; possibly extend `restyle_body.py` to take the target profile/skeleton (it likely already parameterizes the region bounds — read it; if hardcoded to ardent, add a `--profile`/`--skeleton` arg). Test: `tests/test_meridian_rig.py`.

**Interfaces:** Produces `art.char.dolmen.male.base` — a stockier body, 8 `geo_<region>_lod<N>` geosets, skinned to the Dolmen skeleton's 63 bones. Consumed by D3 (catalog body_model) + D4.

- [ ] Precondition: `test -n "$MESHY_API_KEY"` present (BLOCKED if absent); Blender 5.0.0.
- [ ] Meshy generate a stockier stone-folk humanoid (`--text "…stocky broad mountain-folk warrior, thick build…" --terms-verified`); restyle_body fit to the DOLMEN skeleton region bounds (from D1), 8 geosets, LOD chain, ≤4 influences, skin to the 63 Dolmen bones. Budget 45–60k LOD0 (art-prd §2.1).
- [ ] Sidecar source_tier ai + full provenance + restyle_status done + prompts file. Stage; `--update-golden`.
- [ ] Failing→green structural tests: 8 geoset meshes, joints ⊆ Dolmen canonical 63, ≤4 influences, LOD chain. I020/I022/I023 pass. Key never printed/committed.
- [ ] GREEN full gates. Lead GPU-renders the Dolmen body alone (reads as a stockier humanoid). Commit `feat(art): dolmen male body — Meshy → restyle to dolmen skeleton (D2)`.

---

### D3: Dolmen catalog + roster/create-UI confirm
**Files:** Create `content/core/appearance/appearance.dolmen.male.yaml` (or the established path); possibly reuse ⑤ Ardent preset assets; Test: extend char-select verify.

**Interfaces:** Produces `appearance.dolmen.male` (skeleton=D1, body_model=D2, hair/face/skin presets). Consumed by the assembler when race=2.

- [ ] Author the Dolmen catalog: `meridian/appearance_catalog@1`, race dolmen, sex male, skeleton + body from D1/D2. Presets: M1-lean — reuse the ⑤ Ardent hair/face/skin asset ids (they're generic enough) OR a small re-tint; keep stable preset ids. validate_content green (schema + L082/L083 per-race-sex uniqueness).
- [ ] Confirm the client: `MeridianRoster.RACES` includes Dolmen (id 2) and char-create can select it; the preview + create path resolve `catalog(2, 0)` (ContentDB) → the Dolmen body. Extend char_select_verify: selecting Dolmen assembles the Dolmen body (not Ardent). Lead runs the verify on the engine.
- [ ] `--update-golden`; GREEN gates. Commit `feat(content): dolmen appearance catalog + create-UI wiring (D3)`.

---

### D4: The fit-check + race_overrides (the proof)
**Files:** Possibly `content/core/items/warden_*.item.yaml` (add `race_overrides.dolmen` ONLY for pieces that fail); Test: assembler verify (Dolmen + kit).

**Interfaces:** Consumes D1–D3 + the merged Warden's Kit (#594) + sword (#605). Produces the DoD render + any sparse overrides.

- [x] Extend `assembled_character_verify.gd`: assemble `(race=2, sex=0, appearance, full Warden's Kit + sword)` → all pieces mount, geoset hides correct, dye applies, sword on the hand. (Structural — the LEAD's GPU render judges the deformation quality.) → new `_verify_dolmen_fitcheck` phase, the standing regression guard (see Proof outcome below).
- [x] **Lead GPU render (the proof):** Dolmen + full dyed Warden's Kit + hair + sword. Assess each piece: does the Ardent-authored gear deform acceptably onto Dolmen's proportions (bound by bone name)? Expected: yes (some looseness OK). → **DONE by the lead — deforms cleanly by bone name; render attached to #615.**
- [x] For ANY piece that clips/voids badly on Dolmen: author `race_overrides.dolmen` on that item@2 file … If ZERO overrides needed, state that explicitly — it's the strongest proof. → **ZERO `race_overrides` needed** (see Proof outcome). The ① escape hatch went unused — the strongest form of the proof.
- [x] GREEN full gates + the DoD render. Commit `feat(content): dolmen fit-check + race_overrides for any failing piece (D4)`.

**Proof outcome (D4 — the "model per race" result):**
The Dolmen (race 2, sex 0) wears the **Ardent-authored Warden's Kit** (head/shoulders/chest/hands/legs/feet) + hair + iron sword, all russet-dyed, with **ZERO `race_overrides.dolmen`**. The gear was authored ONCE, on Ardent; it binds onto the Dolmen's stockier/shorter 63-bone skeleton **purely by bone name** (both skeletons share bone names + hierarchy; only rest transforms differ). No per-race gear duplication, no stretched "universal" mesh — one mesh per race, gear reused across races. The lead's D4 GPU render confirms the deformation reads cleanly (some looseness from proportion mismatch is acceptable greybox-plus, per spec §D4).

- **Regression guard:** `client/project/characters/assembled_character_verify.gd → _verify_dolmen_fitcheck` (headless, no render). It asserts `assemble(2, 0, …, full dyed kit + sword) == true`, the canonical `Skeleton3D` has exactly **63** bones, **no `assembly_failed`** fires across the whole assemble+equip, the six armour pieces + the sword each mount (sword on the `socket_main_hand` `BoneAttachment3D`), the hide union hides `head/torso/hands/hips_legs/feet` while `forearms/lower_legs/waist` stay visible, and **11 meshes render** (3 uncovered body geosets + 6 armour + hair + sword) — matching the lead's GPU render. If cross-race gear reuse ever regresses, this phase goes red.
- **Run it:** `godot --headless --path client/project --import` once (builds the `MeridianRoster` global-class cache), then `godot --headless --path client/project --script res://characters/assembled_character_verify.gd` → `ALL RUNTIME CHECKS PASS`, exit 0.

---

## Dependencies / dispatch order
```
D1 (skeleton) ─► D2 (body) ─► D3 (catalog) ─► D4 (fit-check + overrides)
```
Strictly sequential (each consumes the prior). D1 dispatches now; D2 gated on the key (already set); D4 is the model-per-race proof.
Out of scope (spec §1): races 3/4, Dolmen-specific armor, female bodies, assembler/contract changes.
