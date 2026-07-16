-- =====================================================================
-- File 10: NPCs / mobs
-- Mirrors: schema/content/npc.schema.yaml (NPC-01, NPC-02, CMB-02)
-- =====================================================================

-- ---------------------------------------------------------------------
-- npc_template
--   Mirrors npc.schema.yaml. Nested objects are flattened into columns:
--     stats.*      -> stat_*          (required sub-object)
--     ai.*         -> ai_*            (scalar params; ai.abilities -> npc_ability)
--     movement.*   -> move_*
--     interaction.gossip_text -> gossip table (90_gossip.sql)
--     interaction.vendor      -> vendor_ref_id (numeric ref -> vendor_inventory)
--     loot.*       -> loot_table_ref_id + loot_money_min/max
--     visual.*     -> visual_* (model/sound_set are asset refs -> .pck, not
--                     server-loaded; kept as numeric ids for the id tie)
--   intRange fields become <field>_min / <field>_max.
-- ---------------------------------------------------------------------
CREATE TABLE npc_template (
  id                       INT UNSIGNED NOT NULL,            -- IF-9 numeric id (no AUTO_INCREMENT)
  name                     VARCHAR(80)  NOT NULL,           -- displayName
  subtitle                 VARCHAR(80)  NULL,
  level_min                SMALLINT UNSIGNED NOT NULL,      -- level (intRange)
  level_max                SMALLINT UNSIGNED NOT NULL,
  creature_type            ENUM('humanoid','beast','undead','elemental',
                                'demon','dragonkin','mechanical','critter') NOT NULL,
  `rank`                   ENUM('normal','elite','rare','boss') NOT NULL DEFAULT 'normal',
  faction                  ENUM('friendly','neutral','hostile') NOT NULL,

  -- stats.* (required sub-object)
  stat_health              INT UNSIGNED NOT NULL,
  stat_mana                INT UNSIGNED NULL,
  stat_armor               INT UNSIGNED NULL,
  stat_damage_min          INT UNSIGNED NULL,               -- stats.damage (intRange)
  stat_damage_max          INT UNSIGNED NULL,
  stat_attack_speed_ms     INT UNSIGNED NOT NULL,

  -- ai.* (scalars; ai.abilities -> npc_ability child table)
  ai_behavior              ENUM('aggressive','defensive','passive') NULL DEFAULT 'defensive',
  ai_aggro_radius_m        FLOAT NULL DEFAULT 20,
  ai_leash_radius_m        FLOAT NULL DEFAULT 60,
  ai_call_for_help_radius_m FLOAT NULL DEFAULT 0,
  ai_flee_at_health_pct    FLOAT NULL,                      -- pct (0..100)

  -- movement.*
  move_walk_speed_mps      FLOAT NULL DEFAULT 2.5,
  move_run_speed_mps       FLOAT NULL DEFAULT 7.0,

  -- interaction.*
  vendor_ref_id            INT UNSIGNED NULL,               -- interaction.vendor -> vendor_inventory.id
  -- interaction.gossip_text lives in gossip (one page per npc) — 90_gossip.sql

  -- loot.*
  loot_table_ref_id        INT UNSIGNED NULL,               -- loot.table -> loot_table.id
  loot_money_min           BIGINT UNSIGNED NULL,            -- loot.money (intRange copper, D-25 additive)
  loot_money_max           BIGINT UNSIGNED NULL,

  -- visual.* (asset refs are client-facing; numeric ids kept for the id tie).
  -- @2 makes visual a oneOf: branch A is `model` (below); branch B assembles the NPC
  -- like a player from an appearance_catalog, projected to the visual_appearance_*
  -- columns. Exactly ONE branch is populated per NPC: a model-only NPC has
  -- visual_model_id set + every visual_appearance_* NULL; an appearance NPC has
  -- visual_model_id NULL + the visual_appearance_* set. scale/sound_set apply to both.
  visual_model_id          INT UNSIGNED NULL,               -- visual.model (branch A artRef)
  visual_scale             FLOAT NULL DEFAULT 1.0,
  visual_sound_set_id      INT UNSIGNED NULL,               -- visual.sound_set (sfxRef)

  -- visual.appearance (@2 branch B) — the CharacterVisual scalars worldd relays on
  -- EntityEnter (identity.visual), the SAME per-race/sex assemble+recolor path a
  -- player uses. All NULL for a model-only NPC (every M1 NPC). race_id is the race
  -- ROSTER id (character.race space, not the IF-9 content id); sex 0=male/1=female;
  -- hair/face/skin are 1-based appearance_catalog preset ids.
  visual_appearance_race_id  TINYINT UNSIGNED NULL,         -- race roster id (1..255)
  visual_appearance_sex      TINYINT UNSIGNED NULL,         -- 0 = male, 1 = female
  visual_appearance_hair     TINYINT UNSIGNED NULL,         -- hair preset id (1-based)
  visual_appearance_face     TINYINT UNSIGNED NULL,         -- face preset id (1-based)
  visual_appearance_skin     TINYINT UNSIGNED NULL,         -- skin preset id (1-based)

  PRIMARY KEY (id),
  CONSTRAINT fk_npc_vendor  FOREIGN KEY (vendor_ref_id)     REFERENCES vendor_inventory (id),
  CONSTRAINT fk_npc_loot    FOREIGN KEY (loot_table_ref_id) REFERENCES loot_table (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- npc_ability — ai.abilities[] (CMB-02 AI ability list)
--   Mirrors npc.schema.yaml ai.abilities[] items.
-- ---------------------------------------------------------------------
CREATE TABLE npc_ability (
  npc_id                   INT UNSIGNED NOT NULL,           -- -> npc_template.id
  ability_id               INT UNSIGNED NOT NULL,           -- ability (abilityRef) -> ability.id
  priority                 INT UNSIGNED NOT NULL DEFAULT 0, -- lower considered first
  cooldown_override_ms     INT UNSIGNED NULL,
  use_at_health_below_pct  FLOAT NULL,                      -- pct
  PRIMARY KEY (npc_id, ability_id),
  CONSTRAINT fk_npcability_npc FOREIGN KEY (npc_id)     REFERENCES npc_template (id),
  CONSTRAINT fk_npcability_abl FOREIGN KEY (ability_id) REFERENCES ability (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- npc_trainer — the trainer role marker (NPC-02, #392)
--   Mirrors npc.schema.yaml interaction.trainer. One row per NPC that
--   teaches abilities; presence of the row (and its npc_trainer_ability
--   children) is what surfaces the "train" gossip option. The parent table
--   is the FK anchor for npc_trainer_ability and the "is a trainer" flag.
-- ---------------------------------------------------------------------
CREATE TABLE npc_trainer (
  npc_id                   INT UNSIGNED NOT NULL,           -- -> npc_template.id
  PRIMARY KEY (npc_id),
  CONSTRAINT fk_npctrainer_npc FOREIGN KEY (npc_id) REFERENCES npc_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- npc_trainer_ability — interaction.trainer[] taught abilities (NPC-02, #392)
--   Mirrors npc.schema.yaml interaction.trainer[] items. Each row is one
--   ability the trainer teaches, with its copper cost + class/level gate.
--   required_class is the class-name ENUM (NULL = any class may learn),
--   mapped to the roster Class id (roster.h) by the worldd loader.
-- ---------------------------------------------------------------------
CREATE TABLE npc_trainer_ability (
  npc_id                   INT UNSIGNED NOT NULL,           -- -> npc_trainer.npc_id
  ability_id               INT UNSIGNED NOT NULL,           -- ability (abilityRef) -> ability.id
  cost_copper              BIGINT UNSIGNED NOT NULL DEFAULT 0,  -- cost (money, copper)
  required_class           ENUM('vanguard','runcaller','warden','mender') NULL,  -- NULL = any class
  required_level           SMALLINT UNSIGNED NOT NULL DEFAULT 1, -- min character level to learn
  PRIMARY KEY (npc_id, ability_id),
  CONSTRAINT fk_npctrainerabl_trainer FOREIGN KEY (npc_id)     REFERENCES npc_trainer (npc_id),
  CONSTRAINT fk_npctrainerabl_ability FOREIGN KEY (ability_id) REFERENCES ability (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
