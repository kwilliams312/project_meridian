# Design — Character / Equipment / Appearance Data Contract (①)

**Date:** 2026-07-09
**Track:** Art + Client + Tools (Server consulted: appearance record persistence + wire)
**Status:** Draft (design approved in brainstorming; pending spec review → implementation plan)
**Relates to:** CHR-01, ITM-01, A-03/D-32 ([01-SYNC-DECISIONS.md §7.2](../../01-SYNC-DECISIONS.md)),
Art PRD §2.1/§2.5/§4.2 ([art-prd.md](../../prd/art-prd.md)),
[item.schema.yaml](../../../schema/content/item.schema.yaml),
[npc.schema.yaml](../../../schema/content/npc.schema.yaml),
[asset.schema.yaml](../../../schema/content/asset.schema.yaml),
[world.fbs](../../../schema/net/world.fbs)

## 1. Overview

Define the **data contract** that lets a player character be assembled at runtime
from a race/sex body + modular worn equipment, per the architecture the Art PRD
already commits to: **one shared skeleton per race/sex; armor authored once and
skinned to that skeleton; weapons as static meshes on bone attachments; NPCs/mobs
as monolithic models** (art-prd §2.1 rows "Player character body", "Armor set
piece", "Weapon"; §4.2 "one skeleton per race/sex, shared by all gear").

This spec is **pure data/vocabulary** — schemas and enums. It is the foundation
consumed by four follow-on work packages, each with its own spec → plan cycle:

| # | Package | Consumes this contract as |
|---|---------|---------------------------|
| ② | Client character assembler (Godot) | runtime input: what to bind/attach/hide/tint |
| ③ | NPC forward-compat hook | reserved fields only (M1 keeps monolithic `visual.model`) |
| ④ | Authoring pipeline (rig `.blend`, export preset, import-validator checks) | the canonical bone/socket/geoset names to validate against |
| ⑤ | M1 preset content (bodies, hair/face/skin set, starter gear) | the schemas the content is authored in |

### Scope decision (brainstormed 2026-07-09)

**Forward-compatible schema, M1 content.** The schemas below support multi-race,
per-race mesh overrides, morphs, dyes, and NPC gear-reuse **additively**, but M1
authors only: 1 race/sex set, the customization set v1 (4 hair / 4 face / 3 skin,
art-prd §5), a starter gear set, and no morphs.

### Goals

- A single versioned **vocabulary file** (bones, attach sockets, geoset regions)
  that the rig, importer, and client all reference — no string drift.
- Extend `meridian/item@1` `visual` so worn armor/weapons carry everything the
  assembler needs: worn mesh(es), geoset hide-mask, attach socket, sparse
  per-race overrides, dye channels.
- Define the **appearance catalog** (content: what options exist per race/sex)
  and the **appearance record** (per-character: what was chosen) — the versioned,
  additive-only record A-03/D-32 mandates.
- Define **dyes** as a curated content palette + per-item-instance channel state.
- Reserve (not implement) the NPC path onto the same assembler.

### Non-goals

- The Godot assembler itself (②), the rig/export/validator work (④), and content
  authoring (⑤) — separate specs.
- Dye **acquisition/economy** (dye items, vendors, consumables) — ECO/ITM scope.
- Class-tint on gear — explicitly rejected (owner decision, 2026-07-09);
  `feat/class-player-color` remains a nameplate/UI concern, not a gear-material one.
- Deep morph/slider customization — deferred post-1.0 per A-03/D-32.
- Transmog, helmet-hide toggles, mount gear — post-M1 features; all additive on
  this contract.

## 2. Background — why this shape

Three archetypes exist for character+gear modeling:

- **A — Monolithic/baked:** each body+gear combo is one mesh. Right for NPCs/mobs
  (they never change gear); combinatorially impossible for players.
- **B — Single body + morph/stretch:** races as morphs/scale on one mesh. Rejected
  as the racial-shape mechanism by A-03/D-32 (presets-first; morphs capped at 1–2
  optional continuous knobs, budget-permitting) — per-character morph skinning
  cost is the binding constraint in 50+ player crowds (art-prd §2.5).
- **C — Shared skeleton + modular skinned gear:** each race/sex has its own body
  mesh on a **shared bone layout**; each gear piece is authored **once**, skinned
  to canonical bones, and deforms onto any race. The MMO standard; what the Art
  PRD commits to.

**Resolution of the "single model with stretch/transform vs. individual items per
race" question:** C gives both — one authored mesh per item (not per race), correct
on every race because the skeleton is shared. Per-race meshes survive only as a
**sparse escape hatch** (`race_overrides`) for pieces that genuinely cannot deform
cleanly (helmets over horns, boots on hooves).

Clipping between gear and body is handled by **geosets**: the body mesh is
pre-segmented into named regions; each worn piece declares which regions it hides.
Chosen granularity: **coarse, 8 regions** (owner decision, 2026-07-09) — simplest
to author; a region can later be **split** into finer children additively (old
hide-masks keep working by hiding all children of the named region).

## 3. Deliverable 1 — `schema/content/skeleton.defs.yaml` (`meridian/skeleton@1`)

One versioned defs file, `$ref`'d by the item and appearance schemas (same pattern
as `common.defs.yaml`), and the source of truth for validator (④) and assembler (②).

