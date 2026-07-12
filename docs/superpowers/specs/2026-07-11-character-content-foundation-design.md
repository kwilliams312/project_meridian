# Design — Character Content Foundation (⑤): Real Ardent Body, Customization v1, Starter Armor, Dyes

**Date:** 2026-07-11
**Track:** Art (lead) + Tools + Client (dye shader/codec)
**Status:** Draft (design approved in brainstorming; pending spec review → implementation plan)
**Relates to:** ① #451, ④ #507, ② #542; CHR-01, ITM-01, A-03/D-32; Art PRD §2.1/§2.3/§5;
follow-ups #524 (Meshy map), #525 (quarantine lint — MUST merge before Meshy content), #551 (dye application).

## 1. Overview

①/④/② built the contract, the pipeline, and the runtime assembler; everything visible
is still the greybox blockout. This package delivers the **M1 character content
foundation** — the real assets that replace the blockout end to end — and completes
the dye system the ② handoff left as machinery-without-content.

Deliverables:
1. **Ardent male body** — real stylized low-poly, LOD chain, 8-geoset cut, skinned to
   the canonical rig (replaces `art.char.ardent.male.base`).
2. **Customization set v1** (Art PRD §5) — 4 hair meshes, 4 face texture presets, 3
   skin palettes; the catalog's placeholder presets become real.
3. **"Warden's Kit" starter armor** — 6 visible slots (head, shoulders, chest, hands,
   legs, feet) as `item@2` pieces with worn blocks, hides, budgets, and RGB dye masks.
4. **Dye completion** — codec regains the dropped dye *channel*; the assembler upgrades
   from material-replacement tint to a mask-based tint shader preserving albedo.

Definition of done: the ②-finale shot, re-rendered — an ardent in the Warden's Kit with
a dye applied, no grey boxes.

### Sourcing (owner decision 2026-07-11: hybrid)

- **Meshy-tier** (organic): body base sculpt, hair volumes → agent **restyle pass** in
  Blender (budget cleanup, geoset cut, rig-skin), export through `meridian_export` (E-rules
  gate), pass I-rules in CI, `restyle_status: done` flipped in the reviewed PR. Requires
  `MESHY_API_KEY` in the operator env + TD-09 terms confirmation; first real run also
  verifies the #524 bone map. These stories are GATED on #525 (quarantine lint) merging first.
- **Scripted-original** (hard-surface + procedural): Warden's Kit plate generators, dye
  masks, face/skin textures, the dye shader. No key; dispatch immediately.

### Non-goals

- Animations (own package); dye acquisition/economy (#551); second race (M2); transmog;
  the optional 1–2 morphs (A-03 — ship only if budget allows, deferred here); face/skin
  as anything beyond preset textures/palettes.

## 2. Body + customization (Art + Tools)

- Body: Meshy base → restyle to Art PRD §2.1 (45–60k LOD0, LOD0–3 chain, ≤120 bones,
  ≤4 influences), cut into the 8 `geo_<region>_lod<N>` meshes, skinned by canonical bone
  name. Sidecar class `character_model`, `source_tier: ai`, full AI provenance +
  `restyle_status: done`. Replaces the blockout base asset id (idmap id reused; golden +
  staged-pack regen).
- Hair: 4 meshes (Meshy → restyle), head-region mounts, `source_tier: ai`, restyled.
- Face (4) + skin (3): procedural stylized textures/palettes, `source_tier: original`,
  scripted generator under `tools/art/`. Deterministic; no Meshy.
- Catalog `appearance.ardent.male` upgraded: preset lists point at the real assets (ids
  stable — same ints the server validates; content swap only).

## 3. Warden's Kit armor (Art scripted + Tools)

- 6 `item@2` pieces (`content/core/items/warden_*`), visible slots head/shoulders/chest/
  hands/legs/feet; each: skinned mesh 3–8k (§2.1, outfit ≤40k), `worn.models`,
  `worn.hides` (the geoset regions each piece covers), `worn.dye_channels`, budgets, icon.
- Hard-surface generators under `tools/art/` (deterministic, `source_tier: original`).
- Each piece ships an **RGB dye-mask** texture (R/G/B = primary/secondary/accent per ① §6).
- A `warden_set` reference the char-preview/E2E equips to show a full assembled outfit.

## 4. Dye completion (Client + Tools)

- **Codec** (`client/net`): decode the dye `channel` currently flattened away — the
  EntityEnter/equipment path yields `dyes: [{channel:int, dye_id:int}]` (additive; the
  wire always carried channel — T1/②). Failing clientnet test first.
- **Assembler** (`assembled_character.gd`): replace whole-piece albedo tint with a
  **mask-tint shader** — samples the piece's RGB dye mask, multiplies each channel region
  by its dye color, preserves unmasked albedo. Master-material-compatible (Art PRD §2.3
  ≤12 materials — this is a parameterized variant, not a new master). Unknown dye →
  authored colors (unchanged fallback).
- Server still sends dyes empty at M1 (#551 owns application); proven via verify-script
  fixtures (a warden piece + russet/slate mask-tinted) and the char-preview, not live wire.

## 5. Restyle workflow (Tools doc + gate)

Make "kitbash to style" (Art PRD §3.4) concrete — a documented per-asset procedure in
`tools/blender/README.md`: Meshy fetch (via `tools/meshy`) → Blender import → budget
cleanup + retopo-lite → geoset cut + rig-skin → export via `meridian_export` (E100–E105
gate) → CI I020–I023 → the reviewed PR flips `restyle_status: done`. The #525 quarantine
lint is the backstop: no ai-tier asset merges while `pending`.

## 6. Data flow

```
Meshy (key, gated on #525) ─► restyle ─► meridian_export (E-rules) ─► .glb + sidecar(restyle:done)
tools/art scripted gens ────► plate .glb + dye masks + face/skin textures (original tier)
        both ─► mcc emit (pack.data.json + staged art, drift-gated) ─► ContentDB
              ─► AssembledCharacter: real body + warden kit + mask-tint dyes + presets
catalog appearance.ardent.male ─► real preset lists (ids stable)
```

## 7. Error handling / testing

- Budgets enforced by the existing L070–L072 + the validator; restyle gated by E-rules +
  I-rules + #525. Real body must pass I020 (exact 63 joints) / I022 (8 geosets) / I023
  (LOD chain — real body ships authored LODs, no `single` exemption).
- Client: clientnet ctest for the channel decode; `assembled_character_verify.gd` gains
  mask-tint checks (a channel-masked piece → per-region color; unknown dye → authored).
- E2E / visual: the ② two-session run + a **lead GPU-render capture** of the ardent in the
  Warden's Kit with a dye applied — the definition-of-done evidence.
- Full suites green; golden + staged-pack determinism gates cover the new content.

## 8. Story decomposition (for the plan)

| # | Story | Sourcing | Key? | Depends |
|---|-------|----------|------|---------|
| S1 | Face/skin procedural textures + generator | original | no | — |
| S2 | Warden's Kit plate generators + 6 item@2 + dye masks | original | no | — |
| S3 | Dye mask-tint shader + codec channel decode | client | no | S2 (a masked piece to test) |
| S4 | Ardent body: Meshy → restyle → replace blockout | ai | **yes** | #525 merged |
| S5 | Hair set (4): Meshy → restyle | ai | **yes** | #525, S4 |
| S6 | Catalog upgrade + full-kit E2E/GPU proof | integration | no | S1–S5 |

S1–S3 dispatch immediately; S4–S5 gated on the rotated key + #525; S6 last.
