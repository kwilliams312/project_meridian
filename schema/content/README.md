# Content Schema v1

Schemas for the YAML content database (`/content`) — the source of truth for all game data (Baseline TD-06). Authored by Forge and Codex, hand-editable, compiled by `mcc` into world-DB SQL for the server and `.pck` resource packs for the client. These schema files are the contract all three consumers share (Baseline §5.1–5.2).

Format: JSON Schema draft 2020-12, stored as YAML for readability. Shared definitions live in [common.defs.yaml](common.defs.yaml); validators must merge its `$defs` into each type schema before use (the reference validator [tools/validate_content.py](../../tools/validate_content.py) and `mcc` both do this).

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
    assets/   **/*.asset.yaml  # IF-8 sidecars (meridian/asset@1), one per asset ID
  ```
- Units are always suffixed: `_ms`, `_m` (meters), `_mps`, `_pct`, `_deg`, `_seconds`. Money is always integer **copper** (100c = 1s, 10 000c = 1g).

## Type schemas

| Schema | File | Notes |
|---|---|---|
| `meridian/pack@1` | [pack.schema.yaml](pack.schema.yaml) | Pack manifest: namespace, version, engine pin, deps, license |
| `meridian/npc@1` | [npc.schema.yaml](npc.schema.yaml) | NPCs & mobs: stats, AI, interaction, loot link (NPC-01/02, CMB-02) |
| `meridian/item@1` | [item.schema.yaml](item.schema.yaml) | ITM-01; stat-budget lint vs item_level arrives with ITM-03 (M2) |
| `meridian/ability@1` | [ability.schema.yaml](ability.schema.yaml) | Spells/abilities incl. data-driven VFX/SFX hooks (CMB-01/04) |
| `meridian/quest@1` | [quest.schema.yaml](quest.schema.yaml) | kill/collect/deliver/explore objectives (QST-01); `script` reserved for QST-02 |
| `meridian/loot@1` | [loot.schema.yaml](loot.schema.yaml) | Independent entries + exclusive pick-groups (ITM-02) |
| `meridian/vendor@1` | [vendor.schema.yaml](vendor.schema.yaml) | ECO-01 |
| `meridian/spawn@1` | [spawn.schema.yaml](spawn.schema.yaml) | Spawn points/patrols; written by Forge (NPC-01) |
| `meridian/zone@1` | [zone.schema.yaml](zone.schema.yaml) | Zone manifest: level range, music, POIs; `chunk_manifest` reserved pending the A-08 chunk-format contract |
| `meridian/asset@1` | [asset.schema.yaml](asset.schema.yaml) | IF-8 asset-registry sidecar (A-12): source file, license/provenance (TD-09), art import hints, audio stream metadata |

Deferred to M2 (do not invent early): `statprofile` (class/level stat tables), `faction` (v1 uses a simple friendly/neutral/hostile enum on NPCs), `recipe` (ECO-02), `talent` (CHR-04), `gossip` graphs (v1 uses a single `gossip_text`).

## Versioning

Schema major version is embedded in the envelope (`@1`). Additive optional fields are non-breaking (same major); renames/removals/semantic changes bump the major, and `mcc` must support reading N and N−1 during a migration window. Schema changes require client+server+tools sign-off (Baseline §5.1).
