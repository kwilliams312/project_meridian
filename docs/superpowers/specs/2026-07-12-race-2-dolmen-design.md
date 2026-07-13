# Design — Race #2 (Dolmen): The Model-Per-Race Proof

**Date:** 2026-07-12
**Track:** Art + Tools + Client (Server: roster only)
**Status:** Draft (design; pending review → plan)
**Relates to:** ① #451, ④ #507, ② #542, ⑤ #574, Warden's Kit v2 #594; CHR-01;
Baseline §3 (M2: 2 races); `server/characters/src/roster.h` (Dolmen = race id 2, frozen);
`tools/blender/meridian_rig/bones.py` (`VALID_PROFILES`), `restyle_body.py`, `restyle_armor.py`.

## 1. Overview

Every prior epic built the character *system*; this one **proves its central thesis**.
The whole arc chose "a model per race on a shared skeleton, gear authored once" over
"one stretched mesh" or "per-race gear duplication." M1 shipped that with a single race
(Ardent). Race #2 (Dolmen — "mountain/stone folk," roster id 2) is the first time the
design is tested under a **second, differently-proportioned body**.

The deliverable is a playable Dolmen that:
1. Renders as its own real body on the **same 63-bone skeleton** (different proportions).
2. Has its own customization catalog (hair/face/skin presets).
3. **Wears the existing Ardent-authored Warden's Kit and sword with NO re-authoring** —
   the gear deforms onto Dolmen's proportions by bone-name binding.

That third point is the proof. If it holds, "model per race" is validated and race 3/4
are pure pipeline reuse. If a piece fails, it becomes the **first real use of item@2
`race_overrides`** (the sparse escape hatch designed in ① and never yet exercised).

### Goals
- Dolmen body + skeleton + catalog, playable (roster already reserves id 2).
- Validate authored-once gear across a second proportion profile — the architectural test.
- Exercise (only if needed) `race_overrides` for any piece that doesn't deform acceptably.

### Non-goals
- Races 3/4 (Sylvane/Emberkin) — this proves the pattern; they follow later, pure reuse.
- Dolmen-specific *armor* (the point is Ardent's kit deforms; new Dolmen gear is future).
- Female bodies (M1 is male-only; the per-sex axis is reserved, not built here).
- New assembler/contract logic — this is content + one proportion profile; the systems exist.

## 2. Dolmen proportions (the crux)

Dolmen is "mountain/stone folk" — the design intent is **shorter and broader/stockier**
than Ardent (heavier build, thicker limbs, lower height). This is expressed as a **new
proportion profile**, NOT a new skeleton topology:

- `bones.py`: add `"dolmen_male"` to `VALID_PROFILES` and a `for_profile("dolmen_male")`
  bone table — the **same 56 profile bones + 7 sockets (identical names/hierarchy)**, with
  different rest head/tail coordinates (e.g. ~0.88× height, wider shoulders/hips, shorter
  legs). The bone NAMES are the contract; only the rest transforms change. This is the
  literal test of ④'s "proportions are parameters" claim.
- Because the skeleton bone names are identical, gear skinned to those names (all of the
  Warden's Kit, authored on Ardent) binds to Dolmen's skeleton and follows Dolmen's rest
  proportions automatically — the shared-skeleton mechanism doing its job.

## 3. Deliverables

### D1 — Dolmen skeleton
`generate_rig.py --profile dolmen_male` → `art.char.dolmen.male.skeleton` (.glb + sidecar,
class `character_model`, `source_tier: original` — scripted, same as Ardent). 63 canonical
bones, Dolmen rest pose. Passes I020 (exact 63 joints). Staged.

### D2 — Dolmen body
Meshy → `restyle_body.py` fit to the **Dolmen** skeleton region bounds → 8 `geo_<region>_lod<N>`
geosets → skinned to the 63 bones → LOD chain. `art.char.dolmen.male.base`, `source_tier: ai`
+ full provenance + `restyle_status: done`. Passes I020/I022/I023. Staged. The body reads as
a stockier stone-folk humanoid.

### D3 — Dolmen catalog + roster wiring
- `appearance.dolmen.male` catalog (per race/sex) — `skeleton` + `body_model` = D1/D2,
  presets (hair/face/skin). M1-lean: reuse or lightly re-tint the ⑤ Ardent presets, or
  source a small Dolmen-appropriate set (beards/stone-tones) — decide at plan time; presets
  are cheap and additive.
- Roster: Dolmen id 2 is frozen and already in `roster.h` + client `MeridianRoster.RACES`.
  Confirm char-create can select Dolmen and the server accepts race=2 (validation already
  allows it — it's a frozen id). No wire change (race travels as uint8; appearance record is
  race-agnostic ints).

### D4 — The fit-check (the proof) + race_overrides only if needed
- Assemble **Dolmen + full Warden's Kit + hair + dye** and GPU-render (lead). The gear was
  authored on Ardent; on Dolmen it must deform via bone-name binding without gross clipping
  or voids. Also render Dolmen + the #605 sword (socket attach — the socket bones scale with
  the profile, so the sword rides the Dolmen hand).
- **Expected:** authored-once works (skinned gear follows the skeleton; some looseness/tightness
  from proportion mismatch is acceptable greybox-plus). This is the win condition.
- **If a piece fails** (e.g. Dolmen's broader torso clips the cuirass badly): author a sparse
  `race_overrides.dolmen` on that item@2 file (a Dolmen-fitted model variant) — the ① escape
  hatch, first real use. The assembler already resolves `race_overrides` by race name; D4
  proves that path end-to-end. Track each override as a note, not a silent copy.

## 4. Data flow

```
bones.py dolmen_male profile ─► generate_rig ─► dolmen skeleton
Meshy ─► restyle_body(dolmen) ─► dolmen body (8 geosets, 63-bone skin)
appearance.dolmen.male ─► catalog ─► ContentDB
assemble(race=2, sex=0, appearance, equipment) ─► Dolmen body
   + Warden's Kit (Ardent-authored) bound by bone name ─► deforms onto Dolmen
   + sword on main_hand socket (scales with profile)
   + race_overrides.dolmen ONLY for any piece that failed the fit-check
```

## 5. Error handling / testing
- Skeleton/body pass I020/I022/I023; catalog passes schema + L024 (ai body restyled).
- `bones.py` dolmen profile: drift-guard + a test that both profiles produce identical bone
  NAMES/hierarchy (only transforms differ) — the shared-skeleton invariant.
- Determinism/golden/staged gates as always; check_staged_models covers Dolmen assets.
- **Definition of done:** lead GPU render — Dolmen in the Ardent-authored Warden's Kit + sword,
  reading as a coherent armored stone-folk warrior. That render IS the model-per-race proof.

## 6. Story decomposition (for the plan)
| # | Story | Sourcing | Key? | Depends |
|---|-------|----------|------|---------|
| D1 | Dolmen skeleton (bones.py dolmen_male profile + generate_rig) | original | no | — |
| D2 | Dolmen body (Meshy → restyle to Dolmen skeleton) | ai | **yes** | D1 |
| D3 | Dolmen catalog + roster/create-UI confirm | mixed | maybe | D2 |
| D4 | Fit-check render + race_overrides for any failing piece + close | integration | no | D1–D3, Warden's Kit + #605 sword merged |

D1 first (unblocks all); D2 after D1; D3 after D2; D4 last (the proof + any overrides).
