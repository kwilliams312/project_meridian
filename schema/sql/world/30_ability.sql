-- =====================================================================
-- File 30: abilities / spells (players AND mobs)
-- Mirrors: schema/content/ability.schema.yaml (CMB-01, CMB-04)
-- =====================================================================

-- ---------------------------------------------------------------------
-- ability
--   Mirrors ability.schema.yaml. Flattening:
--     cast.*      -> cast_time_ms / cast_channel_ms
--     resource.*  -> resource_type / resource_amount
--     audio_visual.* -> av_* (client-facing anim + asset refs)
--     effects[]   -> ability_effect child table (oneOf discriminated by kind)
-- ---------------------------------------------------------------------
CREATE TABLE ability (
  id               INT UNSIGNED NOT NULL,                   -- IF-9 numeric id
  name             VARCHAR(80)  NOT NULL,                   -- displayName
  description      VARCHAR(500) NULL,
  school           ENUM('physical','fire','frost','nature','shadow','holy','arcane') NOT NULL,
  target           ENUM('self','enemy','friendly') NOT NULL,
  range_m          FLOAT NOT NULL DEFAULT 5,

  -- cast.*
  cast_time_ms     INT UNSIGNED NULL DEFAULT 0,             -- 0 = instant
  cast_channel_ms  INT UNSIGNED NULL,

  cooldown_ms      INT UNSIGNED NOT NULL DEFAULT 0,
  triggers_gcd     BOOLEAN NOT NULL DEFAULT TRUE,

  -- resource.* (optional sub-object; both required together when present)
  resource_type    ENUM('mana','rage','energy') NULL,
  resource_amount  INT UNSIGNED NULL,

  -- audio_visual.* (client-facing; asset refs kept as numeric ids)
  av_cast_anim     VARCHAR(64)  NULL,
  av_cast_vfx_id   INT UNSIGNED NULL,
  av_cast_sfx_id   INT UNSIGNED NULL,
  av_impact_vfx_id INT UNSIGNED NULL,
  av_impact_sfx_id INT UNSIGNED NULL,

  PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- ability_effect — effects[] (1..4), oneOf discriminated by `kind`.
--   damage/heal: amount_min/max (+ coefficient)
--   aura:        duration_ms, max_stacks, and periodic_* (inline periodic)
--   threat:      threat_amount (flat)
--   aura.stat_mods[] -> ability_effect_stat_mod child table.
-- ---------------------------------------------------------------------
CREATE TABLE ability_effect (
  ability_id       INT UNSIGNED NOT NULL,                   -- -> ability.id
  ordinal          SMALLINT UNSIGNED NOT NULL,              -- array position (max 4)
  kind             ENUM('damage','heal','aura','threat') NOT NULL,

  -- damage / heal
  amount_min       INT UNSIGNED NULL,                       -- amount (intRange)
  amount_max       INT UNSIGNED NULL,
  coefficient      FLOAT NULL,                              -- 0..2

  -- threat
  threat_amount    INT NULL,                                -- flat threat (signed)

  -- aura
  duration_ms      INT UNSIGNED NULL,
  max_stacks       SMALLINT UNSIGNED NULL DEFAULT 1,
  -- aura.periodic.* (inline; single object)
  periodic_kind    ENUM('damage','heal') NULL,
  periodic_amount_min INT UNSIGNED NULL,                    -- periodic.amount (intRange)
  periodic_amount_max INT UNSIGNED NULL,
  periodic_tick_ms INT UNSIGNED NULL,

  PRIMARY KEY (ability_id, ordinal),
  CONSTRAINT fk_ableffect_abl FOREIGN KEY (ability_id) REFERENCES ability (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- ability_effect_stat_mod — aura effect stat_mods[]
-- ---------------------------------------------------------------------
CREATE TABLE ability_effect_stat_mod (
  ability_id  INT UNSIGNED NOT NULL,                        -- -> ability_effect.ability_id
  ordinal     SMALLINT UNSIGNED NOT NULL,                   -- -> ability_effect.ordinal
  stat        ENUM('strength','agility','stamina','intellect','spirit') NOT NULL,
  amount      INT NOT NULL,                                 -- signed
  PRIMARY KEY (ability_id, ordinal, stat),
  CONSTRAINT fk_statmod_effect FOREIGN KEY (ability_id, ordinal)
    REFERENCES ability_effect (ability_id, ordinal)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
