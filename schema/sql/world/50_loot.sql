-- =====================================================================
-- File 50: loot tables
-- Mirrors: schema/content/loot.schema.yaml (ITM-02)
-- =====================================================================

-- ---------------------------------------------------------------------
-- loot_table
--   Mirrors loot.schema.yaml top level. `money` (intRange copper) is
--   additive with the referencing NPC's loot.money (D-25).
--   entries[] -> loot_entry (group_id NULL); groups[] -> loot_group;
--   group entries -> loot_entry (group_id set).
-- ---------------------------------------------------------------------
CREATE TABLE loot_table (
  id          INT UNSIGNED NOT NULL,                        -- IF-9 numeric id
  money_min   BIGINT UNSIGNED NULL,                         -- money (intRange copper)
  money_max   BIGINT UNSIGNED NULL,
  PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- loot_group — groups[]: pick exactly `pick` winners by weight
--   (mutually exclusive drops). group entries live in loot_entry.
-- ---------------------------------------------------------------------
CREATE TABLE loot_group (
  loot_table_id  INT UNSIGNED NOT NULL,                     -- -> loot_table.id
  ordinal        SMALLINT UNSIGNED NOT NULL,                -- array position
  name           VARCHAR(64)  NOT NULL,                     -- groups[].name (pattern)
  pick           TINYINT UNSIGNED NOT NULL,                 -- 1..5
  chance_pct     FLOAT NULL,                                -- chance the group rolls at all (default 100)
  PRIMARY KEY (loot_table_id, ordinal),
  CONSTRAINT fk_lootgroup_table FOREIGN KEY (loot_table_id) REFERENCES loot_table (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- loot_entry — both top-level entries[] and group entries[].
--   group_ordinal NULL  -> a top-level independent roll (uses chance_pct + quest).
--   group_ordinal set   -> a member of that loot_group (uses weight).
--   Each entry drops either an item OR a nested table (one level, lint L052).
-- ---------------------------------------------------------------------
CREATE TABLE loot_entry (
  loot_table_id   INT UNSIGNED NOT NULL,                    -- owning table -> loot_table.id
  entry_ordinal   SMALLINT UNSIGNED NOT NULL,               -- array position (unique within scope)
  group_ordinal   SMALLINT UNSIGNED NULL,                   -- NULL = top-level; else -> loot_group.ordinal
  item_id         INT UNSIGNED NULL,                        -- item (itemRef)  — XOR with nested table
  nested_table_id INT UNSIGNED NULL,                        -- table (lootRef) — XOR with item
  chance_pct      FLOAT NULL,                               -- top-level entries[].chance_pct
  weight          INT UNSIGNED NULL,                        -- group entries[].weight
  quantity_min    INT UNSIGNED NULL,                        -- quantity (intRange)
  quantity_max    INT UNSIGNED NULL,
  quest_ref_id    INT UNSIGNED NULL,                        -- entries[].quest (questRef; drop-gate ITM-02)
  PRIMARY KEY (loot_table_id, entry_ordinal),
  CONSTRAINT fk_lootentry_table  FOREIGN KEY (loot_table_id) REFERENCES loot_table (id),
  CONSTRAINT fk_lootentry_group  FOREIGN KEY (loot_table_id, group_ordinal)
    REFERENCES loot_group (loot_table_id, ordinal),
  CONSTRAINT fk_lootentry_item   FOREIGN KEY (item_id)         REFERENCES item_template (id),
  CONSTRAINT fk_lootentry_nested FOREIGN KEY (nested_table_id) REFERENCES loot_table (id),
  CONSTRAINT fk_lootentry_quest  FOREIGN KEY (quest_ref_id)    REFERENCES quest_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
