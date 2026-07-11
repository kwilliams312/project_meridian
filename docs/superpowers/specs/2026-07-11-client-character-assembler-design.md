# Design — Client Character Assembler (②): Per-Race Bodies, Worn Gear, In-World Slice

**Date:** 2026-07-11
**Track:** Client + Server (wire) + Tools (consulted: pck content access)
**Status:** Draft (design approved in brainstorming; pending spec review → implementation plan)
**Relates to:** contract ① ([2026-07-09-character-equipment-data-contract-design.md](2026-07-09-character-equipment-data-contract-design.md), epic #451),
pipeline ④ ([2026-07-10-character-authoring-pipeline-design.md](2026-07-10-character-authoring-pipeline-design.md), epic #507),
CHR-01, ITM-01, issue #477 (absorbed), art-prd §2.5 (crowd rules — explicitly deferred),
[world.fbs](../../../schema/net/world.fbs), [world.gd](../../../client/project/scenes/world/world.gd),
[item.schema.yaml](../../../schema/content/item.schema.yaml), [appearance.schema.yaml](../../../schema/content/appearance.schema.yaml)

## 1. Overview

Contracts ① and ④ made character data and conforming assets real; nothing yet
*renders* them. Players and remotes are class-colored capsules built inline in
`world.gd`; char-select shows a shared placeholder model with hardcoded pickers;
`ENTITY_ENTER` carries no appearance or equipment.

This spec is the **vertical slice** that puts an assembled character on screen:

1. **Wire + server:** `EntityEnter` gains additive `appearance` + `equipment`
   visual fields; `worldd` populates them from state it already holds.
2. **Client content access:** appearance catalogs + item `worn` data + art
   assets readable from the mcc pck; char-create pickers become catalog-driven
   (absorbs #477).
3. **`AssembledCharacter`:** one GDScript node that turns
   `(race, sex, appearance, equipment)` into a rigged visual — per-race body
   model (owner decision 2026-07-11: **a model per race**, never a stretched
   universal mesh), skinned gear bound by bone name, socketed weapons, geoset
   hides, dye tints. Used by BOTH the world scene and char-select preview.
4. **Fallback discipline:** content problems degrade to the current capsule or
   contract-mandated defaults — never a crash.

Proof: two clients in-world see each other's blockout-bodied, pickaxe-wielding
ardent; char-select preview drives off the real catalog.

### Goals

- Replace capsules with assembled characters for the local player and remotes.
- One assembly code path for world and char-select.
- Only ids travel on the wire; all visual lookup is client-side pck content.
- Contract ① error rules implemented verbatim (§9 of that spec).

### Non-goals (deferred, with owners)

- Crowd budgets: ≤10-surface merge, 30 m/60 m LOD + anim-rate throttling
  (art-prd §2.5) — follow-on perf story once real assets exist. The
  `AssembledCharacter` API hides merge strategy so this lands without caller churn.
- Animations/AnimationTree — follow-on (④ handoff already tracks it); the M1
  slice may be static-posed.
- Real bodies/hair/faces/gear content — spec ⑤. This slice proves with the
  blockout body + rusty pickaxe.
- NPC/creature visuals — unchanged (monolithic `visual.model`); the ③ hook stays reserved.
- Dye acquisition/UI; transmog; helmet-hide. Additive later.

## 2. Wire + server (`schema/net/world.fbs`, `worldd`)

- Additive fields appended to the `EntityEnter` payload (exact table per the
  current fbs — never renumber):

```fbs
table DyeChoice { channel:uint8; dye_id:uint32; }          // channel: 0=primary 1=secondary 2=accent
table EquippedVisual {
  slot:uint8;              // equipment slot id (server's existing slot enum)
  item_template:uint32;    // IF-9 world id; 0 never sent (absent = slot empty)
  dyes:[DyeChoice];        // per-instance dye state (contract ① §6); empty = authored colors
}
// EntityEnter (player entities) gains, appended:
//   appearance:Appearance;            (existing table from contract ① T5)
//   equipment:[EquippedVisual];       (visible slots only)
//   race:uint8; sex:uint8;            (if not already present on the entity payload — verify; race may already travel)
```

- `worldd` populates from the character row (appearance JSON, race) and the
  equipment container (item template ids + `item_instance.dyes`) at
  EntityEnter build time. Sex: the appearance catalogs are per race/sex, but
  the character row has no sex column yet (M1 ships male only) — the record's
  catalog is resolved as `appearance.<race>.male` for M1 and the wire field
  reserves `sex` for the additive future (0 = male).
- Golden message corpus gains EntityEnter-with-appearance/equipment cases.
- NPC/creature EntityEnter payloads are untouched.

## 3. Client content access (absorbs #477)

- The client must resolve, from mcc-built pck content: (a) the appearance
  catalog for a race/sex; (b) an item's `visual.worn` block by template id;
  (c) dye definitions; (d) art assets by asset id (`res://art/...` layout,
  art-prd §3.3).
- Mechanism: a `MeridianContentDB` GDScript singleton wrapping whatever the
  existing pack-mount path provides (`pack.contents.jsonl`-derived data in the
  pck — the plan verifies the exact artifact and, if item worn data is not yet
  in a client-readable form, adds the minimal mcc emit needed; Tools track
  consulted). API:
  `catalog(race, sex) -> Dictionary`, `worn(item_template_id) -> Dictionary`,
  `dye(dye_id) -> Color`, `model_path(asset_id) -> String`.
- Char-create pickers (hair/face/skin) populate from `catalog()` preset lists
  instead of hardcoded counts — appearance ids remain the stable ints the
  server already validates.

## 4. `AssembledCharacter` (client/project/characters/)

- `class_name AssembledCharacter extends Node3D`; API:

```gdscript
func assemble(race: int, sex: int, appearance: Dictionary, equipment: Array) -> void
func set_equipment_slot(slot: int, item_template: int, dyes: Array) -> void  # incremental re-equip
func clear() -> void
```

- Internals (hidden behind the API): load body scene for the race/sex catalog's
  `body_model`; single `Skeleton3D`; skinned gear `MeshInstance3D`s re-parented
  onto it binding by canonical bone names; weapons via `BoneAttachment3D` on
  `socket_*` bones (`worn.attach.socket`); geoset hides = `visible = false` on
  `geo_<region>_lod*` nodes named in any equipped item's `worn.hides` union;
  hair preset = mesh on the head; face/skin presets = material params (blockout
  has no textures — machinery lands, real content in ⑤); dyes = material
  params on gear surfaces (mask texture arrives with real assets; M1 applies
  albedo tint to the piece).
- `race_overrides` from item@2 resolve here: if the item has an override for
  this race, its models replace the defaults (M1 content has none — code path
  tested with a fixture).
- World scene: `_spawn_remote` and the local-player body instantiate
  `AssembledCharacter` when EntityEnter carries appearance; class-color capsule
  remains the fallback body (see §6) and the nameplate/HUD wiring is untouched.
- Char-select: the preview pane instantiates the same node, driven by the
  picker state; roster entries re-render on selection.

## 5. Data flow

```
chardb (appearance JSON, equipment+dyes) ─► worldd EntityEnter{appearance, equipment[]}
   ─► client world.gd ─► AssembledCharacter.assemble(...)
        ├─ MeridianContentDB.catalog(race,sex) ─► body scene + preset meshes
        ├─ MeridianContentDB.worn(item) ─► models/hides/attach/dye_channels (+race_overrides)
        └─ MeridianContentDB.dye(id) ─► tint
char-select pickers ─► same assemble(...) on the preview node (no wire)
```

## 6. Error handling (contract ① §9, implemented verbatim)

- Missing/unloadable body model → capsule fallback (current class-colored body),
  one telemetry warning per asset id per session.
- Unknown preset id in a record → catalog entry 1 + telemetry warning.
- Missing worn model for an equipped item → hide the piece, log.
- Unknown dye id → authored default colors.
- Entity payload without appearance (NPCs, old servers) → capsule/monolithic
  path exactly as today — the fields are additive and optional.
- Content DB miss (no catalog for race/sex) → capsule fallback; char-create
  pickers disable with a visible "content missing" state rather than empty lists.

## 7. Testing

- **Headless verify scripts** (existing client pattern, `*_verify.gd`):
  assembler unit checks — body instanced with all 8 geosets visible; equip
  boots → `geo_feet_lod0` hidden; unequip → restored; weapon lands on
  `socket_main_hand` attachment; unknown-preset/dye/model fallback paths;
  race_overrides fixture; incremental `set_equipment_slot`.
- **Wire:** FlatBuffers conformance corpus for the new EntityEnter fields;
  worldd unit/DB tests for payload population (appearance + equipment + dyes).
- **E2E (the slice's definition of done):** two-bot/two-client run on the local
  stack — each client renders the other as an assembled blockout ardent with
  the pickaxe socketed; char-select preview renders from catalog data.
  Screenshot evidence via the client's existing headless/boot-smoke harness
  where available.
- Full existing suites stay green (server ctest DB-backed, pytest, mcc).

## 8. Open items handed to follow-on work

| Item | Owner |
|------|-------|
| Crowd budgets: surface merge, LOD/anim throttling (art-prd §2.5) | perf story, post-⑤ |
| Animations + retarget wiring | follow-on package (④ handoff) |
| Real preset/gear content; dye mask textures; face/skin materials | spec ⑤ |
| Character `sex` column + catalogs beyond male | M2 (additive wire field reserved) |
| NPC assemble-path (③ hook) | post-M1 |
