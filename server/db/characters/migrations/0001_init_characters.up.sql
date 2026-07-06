-- =====================================================================
-- Project Meridian — characters DB schema v1 (migration 0001, UP)
-- Serves: docs/sad/server-sad.md §4.2 (characters DB field list — authoritative),
--         §4.4 (cross-DB reference rule), §4.7 (save_epoch ownership fence).
--
-- Owner: shard workers + servicesd. The characters DB is the ONLY durable
-- player state (§4.2) — it is written CONSTANTLY at runtime: dirty-flag batch
-- flush every 30 s, forced flush on logout/zone-transfer/shard-transfer/trade,
-- and single-transaction economy operations (never batched). The DB row is the
-- single durable arbiter of ownership (§4.7).
--
-- CONTRAST WITH world DB (schema/sql/world/): the world DB is an mcc-produced,
-- read-only artifact replaced wholesale nightly, keyed by IF-9 uint32s with NO
-- AUTO_INCREMENT. The characters DB is the opposite: SERVER-MANAGED and
-- WRITTEN AT RUNTIME. Therefore this schema uses:
--   * AUTO_INCREMENT surrogate PKs where the key is server-minted
--     (character.id, character_inventory.id, character_quest.id, ...);
--   * item_instance.item_guid — a server-minted BIGINT UNSIGNED item GUID
--     (AUTO_INCREMENT here: it is a durable, server-assigned identity);
--   * created_at / updated_at audit timestamps on the mutable entities;
--   * reversible, numbered migrations (0001 is the M0/M1 base; M2+ tables —
--     mail/auction/guild/social/instance — land in later migrations).
--
-- CROSS-DB REFERENCES (§4.4): references to the world DB (item_template_id,
-- quest_id, suffix_id, ...) and to the auth DB (account_id) are NUMERIC ONLY
-- with NO foreign key. The world and auth DBs are separate schemas; a cross-DB
-- FK is impossible in MySQL/MariaDB and forbidden by §4.4. World IDs are IF-9
-- numeric IDs, stable across nightly rebuilds (idmap.lock is committed); their
-- integrity is re-checked out-of-band by meridian-validate, not by an FK.
--
-- SAVE_EPOCH (§4.7): character.save_epoch is the save-ownership fence. It is
-- reserved in v1 (this migration) even though the fenced-write RULE lands at M2
-- — the COLUMN belongs to the character row from v1 per the SAD. It is a
-- monotonically increasing epoch; the epoch-E owner is the only legitimate
-- writer, and from M2 every character-state UPDATE carries `AND save_epoch = ?`
-- (CAS). shard_index is deliberately NOT persisted (§4.2) — placement is
-- recomputed on every zone-enter (shards are ephemeral).
--
-- DIALECT ...... MariaDB / MySQL, ENGINE=InnoDB, utf8mb4 (TD-05), per-table
--                CHARSET/COLLATE declared explicitly for portability.
-- TIME ......... All timestamps are DATETIME in UTC (the server writes UTC);
--                created_at/updated_at use CURRENT_TIMESTAMP defaults so a raw
--                INSERT that omits them is still audit-correct.
-- =====================================================================

SET NAMES utf8mb4;

