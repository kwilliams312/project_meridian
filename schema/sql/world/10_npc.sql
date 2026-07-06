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

  -- visual.* (asset refs are client-facing; numeric ids kept for the id tie)
  visual_model_id          INT UNSIGNED NULL,               -- visual.model (artRef; required in schema)
  visual_scale             FLOAT NULL DEFAULT 1.0,
  visual_sound_set_id      INT UNSIGNED NULL,               -- visual.sound_set (sfxRef)

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
