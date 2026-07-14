# Content Schema v1

Schemas for the YAML content database (`/content`) — the source of truth for all game data (Baseline TD-06). Authored by Forge and Codex, hand-editable, compiled by `mcc` into world-DB SQL for the server and `.pck` resource packs for the client. These schema files are the contract all three consumers share (Baseline §5.1–5.2).

Format: JSON Schema draft 2020-12, stored as YAML for readability. Shared definitions live in two files, both of whose `$defs` validators must merge into every type schema before use: [common.defs.yaml](common.defs.yaml) (general primitives) and [skeleton.defs.yaml](skeleton.defs.yaml) (`meridian/skeleton@1` — the character/geoset vocabulary for contract ①). The reference validator [tools/validate_content.py](../../tools/validate_content.py) and `mcc` both merge both before use.

## Identifiers

```
<namespace>:<type>.<name>
core:npc.kobold_miner
core:item.rusty_pickaxe
myzone:quest.welcome_to_emberfall     ← community pack
```

- `namespace` = the content pack that owns the entity (`[a-z][a-z0-9_]{1,31}`). `core` is reserved for first-party content. This is the mechanism behind community packs (TLS-08): every pack writes only inside its own namespace, and `mcc` maps namespaces to non-overlapping numeric ID bands in `idmap.lock`.
- `type` prefix (`npc.`, `item.`, `quest.`, `ability.`, `loot.`, `vendor.`, `spawn.`, `zone.`) makes every reference type-checkable by pattern — an `item:` field cannot accept an NPC ID.
- References inside a file may omit `<namespace>:`; they resolve to the file's own namespace. Cross-pack references must be fully qualified.
- **Asset IDs** (art/music/SFX/ambience — Baseline §5.3) use the same grammar with `art.`, `mus.`, `sfx.`, `amb.` prefixes: `core:art.char.kobold.miner`, `core:amb.zone01.forest_day` (`amb.` added per decision D-24). Content never references file paths. Every referenced asset ID must have an IF-8 sidecar (`meridian/asset@1`) — lint L020 (warn until first art drop, then error).
- **Numeric IDs never appear in YAML.** `mcc` assigns and persists them in `idmap.lock` (committed); renaming an entity's string ID is a breaking change caught by CI.

## File conventions

- One entity per file, named `<name>.<type>.yaml` (e.g. `kobold_miner.npc.yaml`). The suffix must match the declared `schema` type — lint L001.
- Every file starts with the envelope:
  ```yaml
  schema: meridian/npc@1     # type @ schema major version
  id: core:npc.kobold_miner
  ```
- Pack layout:
  ```
  content/<namespace>/
    pack.yaml            # pack manifest (meridian/pack@1)
    npcs/     *.npc.yaml
    items/    *.item.yaml
    abilities/*.ability.yaml
    quests/   *.quest.yaml
    loot/     *.loot.yaml
    vendors/  *.vendor.yaml
    spawns/   *.spawn.yaml   # Forge-authored (hand-editable)
    zones/    *.zone.yaml
    equip_types/ *.equip_type.yaml  # armor/weapon-type catalog (meridian/equip_type@1)
    talents/  *.talent.yaml         # talent catalog (meridian/talent@1)
    talent_trees/ *.talent_tree.yaml  # tiered talent trees (meridian/talent_tree@1)
    classes/  *.class.yaml          # playable classes (meridian/class@1)
    assets/   **/*.asset.yaml  # IF-8 sidecars (meridian/asset@1), one per asset ID
  ```
- Units are always suffixed: `_ms`, `_m` (meters), `_mps`, `_pct`, `_deg`, `_seconds`. Money is always integer **copper** (100c = 1s, 10 000c = 1g).

## Type schemas