```yaml
schema: "meridian/skeleton@1"

geoset_regions:            # coarse ×8; refine by splitting (additive) only
  [head, hands, forearms, torso, waist, hips_legs, lower_legs, feet]

attach_sockets:            # bone-attachment mount points (weapons/shields/sheaths)
  [main_hand, off_hand, ranged, back, hip_l, hip_r, shield]

dye_channels:              # max 3 == RGB dye-mask texture channels (see §6)
  [primary, secondary, accent]

bones: []                  # canonical humanoid bone names, ≤120 incl. face
                           # (art-prd §2.1); POPULATED BY ④ from the reference rig
                           # (feat/blender-export-addon / feat/world-reference-geometry),
                           # not invented here. Empty list is valid for @1-draft;
                           # the import validator treats an empty list as "no bone
                           # name check yet".
```

Rules:

- **Additive evolution only** within `@1`: new sockets/regions/channels may be
  appended; none renamed or removed. A geoset region may gain `children:` entries
  when split; hide-masks naming the parent hide all children.
- The server never reads this file — it is visual-only vocabulary. Race/class ids
  remain the M0-frozen roster (`server/characters/src/roster.h`, `world.fbs`).
- Each race/sex skeleton asset (`asset.yaml`, class `character_model`) must
  contain every canonical bone with matching names — validated at import (④).

## 4. Deliverable 2 — `meridian/item@2`: the `visual.worn` block

`visual.icon` and `visual.model` are unchanged (`model` = inspect/ground/generic
mesh). A new optional `visual.worn` block carries the assembler contract; schema
`allOf` requires it for `item_class: armor|weapon` with an equip slot.

```yaml
visual:
  icon: art.ui.icon.armor.boots_plate_rusty        # unchanged, required
  model: art.armor.zone01.boots_plate_rusty.ground # unchanged, optional
  worn:
    models:                                # LIST from day one (M1 authors 1 entry);
      - model: art.armor.zone01.boots_plate_rusty  # multi-part/mirrored pieces additive
        mirror: none                       # none | x  (author one pauldron, mirror the pair)
    hides: [feet, lower_legs]              # geoset regions suppressed while worn
    attach:                                # weapons/shields ONLY (static mesh path)
      socket: main_hand                    # from skeleton.defs attach_sockets
      sheath_socket: hip_l                 # optional; where it rides when sheathed
    dye_channels: [primary, secondary]     # subset of skeleton.defs dye_channels;
                                           # ABSENT ⇒ undyeable
    race_overrides:                        # SPARSE escape hatch — not the norm
      dwarf:
        models:
          - { model: art.armor.zone01.boots_plate_rusty_dwarf, mirror: none }
```

Rules:

