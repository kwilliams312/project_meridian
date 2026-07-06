-- =====================================================================
-- File 20: items
-- Mirrors: schema/content/item.schema.yaml (ITM-01)
-- =====================================================================

-- ---------------------------------------------------------------------
-- item_template
--   Mirrors item.schema.yaml. Flattening:
--     weapon.*  -> weapon_*      (present only for item_class=weapon)
--     price.*   -> price_sell/price_buy (money -> BIGINT copper)
--     visual.*  -> visual_icon_id / visual_model_id (asset refs)
--     stats[]   -> item_stat child table
--     effects.on_use          -> effect_on_use_id  (single ability ref)
--     effects.on_equip[]      -> item_effect_on_equip child table
--   subclass is free-form (schema is a pattern, not an enum) -> VARCHAR.
-- ---------------------------------------------------------------------
CREATE TABLE item_template (
  id                INT UNSIGNED NOT NULL,                  -- IF-9 numeric id
  name              VARCHAR(80)  NOT NULL,                  -- displayName
  flavor_text       VARCHAR(500) NULL,
  item_class        ENUM('weapon','armor','consumable','quest',
                         'trade_good','container') NOT NULL,
  subclass          VARCHAR(32)  NULL,                      -- free-form pattern
  slot              ENUM('head','shoulders','back','chest','wrist','hands',
                         'waist','legs','feet','neck','finger','trinket',
                         'main_hand','off_hand','two_hand','ranged','bag') NULL,
  rarity            ENUM('poor','common','uncommon','rare','epic','legendary') NOT NULL,
  required_level    SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  item_level        SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  is_unique         BOOLEAN NOT NULL DEFAULT FALSE,         -- schema field `unique` (reserved word)
  binding           ENUM('none','on_pickup','on_equip') NOT NULL DEFAULT 'none',
  stack_size        SMALLINT UNSIGNED NOT NULL DEFAULT 1,

  -- weapon.* (required together when item_class=weapon)
  weapon_damage_min INT UNSIGNED NULL,                      -- weapon.damage (intRange)
  weapon_damage_max INT UNSIGNED NULL,
  weapon_speed_ms   INT UNSIGNED NULL,
  weapon_school     ENUM('physical','fire','frost','nature','shadow','holy','arcane')
                       NULL DEFAULT 'physical',

  armor             INT UNSIGNED NULL,                      -- top-level armor value

  -- effects.*
  effect_on_use_id  INT UNSIGNED NULL,                      -- effects.on_use (abilityRef)

  -- price.* (money = copper)
  price_sell        BIGINT UNSIGNED NULL,                   -- price.sell (omit = unsellable)
  price_buy         BIGINT UNSIGNED NULL,                   -- price.buy  (vendor default)

  -- visual.* (asset refs -> .pck; numeric ids for the id tie)
  visual_icon_id    INT UNSIGNED NULL,                      -- visual.icon (artRef, required in schema)
  visual_model_id   INT UNSIGNED NULL,                      -- visual.model (artRef)

  PRIMARY KEY (id),
  CONSTRAINT fk_item_onuse FOREIGN KEY (effect_on_use_id) REFERENCES ability (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- item_stat — stats[] (statKey + amount, max 8)
-- ---------------------------------------------------------------------
CREATE TABLE item_stat (
  item_id  INT UNSIGNED NOT NULL,                           -- -> item_template.id
  stat     ENUM('strength','agility','stamina','intellect','spirit') NOT NULL,
  amount   INT NOT NULL,                                    -- signed (can be negative)
  PRIMARY KEY (item_id, stat),
  CONSTRAINT fk_itemstat_item FOREIGN KEY (item_id) REFERENCES item_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- item_effect_on_equip — effects.on_equip[] (list of ability refs)
-- ---------------------------------------------------------------------
CREATE TABLE item_effect_on_equip (
  item_id     INT UNSIGNED NOT NULL,                        -- -> item_template.id
  ordinal     SMALLINT UNSIGNED NOT NULL,                   -- array position (deterministic order)
  ability_id  INT UNSIGNED NOT NULL,                        -- effects.on_equip[] (abilityRef)
  PRIMARY KEY (item_id, ordinal),
  CONSTRAINT fk_itemequip_item FOREIGN KEY (item_id)    REFERENCES item_template (id),
  CONSTRAINT fk_itemequip_abl  FOREIGN KEY (ability_id) REFERENCES ability (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
