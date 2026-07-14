# World DB DDL v1 (IF-4)

Hand-maintained SQL DDL for the **world database** — the read-only, nightly-rebuilt
artifact that `mcc` fills and `worldd` loads into immutable in-memory template
stores. This directory *plus* [tools SAD §2.6](../../../docs/sad/tools-sad.md) and
[server SAD §4.3](../../../docs/sad/server-sad.md) is the IF-4 contract.

**Ownership:** Tools track owns that `mcc emit-sql` fills these tables; the DDL is
**co-reviewed with the Server track** (they own the daemon that reads it). `mcc`
embeds this DDL at build time and emits it verbatim — one source of truth
(tools SAD §2.6). The server never writes this DB; it is replaced wholesale by the
content build.

## Dialect

- **MariaDB / MySQL**, `ENGINE=InnoDB`, `DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci`
  on every table, explicitly (worldd's DB, baseline TD-05).
- Charset/collation are declared per-table so the DDL is portable regardless of
  server defaults.

## Key scheme (IF-9)

- Every numeric primary key is a **uint32 assigned by IF-9** (`idmap.lock`), not by
  the database. All PKs are therefore **`INT UNSIGNED PRIMARY KEY` with NO
  `AUTO_INCREMENT`** — `mcc` bakes the numeric id into every row and every reference
  column (server SAD §4.6; DML dump relies on no `AUTO_INCREMENT`, tools SAD §2.6).
- **String content IDs (`core:npc.kobold_miner`) are never stored as keys.** They
  live only in `idmap.lock`. Content refs (`npcRef`, `itemRef`, …) become
  `INT UNSIGNED` numeric ref columns (`*_id` / `*_ref_id`).
- **Asset refs** (`artRef`, `musRef`, `sfxRef`, `ambRef`) are client-facing (they
  resolve in the `.pck`, not on the server). They are kept as numeric `*_id`
  columns so the three-way content-hash tie stays intact, but carry no world-DB FK
  (the referenced asset table is not in this DB).

## Column-mapping rules (content schema → DDL)

The DDL columns **mirror the content-schema field sets** in
[`schema/content/*.schema.yaml`](../../content/). Conventions:

| Content schema construct | DDL representation |
|---|---|
| `money` (copper) | `BIGINT UNSIGNED` (integer copper; 100c = 1s, 10000c = 1g) |
| `intRange` (`{min,max}`) | two columns `<field>_min` / `<field>_max` |
| `position` (`{x,y,z}`) | `pos_x` / `pos_y` / `pos_z` (FLOAT, zone-local meters, D-20) |
| enum | `ENUM(...)` (short, closed sets) |
| content ref (`*Ref`) | `INT UNSIGNED` numeric ref (`*_id`), FK within world DB where sensible |
| asset ref (`art/mus/sfx/amb`) | `INT UNSIGNED` numeric `*_id`, **no FK** (asset table not in this DB) |
| nested scalar object (`stats`, `ai`, `cast`, `weapon`, `price`, `visual`) | flattened with a prefix (`stat_*`, `ai_*`, `cast_*`, `weapon_*`, `price_*`, `visual_*`) |
| array of objects | child table keyed `(parent_id, ordinal)` (deterministic order for the DML dump) |
| `oneOf` variant array (`effects`, `objectives`, loot entries) | one child table with a `kind`/`type` discriminator + the union of variant columns |

Money is modeled as `BIGINT` copper (game currency is integer copper), **not**
`DECIMAL` — this is a deliberate deviation from the generic "money → DECIMAL"
backend guideline, because the content schema defines `money` as integer copper.

## File layout

DDL is split for readability; `mcc` concatenates them in filename order (the numeric
prefixes are the load/declaration order the DML dump follows, tools SAD §2.6).

| File | Table families | Content schema |
|---|---|---|
| `00_manifest.sql` | `world_manifest` + session/charset preamble; `SET FOREIGN_KEY_CHECKS=0` | tools SAD §2.6, server SAD §4.3 |
| `10_npc.sql` | `npc_template`, `npc_ability` | `npc.schema.yaml` |
| `20_item.sql` | `item_template`, `item_stat`, `item_effect_on_equip` | `item.schema.yaml` |
| `30_ability.sql` | `ability` (effects[] as generic `effects_json`, SP2.1) | `ability.schema.yaml` |
| `35_roster.sql` | `race`, `class` (playable roster, keyed by `roster_id`, SP2.5) | `race.schema.yaml`, `class.schema.yaml` |
| `36_attribute.sql` | `attribute`, `class_attribute_mod`, `race_attribute_mod` (attribute framework, SP2.4) | `attribute.schema.yaml` (+ `$defs/attributeMods`) |
| `40_quest.sql` | `quest_template`, `quest_objective`, `quest_prereq`, `quest_reward` | `quest.schema.yaml` |
| `50_loot.sql` | `loot_table`, `loot_group`, `loot_entry` | `loot.schema.yaml` |
| `60_vendor.sql` | `vendor_inventory`, `vendor_inventory_item`, `vendor_inventory_buys` | `vendor.schema.yaml` |
| `70_spawn.sql` | `spawn_point`, `patrol_path`, `patrol_waypoint` | `spawn.schema.yaml` |
| `80_zone.sql` | `zone`, `area`, `graveyard`, `area_trigger`, `navmesh_ref` | `zone.schema.yaml` (+ reserved) |
| `90_gossip.sql` | `gossip`; commented `class_level_stats` stub (M2); restores `FOREIGN_KEY_CHECKS=1` | `npc.schema.yaml` (`interaction.gossip_text`) |

### Load ordering & foreign keys

Foreign keys are declared **within the world DB** where they add integrity value.
Because tables have forward references (e.g. `npc_template.vendor_ref_id →
vendor_inventory`, declared in file 10 but created in file 60), `00_manifest.sql`
sets `FOREIGN_KEY_CHECKS = 0` for the bulk create and `90_gossip.sql` restores it
to `1`. References into the **characters DB** (item instances, character quests) are
**numeric-only with no cross-DB FK** — the world DB never points at character state
(server SAD §4.4).

### Reserved / not-yet-schema-driven tables

- **`area_trigger`, `navmesh_ref`** (in `80_zone.sql`) — named server-SAD §4.3
  families that have **no content-schema field set in v1**. `zone.chunk_manifest`
  is `RESERVED` (type `null`) until the IF-6 chunk-format contract is signed
  (A-08). Declared now so `worldd`'s loader and the manifest `schema_version` are
  stable; `mcc` leaves them empty until the geometry/trigger schemas land.
- **`class_level_stats`** — M2 family; commented-out stub in `90_gossip.sql`. No
  content schema exists (stat profiles arrive M2 per `npc.schema.yaml`).

### Intentional omissions (content field → not a column)

| Field | Why it is not a column |
|---|---|
| `schema` (every file) | Envelope discriminator; `mcc` knows the type — not stored. |
| `id` (string, every file) | Replaced by the IF-9 **numeric** `id` PK; string id lives in `idmap.lock`. |
| `quest.script` | Reserved M2 (`type: null`); validators reject non-null until QST-02. |
| `zone.chunk_manifest` | Reserved (A-08, `type: null`); Forge geometry, never reaches the server directly → surfaces later as `navmesh_ref`. |

## Validation

**MariaDB is not installed in the authoring environment**, so this DDL's *execution*
is not claimed here — a future CI MariaDB-service job runs the DDL against a live
server. What is validated in-repo:

1. **Field → column coverage.** A throwaway checker loads every
   `schema/content/*.schema.yaml`, recursively extracts its declared leaf property
   names (flattening nested objects and `intRange`/`position`/array constructs the
   same way the DDL does), and asserts each maps to a column in the corresponding
   DDL file. **Result: all 8 content schemas fully covered**, with the intentional
   omissions above explicitly listed. This coverage report is the verification
   evidence for IF-4 v1.
2. **Syntax self-review.** MariaDB-specific constructs a CI MariaDB-service job will
   validate: `ENUM(...)`, `BOOLEAN` (alias for `TINYINT(1)`), `INT UNSIGNED`
   PKs without `AUTO_INCREMENT`, composite-key foreign keys
   (`loot_entry → loot_group (loot_table_id, ordinal)`), the `JSON` column type
   (`ability.effects_json`, LONGTEXT + a `JSON_VALID` CHECK), and the
   forward-reference create order guarded by `FOREIGN_KEY_CHECKS`.

## References

- [tools SAD §2.6 — `emit-sql` / IF-4 contract](../../../docs/sad/tools-sad.md)
- [server SAD §4.3 — world DB table families + boot handshake](../../../docs/sad/server-sad.md)
- [server SAD §4.4 — cross-DB reference rule](../../../docs/sad/server-sad.md)
- [server SAD §4.6 — IF-9 numeric ID consumption](../../../docs/sad/server-sad.md)
- [content schemas](../../content/) — the field-set source of truth