| Schema | File | Notes |
|---|---|---|
| `meridian/pack@1` | [pack.schema.yaml](pack.schema.yaml) | Pack manifest: namespace, version, engine pin, deps, license |
| `meridian/npc@1` | [npc.schema.yaml](npc.schema.yaml) | NPCs & mobs: stats, AI, interaction, loot link (NPC-01/02, CMB-02) |
| `meridian/item@2` | [item.schema.yaml](item.schema.yaml) | ITM-01; @2 adds `visual.worn` (modular-gear render contract, contract ①); stat-budget lint vs item_level arrives with ITM-03 (M2) |
| `meridian/ability@1` | [ability.schema.yaml](ability.schema.yaml) | Spells/abilities incl. data-driven VFX/SFX hooks (CMB-01/04) |
| `meridian/quest@1` | [quest.schema.yaml](quest.schema.yaml) | kill/collect/deliver/explore objectives (QST-01); `script` reserved for QST-02 |
| `meridian/loot@1` | [loot.schema.yaml](loot.schema.yaml) | Independent entries + exclusive pick-groups (ITM-02) |
| `meridian/vendor@1` | [vendor.schema.yaml](vendor.schema.yaml) | ECO-01 |
| `meridian/spawn@1` | [spawn.schema.yaml](spawn.schema.yaml) | Spawn points/patrols; written by Forge (NPC-01) |
| `meridian/zone@1` | [zone.schema.yaml](zone.schema.yaml) | Zone manifest: level range, music, POIs; `chunk_manifest` reserved pending the A-08 chunk-format contract |
| `meridian/asset@1` | [asset.schema.yaml](asset.schema.yaml) | IF-8 asset-registry sidecar (A-12): source file, license/provenance (TD-09), art import hints, audio stream metadata |
| `meridian/equip_type@1` | [equip_type.schema.yaml](equip_type.schema.yaml) | Armor/weapon-type catalog (pack-contract spec §2.1): `category: armor\|weapon` distinguishes the armor materials (Cloth/Leather/Mail/Plate) from the weapon types (Two-Hand/One-Hand/Wand/Staff). Items reference one via `item.equip_type`; class proficiencies gate on it in sub-project 2 |
| `meridian/attribute@1` | [attribute.schema.yaml](attribute.schema.yaml) | Kernel-blessed base attribute vocabulary (pack-contract spec §2.2): `kind: primary\|derived` (seed: strength/agility/intellect/stamina + crit/haste/armor). Operators reference & tune these via the shared `common.defs.yaml#/$defs/attributeMods` (race/class) and ability `buff`/`debuff` effects; every such ref is L011-resolved. Kernel formulas consume them in sub-project 2 |
| `meridian/race@1` | [race.schema.yaml](race.schema.yaml) | A playable race (pack-contract spec §2.3): cosmetic — names a display identity and points at an `appearance` catalog (L011-resolved). Only mechanical hook is the optional `attribute_mods` tuning (zeroed/omitted in the Chibi theme). Kernel roster loading is sub-project 2 |
| `meridian/talent@1` | [talent.schema.yaml](talent.schema.yaml) | A talent (pack-contract spec §2.5): `grants` a bundle of active abilities (by id) and/or passive buff/debuff effects over an attribute id (reusing the ability effect-primitive palette). Rules-data; kernel consumption is sub-project 2 |
| `meridian/talent_tree@1` | [talent_tree.schema.yaml](talent_tree.schema.yaml) | A simple tiered row-unlock talent tree (pack-contract spec §2.5): `tiers[]` of `{required_points, talents:[<talent id>]}`. No arbitrary prerequisite graph in v1. A class references one via `class.talent_tree` (sub-project 2) |
| `meridian/class@1` | [class.schema.yaml](class.schema.yaml) | A playable class — the 7-field integrator (pack-contract spec §2.4): `abilities` (spellbook), `usable_armor_types`/`usable_weapon_types` (equip_type ids), `role` XOR `hybrid`, optional `attribute_mods`/`race_limits`/`talent_tree`. Every ref is L011-resolved; the equip_type category-match (armor/weapon) is a sub-project 2 semantic gate. Rules-data; kernel consumption is sub-project 2 |

Deferred to M2 (do not invent early): `statprofile` (class/level stat tables), `faction` (v1 uses a simple friendly/neutral/hostile enum on NPCs), `recipe` (ECO-02), `gossip` graphs (v1 uses a single `gossip_text`).

## Versioning

Schema major version is embedded in the envelope (`@1`). Additive optional fields are non-breaking (same major); renames/removals/semantic changes bump the major, and `mcc` must support reading N and N−1 during a migration window. Schema changes require client+server+tools sign-off (Baseline §5.1).

## Codex authoring annotations

Schemas may carry the Draft 2020-12 extension keywords `x-meridian-ui` and
`x-meridian-asset`. They are non-validating annotations: content validity,
runtime models, and `mcc` behavior never depend on them. `schema_gen` validates
their vocabulary and emits the checked-in Codex form-descriptor manifest.

`x-meridian-ui` supports:

| Key | Contract |
|---|---|
| `group` | Stable lower-snake-case section id local to the schema. |
| `label` | Non-empty human-facing field label. |
| `widget` | `single_line`, `multiline`, `slug`, `semver`, `number`, `asset_picker`, `reference_picker`, or `animation`. |
| `unit` | `ms`, `m`, `mps`, `percent`, `copper`, or `scale`. |
| `reference_type` | Typed target in `content:<type>` or `asset:<type>` form. |
| `help` | Optional task-oriented explanation beyond the schema description. |
| `example` | Optional scalar example; never a default. |
| `constraint` | Optional plain-language summary of a schema-owned constraint. |

`x-meridian-asset` is valid on asset-reference fields. `allowed_classes` is a
non-empty, unique subset of `asset.schema.yaml`'s `class` enum.
`eligible_generators` is a unique list of approved remote generators; v1 knows
only `meshy`. An omitted or empty list means selection only. Generator
eligibility is explicit and field-local: for example a creature model may be
Meshy-eligible while an icon, VFX, SFX, or canonical skeleton is not.

```yaml
model:
  $ref: "#/$defs/artRef"
  x-meridian-ui:
    group: presentation
    label: Creature model
    widget: asset_picker
    reference_type: asset:art
  x-meridian-asset:
    allowed_classes: [creature_model]
    eligible_generators: [meshy]
```