-- ---------------------------------------------------------------------
-- character — one row per player character (§4.2). The root durable entity.
--   id is a server-minted surrogate (BIGINT UNSIGNED AUTO_INCREMENT) — characters
--   are created at runtime. account_id is a SOFT numeric reference into the auth
--   DB (account.id) with NO cross-DB FK (§4.4). name is UNIQUE (case-insensitive
--   via collation). money is BIGINT (server-managed currency; whole copper units,
--   never FLOAT). pos_x/y/z are world coordinates; pos_o is orientation (radians).
--   save_epoch is the §4.7 ownership fence, reserved now (see header).
-- ---------------------------------------------------------------------
CREATE TABLE `character` (
  id            BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,
  account_id    BIGINT UNSIGNED   NOT NULL,                      -- SOFT ref -> auth DB account.id (NO cross-DB FK, §4.4)
  name          VARCHAR(32)       NOT NULL,                      -- character name; case-insensitive UNIQUE via collation
  race          TINYINT UNSIGNED  NOT NULL,                      -- race id (world DB / rules enum)
  class         TINYINT UNSIGNED  NOT NULL,                      -- class id (world DB / rules enum)
  level         SMALLINT UNSIGNED NOT NULL DEFAULT 1,            -- character level
  xp            INT UNSIGNED      NOT NULL DEFAULT 0,            -- experience toward next level
  money         BIGINT UNSIGNED   NOT NULL DEFAULT 0,            -- currency in base units (copper); server-managed, never FLOAT
  map_id        INT UNSIGNED      NOT NULL,                      -- current map/zone (world DB numeric id, §4.4)
  instance_id   INT UNSIGNED      NOT NULL DEFAULT 0,            -- 0 = open world; >0 = instance id (M2 instancing)
  pos_x         FLOAT             NOT NULL,                      -- world position X
  pos_y         FLOAT             NOT NULL,                      -- world position Y
  pos_z         FLOAT             NOT NULL,                      -- world position Z
  pos_o         FLOAT             NOT NULL DEFAULT 0,            -- orientation (facing), radians
  played_time   INT UNSIGNED      NOT NULL DEFAULT 0,            -- total played time, seconds
  logout_at     DATETIME          NULL,                          -- last clean logout; NULL = never logged out / online
  save_epoch    BIGINT            NOT NULL DEFAULT 0,            -- §4.7 save-ownership fence (reserved v1; fenced writes M2)
  created_at    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_character_name (name),
  KEY idx_character_account (account_id)                        -- "list my characters" query by account
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- item_instance — one row per concrete item in the world (§4.2).
--   item_guid is a durable, server-minted item identity (AUTO_INCREMENT). It is
--   referenced by character_inventory (equipped/bagged), and later by mail_item /
--   auction (escrow). item_template_id is a NUMERIC-ONLY reference into the world
--   DB (IF-9 uint32, §4.4) — NO cross-DB FK. stack is the stack count; durability
--   the remaining durability; suffix_id an optional random-suffix ref (world DB,
--   numeric, 0 = none); creator the crafting character.id (soft in-DB ref, kept
--   nullable + SET NULL so deleting the crafter does not destroy the item).
--
--   Defined BEFORE character_inventory because inventory FKs item_guid.
-- ---------------------------------------------------------------------
CREATE TABLE item_instance (
  item_guid         BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,    -- server-minted durable item identity
  item_template_id  INT UNSIGNED     NOT NULL,                   -- NUMERIC ref -> world DB item_template (IF-9, NO FK, §4.4)
  stack             INT UNSIGNED     NOT NULL DEFAULT 1,         -- stack count
  durability        INT UNSIGNED     NOT NULL DEFAULT 0,         -- remaining durability (0 = n/a or broken)
  suffix_id         INT UNSIGNED     NOT NULL DEFAULT 0,         -- NUMERIC ref -> world DB random-suffix (0 = none, NO FK, §4.4)
  creator           BIGINT UNSIGNED  NULL,                       -- crafting character.id (in-DB soft ref; SET NULL on delete)
  created_at        DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at        DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (item_guid),
  KEY idx_item_instance_template (item_template_id),
  KEY idx_item_instance_creator (creator),
  CONSTRAINT fk_item_instance_creator
    FOREIGN KEY (creator) REFERENCES `character` (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- character_inventory — placement of item_instances for a character (§4.2).
--   Maps (char_id, bag, slot) -> item_guid. bag 0 = backpack/equipped set; bag>0
--   = a container item's slots. slot layout is server-validated (§4.2). Both FKs
--   are WITHIN the characters DB:
--     char_id  -> character(id)      ON DELETE CASCADE  — deleting a character
--                 removes its inventory placements.
--     item_guid -> item_instance     ON DELETE CASCADE  — an inventory row cannot
--                 outlive the item it points at; destroying the item removes the
--                 placement. item_guid is UNIQUE (an item is in at most one slot).
--   UNIQUE (char_id, bag, slot) enforces one item per slot.
-- ---------------------------------------------------------------------
CREATE TABLE character_inventory (
  id          BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,          -- surrogate PK for the placement row
  char_id     BIGINT UNSIGNED  NOT NULL,                         -- owning character (in-DB FK)
  bag         TINYINT UNSIGNED NOT NULL,                         -- 0 = backpack/equipped; >0 = container item
  slot        SMALLINT UNSIGNED NOT NULL,                        -- slot within the bag (server-validated layout)
  item_guid   BIGINT UNSIGNED  NOT NULL,                         -- placed item (in-DB FK -> item_instance)
  created_at  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_character_inventory_slot (char_id, bag, slot),  -- one item per (char, bag, slot)
  UNIQUE KEY uq_character_inventory_item (item_guid),           -- an item is in at most one slot
  KEY idx_character_inventory_char (char_id),
  CONSTRAINT fk_character_inventory_char
    FOREIGN KEY (char_id)   REFERENCES `character` (id)   ON DELETE CASCADE,
  CONSTRAINT fk_character_inventory_item
    FOREIGN KEY (item_guid) REFERENCES item_instance (item_guid) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- character_quest — per-character quest progress (§4.2).
--   quest_id is a NUMERIC-ONLY reference into the world DB (IF-9, §4.4) — NO
--   cross-DB FK. state is the quest state (accepted/complete/failed/rewarded —
--   a rules enum). objective_counts holds per-objective progress; stored as a
--   compact JSON document (MariaDB JSON = LONGTEXT alias) so the objective count
--   layout can evolve with content without a schema change. completed_at is set
--   when the quest is turned in. char_id FK is WITHIN the characters DB
--   (ON DELETE CASCADE). UNIQUE (char_id, quest_id) — one progress row per quest.
-- ---------------------------------------------------------------------
CREATE TABLE character_quest (
  id                BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  char_id           BIGINT UNSIGNED  NOT NULL,                   -- owning character (in-DB FK)
  quest_id          INT UNSIGNED     NOT NULL,                   -- NUMERIC ref -> world DB quest_template (IF-9, NO FK, §4.4)
  state             TINYINT UNSIGNED NOT NULL DEFAULT 0,         -- quest state enum (accepted/complete/failed/rewarded)
  objective_counts  JSON             NULL,                       -- per-objective progress (compact JSON; layout is content-driven)
  completed_at      DATETIME         NULL,                       -- turn-in time; NULL = not completed
  created_at        DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at        DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_character_quest (char_id, quest_id),            -- one progress row per (char, quest)
  KEY idx_character_quest_char (char_id),
  CONSTRAINT fk_character_quest_char
    FOREIGN KEY (char_id) REFERENCES `character` (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- character_spell — abilities a character has learned (§4.2; M1).
--   spell_id is a NUMERIC-ONLY reference into the world DB ability table (IF-9,
--   §4.4) — NO cross-DB FK. One row per learned ability. char_id FK is WITHIN the
--   characters DB (ON DELETE CASCADE). UNIQUE (char_id, spell_id).
-- ---------------------------------------------------------------------
CREATE TABLE character_spell (
  id          BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  char_id     BIGINT UNSIGNED  NOT NULL,                         -- owning character (in-DB FK)
  spell_id    INT UNSIGNED     NOT NULL,                         -- NUMERIC ref -> world DB ability (IF-9, NO FK, §4.4)
  created_at  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_character_spell (char_id, spell_id),            -- learned once per character
  KEY idx_character_spell_char (char_id),
  CONSTRAINT fk_character_spell_char
    FOREIGN KEY (char_id) REFERENCES `character` (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- character_talent — talent points spent per talent (§4.2; M2).
--   talent_id is a NUMERIC-ONLY reference into the world DB (IF-9, §4.4) — NO
--   cross-DB FK. rank is the points spent in that talent. char_id FK is WITHIN
--   the characters DB (ON DELETE CASCADE). UNIQUE (char_id, talent_id).
-- ---------------------------------------------------------------------
CREATE TABLE character_talent (
  id          BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  char_id     BIGINT UNSIGNED  NOT NULL,                         -- owning character (in-DB FK)
  talent_id   INT UNSIGNED     NOT NULL,                         -- NUMERIC ref -> world DB talent (IF-9, NO FK, §4.4)
  rank        TINYINT UNSIGNED NOT NULL DEFAULT 0,               -- points spent in this talent
  created_at  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_character_talent (char_id, talent_id),          -- one row per (char, talent)
  KEY idx_character_talent_char (char_id),
  CONSTRAINT fk_character_talent_char
    FOREIGN KEY (char_id) REFERENCES `character` (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- character_aura — auras (buffs/debuffs) persisted across logout (§4.2; M1).
--   spell_id is a NUMERIC-ONLY reference into the world DB ability table (IF-9,
--   §4.4) — NO cross-DB FK. stacks is the aura stack count; remaining_ms the
--   remaining duration in milliseconds (re-armed on login); caster_guid the
--   applying unit (soft in-DB ref to character.id, nullable + SET NULL — the
--   caster may be an NPC or a since-deleted character). char_id FK is WITHIN the
--   characters DB (ON DELETE CASCADE).
-- ---------------------------------------------------------------------
CREATE TABLE character_aura (
  id            BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  char_id       BIGINT UNSIGNED  NOT NULL,                       -- owning character (in-DB FK)
  spell_id      INT UNSIGNED     NOT NULL,                       -- NUMERIC ref -> world DB ability (IF-9, NO FK, §4.4)
  stacks        TINYINT UNSIGNED NOT NULL DEFAULT 1,             -- aura stack count
  remaining_ms  INT UNSIGNED     NOT NULL DEFAULT 0,             -- remaining duration at logout, milliseconds
  caster_guid   BIGINT UNSIGNED  NULL,                           -- applying character.id (soft in-DB ref; SET NULL on delete)
  created_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_character_aura_char (char_id),
  KEY idx_character_aura_caster (caster_guid),
  CONSTRAINT fk_character_aura_char
    FOREIGN KEY (char_id)     REFERENCES `character` (id) ON DELETE CASCADE,
  CONSTRAINT fk_character_aura_caster
    FOREIGN KEY (caster_guid) REFERENCES `character` (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- M2+ tables (STUB — deliberately NOT implemented in v1):
--   mail, mail_item ...................... §4.2, transactional; owner servicesd (M2)
--   auction .............................. §4.2, escrow; owner servicesd (M2)
--   instance_bind, instance_state ........ §4.2, group<->instance binding (M2)
--   guild, guild_member, guild_rank ...... §4.2; owner servicesd (M3)
--   character_social ..................... §4.2, friends/ignore; owner servicesd (M3)
-- Each lands in its own numbered migration when its milestone arrives — the
-- migration sequence is the timeline of the DB (see README). They are listed
-- here only as a roadmap note; adding them now would be premature (YAGNI) and
-- would put un-owned tables into the M0/M1 base.
-- ---------------------------------------------------------------------
