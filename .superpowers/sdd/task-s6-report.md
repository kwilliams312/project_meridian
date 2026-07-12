# Story S6 (#573) — catalog upgrade + full-kit E2E/GPU proof — implementer report

**Status:** COMPLETE — PR open, all CI green, awaiting lead engine/GPU verify + merge.
**Branch:** `feat/content-s6-finale` (from `origin/dev`)
**Commit:** `49d03fd`
**PR:** #588 (`--base dev`, `Closes #573`, `Part of epic #574`) — NOT merged.
**Follow-up filed:** #587 (arms-seam root cause — body geoset re-cut, S4 territory)

## Work delivered (priority order)

### 1. Catalog upgrade (core deliverable) — DONE
`content/core/appearance/ardent_male.appearance.yaml`: every preset swapped off the
blockout-base placeholder onto real spec-⑤ assets.
- hair: presets 1-3 → `hair_1..3`; **append-only preset id 4** → `hair_4` (4 real S5 meshes)
- face: presets 1-3 → `face_1..3`; **append-only preset id 4** → `face_4` (4 S1 textures)
- skin: presets 1-3 → `skin_1..3` (3 S1 palettes); `body_model` already real S4 base
- Preset ids 1..3 unchanged (the ints the server validates); id 4 is append-only —
  `AppearanceRecord::normalise` only clamps 0→1 and has no upper-bound/count check, so
  appending is safe. `char_select_verify.gd` sizes the pickers dynamically (`.size()`),
  no hardcoded count anywhere in server/client C++.

### 2. Plate appearance — kill the grey boxes — DONE
`tools/art/generate_warden_kit.py`. Root causes of the muddy/stripe dye were: (a) mid-grey
base albedo, (b) high metallic swallowing albedo, (c) **plates had NO UVs** so the mask
sampled one texel.
- Light brushed-steel base albedo `(0.74,0.76,0.80)` + metallic `0.15` → dyes read true.
- Added per-face planar UVs (`TEXCOORD_0`) → mask maps across the full surface.
- Full-coverage mask: dominant primary + secondary trim + accent piping, no neutral gaps.
- Regenerated 6 plates + 6 masks (byte-deterministic, verified two clean temp dirs), re-staged
  + golden updated in lockstep.

### 3. Full-kit fixture + verify — DONE (lead runs the engine)
`client/project/characters/assembled_character_verify.gd` new `_verify_full_kit` phase:
assembles ardent + hair preset + all 6 Warden's Kit slots + a chest dye; asserts all slots
mount, hide union leaves only forearms+waist visible (2/8 geosets), chest dye ShaderMaterial
survives in the composite, hair mesh seated, shoulders bridges an UpperArm bone.
GDScript follows epic rules (no `:=` on Variant; typed vars).

### 4. Floating-arms hide seam — FIXED for full-kit (option A) + root-cause SPUN OFF
- Root cause: upper arm (`RightUpperArm`/`LeftUpperArm`) lives in the `torso` geoset;
  `forearms` geoset is elbow→wrist only. Hiding torso orphans the forearms.
- Least-invasive fix shipped: shoulders plate gains an upper-arm guard shell skinned to the
  UpperArm bones (`_UPPERARM_R`), so the **full kit** bridges the seam (the S6 DoD render).
- Does NOT fix a torso-hiding chest worn without shoulders. Proper cure is a body geoset
  re-cut (S4 territory) — filed as **#587** with 3 options documented; noted in the PR.
  Did not block priorities 1-3.

## Verification (fresh, all green)
- `uv run pytest -q` → **470 passed** (12 new tests: `TestPlateAppearanceS6`, bridge, mask coverage)
- `uv run tools/validate_content.py` → OK (catalog + plates, all refs resolve)
- `uv run tools/validate_imports.py` → OK
- `uv run tools/check_staged_models.py` → OK
- `scripts/check-golden.sh` → OK (determinism + golden + staged pack byte-identical)
- `./tools/mcc/build/mcc check ./content` → OK
- `ruff check` (changed files) → clean
- **`gh pr checks 588` → ALL PASS** (account, authd, characters, client-cores, client-gdext,
  content-build, db, mcc, mcc-parity, server, strudel-render, validate, worldd-session;
  client-boot-smoke/client-export skipped by path filter)

Outfit budget after bridge shells: 30,312 tris (≤40k); shoulders 6,456 (3-8k band).

## Lead-run gates (implementer sandbox has no Godot/GPU)
```
godot --headless --path client/project --script res://characters/assembled_character_verify.gd
```
Then the DoD GPU render: ardent in dyed full Warden's Kit + hair, confirm no grey boxes and no
floating arms, plus the two-session E2E.
