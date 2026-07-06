# characters DB

DDL for the **characters DB** — the **only durable player state** in the system
(server SAD §4.2). Like the auth DB (`../auth/`), it is **owned and mutated by
the server at runtime** — characters are created, inventory and quests change
constantly, auras persist on logout — and so it evolves over time and ships as
**reversible, numbered migrations** rather than a static DDL dump. It is the
opposite of the world DB (`schema/sql/world/`), which is an `mcc`-produced,
read-only artifact replaced wholesale each night.

| DB | Owner | This tree covers |
|----|-------|------------------|
| `characters` | shard workers + `servicesd` (character persistence, economy, social) | `characters/migrations/` — GitHub issue #191 |

> Scope of this deliverable (#191): the **M0/M1 base** of the characters DB —
> `character`, `item_instance`, `character_inventory`, `character_quest`,
> `character_spell`, `character_talent`, `character_aura`. The M2+ tables
> (`mail`/`mail_item`, `auction`, `instance_bind`/`instance_state`,
> `guild`/`guild_member`/`guild_rank`, `character_social`) are **stubs** — a
> commented roadmap note in `0001_init_characters.up.sql`; each lands in its own
> later migration at its milestone. The auth DB lives at `../auth/`; the world
> DB is **not** here — it lives at `schema/sql/world/` because Tools owns it
> (IF-4).

Authoritative field list: [`docs/sad/server-sad.md` §4.2](../../../docs/sad/server-sad.md)
(characters DB tables), §4.4 (cross-DB reference rule), §4.7 (`save_epoch`
ownership fence). This DDL is **co-reviewed with the Server track**.

## Directory layout

```
server/db/
  README.md                                <- server-managed DBs overview
  auth/          ...                        <- auth DB (issue #76)
  characters/
    README.md                              <- this file
    migrations/
      0001_init_characters.up.sql          <- character, item_instance,
                                              character_inventory, character_quest,
                                              character_spell, character_talent,
                                              character_aura
      0001_init_characters.down.sql        <- drops them (reverse order)
```

## Migration convention

Identical to the auth DB (`../auth/` and `../README.md`):

- **Numbered pairs.** Every change is a zero-padded, monotonically increasing
  number + a slug, in **two files**: `NNNN_<slug>.up.sql` and
  `NNNN_<slug>.down.sql`. The `up` applies the change; the `down` reverses
  **exactly** what that `up` created (Backend Standards → Migrations: every
  migration MUST have a working rollback).
- **The sequence is the timeline.** Migrations are applied in ascending number
  order and never modified once merged; a new change is always a new pair. The
  M2+ tables (mail, auction, guild, social, instance) will therefore be
  `0002+`, separate from this M0/M1 base — the numbering records *when* each
  table entered the DB.
- **`down` drops in reverse dependency order** (children before parents) so
  foreign keys never block a rollback, and uses `DROP TABLE IF EXISTS` to stay
  idempotent.
- **Reversibility rule of thumb.** An `up` that only `CREATE`s tables has a
  `down` that only `DROP`s them. A future `up` that adds a column has a `down`
  that drops that column. Keep one logical change per migration.

## How migrations are applied

Per server PRD §6 (OPS-01 / D-30), the one-shot `migrations` job in
`docker compose up` applies every pending `up` migration in number order —
against the auth DB **and the characters DB** — before the daemons accept
traffic. **The characters DB persists across redeploys**, so migrations are
**forward-only in production** — `down` files exist for local/dev rollback and
to *prove each `up` is reversible*, not for routine production downgrades.

Applying by hand (any MySQL client works — the files are plain DDL):

```sh
# apply, in order
mariadb characters < characters/migrations/0001_init_characters.up.sql

# roll back
mariadb characters < characters/migrations/0001_init_characters.down.sql
```

## Dialect

- **MariaDB / MySQL**, `ENGINE=InnoDB`, `DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_unicode_ci` on every table, declared explicitly (baseline
  TD-05), so the DDL is portable regardless of server defaults.
- **Timestamps** are `DATETIME` in UTC. Mutable entities carry `created_at`
  (default `CURRENT_TIMESTAMP`) and `updated_at` (`ON UPDATE
  CURRENT_TIMESTAMP`); append-only rows (`character_spell`, `character_aura`)
  carry `created_at` only.
- **Primary keys** are **server-minted** `AUTO_INCREMENT` surrogates
  (`character.id`, `item_instance.item_guid`, and the per-child `id`s), in
  contrast to the world DB's IF-9-assigned `uint32`s. `item_guid` is a durable,
  server-assigned item identity (referenced by inventory now; by mail/auction
  escrow from M2).
- **`money` is `BIGINT UNSIGNED`** (base currency units, e.g. copper) —
  server-managed integer currency, never `FLOAT`.

### Cross-DB references (§4.4) — numeric only, NO foreign keys

References that leave the characters DB are **numeric IDs only with NO foreign
key** — the auth and world DBs are separate schemas, so a cross-DB FK is
impossible in MySQL/MariaDB *and* forbidden by §4.4:

| Column | Points at | Kind |
|--------|-----------|------|
| `character.account_id` | auth DB `account.id` | soft numeric ref (no FK) |
| `character.map_id` | world DB zone/map | IF-9 numeric ref (no FK) |
| `item_instance.item_template_id` | world DB `item_template` | IF-9 numeric ref (no FK) |
| `item_instance.suffix_id` | world DB random-suffix (`0` = none) | IF-9 numeric ref (no FK) |
| `character_quest.quest_id` | world DB `quest_template` | IF-9 numeric ref (no FK) |
| `character_spell.spell_id` / `character_aura.spell_id` | world DB `ability` | IF-9 numeric ref (no FK) |
| `character_talent.talent_id` | world DB talent | IF-9 numeric ref (no FK) |

World IDs are stable across nightly rebuilds because `idmap.lock` is committed
(IF-9). Their integrity is re-checked **out-of-band** by `meridian-validate`
after a rebuild (a dangling ref is a content-CI failure, not a server crash) —
§4.4 — **not** by a foreign key.

### In-DB foreign keys and `ON DELETE` choices

Foreign keys exist **only within the characters DB** — the
`character ↔ character_inventory ↔ item_instance` core and the per-character
child tables — with explicit `ON DELETE`:

| FK | `ON DELETE` | Why |
|----|-------------|-----|
| `character_inventory.char_id → character.id` | `CASCADE` | deleting a character removes its inventory placements |
| `character_inventory.item_guid → item_instance.item_guid` | `CASCADE` | a placement cannot outlive the item it points at; `item_guid` is `UNIQUE` (an item is in at most one slot) |
| `item_instance.creator → character.id` | `SET NULL` | keep the item if its crafter is later deleted; `NULL` reads as "creator gone" |
| `character_quest.char_id → character.id` | `CASCADE` | quest progress is meaningless without its character |
| `character_spell.char_id → character.id` | `CASCADE` | learned abilities belong to the character |
| `character_talent.char_id → character.id` | `CASCADE` | talent points belong to the character |
| `character_aura.char_id → character.id` | `CASCADE` | persisted auras belong to the character |
| `character_aura.caster_guid → character.id` | `SET NULL` | keep the aura if the applying character is deleted (or the caster was an NPC) |

### `save_epoch` (§4.7) — reserved in v1

`character.save_epoch BIGINT NOT NULL DEFAULT 0` is the **save-ownership fence**.
The **column is reserved in this v1 migration** even though the fenced-write
*rule* lands at M2: it is a monotonically increasing epoch, and from M2 every
character-state `UPDATE` is compare-and-set — `UPDATE character … WHERE id = ?
AND save_epoch = ?` — so exactly one process may write a character's durable
state at any instant (a 0-row result is a fencing event: logged, alerted, write
discarded). `shard_index` is **deliberately not persisted** (§4.2) — shard
placement is recomputed on every zone-enter (shards are ephemeral).

## Verification status

Syntax validated with `sqlglot` (MySQL dialect) — all files parse, and the
`down` was confirmed to drop **exactly** the tables the `up` creates.
**Execution against a live MariaDB is deferred to the MariaDB-service CI job
(issue #187);** MariaDB is not part of the local toolchain, so no `up`/`down`
round-trip has been run against a real server here.