- **Skinned vs attached:** armor entries in `models` are skinned meshes bound to
  the shared skeleton; weapons use `attach` and ship static meshes (art-prd §2.1).
  A worn block has `attach` XOR is skinned — schema-enforced by `item_class`.
- `race_overrides` keys are roster race ids; an override replaces `models` (and
  may replace `hides`) for that race only. Everything absent falls through to the
  default. **M1 authors zero overrides.**
- Budget lints (L070+) apply per entry: armor 3–8k tris/slot, full 8-slot outfit
  ≤ 40k added; weapons 6–12k (15k 2-hander/legendary) — art-prd §2.1.
- The assembler (②) merges body + worn pieces to **≤ 10 surfaces** per character
  (art-prd §2.5); nothing in this contract may force a per-piece unique material —
  worn meshes parent to the Character master material with per-asset overrides.
- **Invisible slots are exempt:** `neck`, `finger`, `trinket`, `bag` never require
  (or allow) `worn` — jewelry has no worn mesh. `worn` is required for weapons and
  for armor in visible slots (head…feet, shield/off-hand).
- **Existing content migration:** bumping to `meridian/item@2` touches the five
  `content/core/items/*.item.yaml` files; only `rusty_pickaxe` (weapon) gains a
  `worn` block (`attach: main_hand`, placeholder model id). `brens_signet`
  (finger), the potion, the ear, and the charm are unaffected.

## 5. Deliverable 3 — appearance catalog + appearance record

Two artifacts: a content-side **catalog** (what exists) and a per-character
**record** (what was chosen). Catalogs are **per race/sex** (WoW-style).

### 5.1 Catalog — `schema/content/appearance.schema.yaml` (`meridian/appearance_catalog@1`)

One file per race/sex under `content/<ns>/appearance/`:

```yaml
schema: "meridian/appearance_catalog@1"
id: appearance.human.male
race: human                    # roster id
sex: male
skeleton: art.char.human.male.skeleton     # asset class character_model
body_model: art.char.human.male.base
presets:                       # customization set v1 (art-prd §5): 4 hair / 4 face / 3 skin
  hair:  [{ id: 1, model: art.char.human.male.hair_short }, ...]   # mesh presets
  face:  [{ id: 1, texture: art.char.human.male.face_a }, ...]     # texture presets
  skin:  [{ id: 1, palette: art.char.human.male.skin_pale }, ...]  # palette presets
morphs: []                     # 0 at M1; up to 2 entries {id, name, blendshape, min, max}
                               # ship ONLY if the §2.5 crowd budget allows (A-03/D-32)
```

- Preset ids are **small integers, stable forever** within a catalog; new presets
  append. This is what makes the record additive.
- `hair` entries are geoset-like meshes on the head region; `face`/`skin` are
  texture/palette layers on the Character master material. Exact material plumbing
  is ②/④ scope.

### 5.2 Record — persisted per character (server DB + wire)

```
appearance_version: 1          # bump only on structural change; unknown fields ignored
hair: u8  face: u8  skin: u8   # preset ids from the character's race/sex catalog
morphs: []                     # (id, value 0..255) pairs; empty at M1
```

- **Wire:** `world.fbs` char-create gains an `appearance` table alongside the
  existing `race`/`class`; char-list/enum returns it so the select screen renders
  the real character. Server validates preset ids against the catalog ranges it
  is told at content-load (or clamps — server treats appearance as opaque-but-
  bounded; it is never gameplay-authoritative data).
- **DB:** one versioned blob/row on the character (chardb), never interpreted by
  gameplay code. Additive fields ⇒ no migration (the A-03/D-32 promise).
- Equipped-gear visuals are **not** in the record — they derive from the equipment
  container server-side, as today.

## 6. Deliverable 4 — dyes (`meridian/dye@1`) + per-instance dye state

Owner decision 2026-07-09: **dyes yes, class-tint no.**

- **Dye definitions** are curated content, `content/<ns>/dyes/*.dye.yaml`:

  ```yaml
  schema: "meridian/dye@1"
  id: dye.core.russet
  name: "Russet"
  color: "#8a4b2d"             # sRGB albedo tint; curated palette, NOT free RGB,
  rarity: common               # so the painterly art direction stays coherent
  ```

- **Implementation contract:** each dyeable worn mesh ships one **RGB dye-mask
  texture** (R=primary, G=secondary, B=accent — hence max 3 channels); the
  Character master material multiplies masked albedo by the chosen dye colors.
  One extra mask map fits the ≤ 32 MB armor-set texture budget (art-prd §2.3) and
  adds **no** new master material (≤ 12 game-wide cap holds).
- **Per-instance state:** the chosen `{channel → dye id}` map lives on the **item
  instance** (DB + wire with the equipment payload), not on the item template and
  not in the appearance record — dye follows the boots, not the body. Absent map
  ⇒ item's authored default colors.
- Acquisition (dye items, vendors, application UX) is out of scope here; the
  contract only guarantees the state slot exists.

## 7. Deliverable 5 — NPC forward-compat hook (③, reserve only)

`meridian/npc@1` keeps `visual.model` (+ `scale`, `sound_set`) as the monolithic
M1 path — kobolds and Bren stay one baked mesh. Reserved for a future `@2`, not
implemented now:

```yaml
visual:
  # EITHER (today, unchanged):
  model: art.creature.zone01.kobold_miner
  # OR (reserved): assemble like a player —
  appearance: appearance.human.male      # catalog ref + preset choices
  worn_items: [item.core.guard_chest, item.core.guard_boots]
```

The only M1 action: schema comment marking `visual` as a future oneOf, so nobody
designs against "NPC visual is always one mesh".

## 8. Data flow

```
skeleton.defs.yaml ──┬─► import validator (④): bone/socket/geoset name checks
                     ├─► item.schema (visual.worn enums) ─► mcc ─► worlddb/pck
                     └─► appearance catalogs ─► mcc ─► client pck
char create (client) ─► CHAR_CREATE{race,class,appearance} ─► chardb (versioned record)
enter world ─► spawn payload {race, sex, appearance, equipped[{item, dyes}]}
            ─► client assembler (②): body(catalog) + presets + worn models
               − hidden geosets + weapon attachments + dye tint ─► merged ≤10 surfaces
```

## 9. Error handling / validation

- **mcc lints (④ wires them):** unknown geoset region / socket / dye channel;
  `worn` missing on equippable armor/weapon; `attach` on armor or `hides` on
  weapon; `race_overrides` key not in roster; dye-mask texture missing when
  `dye_channels` declared; preset id collisions in a catalog; budget lints per
  worn entry (existing L070+ path).
- **Client assembler (②):** unknown preset id in a record → catalog entry 1 +
  telemetry warning (never a crash — records may be newer than the client);
  missing worn model asset → hide the piece, log; unknown dye id → authored
  default colors.
- **Server:** validates appearance preset ids at char-create only (bounds), never
  reads visual data otherwise.

## 10. Testing

- **Schema tests (this spec's implementation):** JSON-schema fixtures — valid/
  invalid `visual.worn` (armor vs weapon shape, override sparsity), catalog files,
  dye files; `additionalProperties: false` everywhere; version-bump additivity
  test (a `@1` reader ignores appended fields).
- **mcc round-trip:** item with `worn` block compiles to worlddb/pck and back with
  no loss (extends the existing golden corpus pattern).
- **Wire:** FlatBuffers conformance for the new appearance table + dye map on the
  equipment payload (existing `golden-message-corpus` pattern).
- Renderer-level verification (does it *look* right) belongs to ②'s spec.

## 11. Open items handed to follow-on specs

| Item | Owner spec |
|------|-----------|
| Canonical bone list (populate `bones:`) | ④ (from reference rig) |
| Master-material dye plumbing (`.gdshader`) | ② + ④ |
| Surface-merge strategy meeting ≤10 surfaces | ② |
| Preset content + starter gear authoring | ⑤ |
| Dye acquisition/economy | ITM/ECO (M2) |
| NPC `visual@2` oneOf | ③ (post-M1) |
