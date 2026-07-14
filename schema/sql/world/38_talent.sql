-- =====================================================================
-- File 38: talents & talent trees (SP2.7 #697)
-- Mirrors: schema/content/talent.schema.yaml (meridian/talent@1),
--          schema/content/talent_tree.schema.yaml (meridian/talent_tree@1)
-- =====================================================================
--
-- A class's tiered talent tree and the talents it awards (SP2 design §2.4/§2.5).
-- A talent `grants` a bundle of active abilities and/or PASSIVE stat effects
-- (the ability effect-primitive palette — buff/debuff over an attribute id). The
-- kernel (worldd) loads these at boot and applies a class's talent grants to a
-- character: granted ability ids join the character's usable set, and passive
-- stat effects fold into the SP2.4 effective-stat framework (as an AttributeDelta
-- layer, keyed by attribute ref — the SAME key the aura ledger + class/race mods
-- use). A talent passive is permanent while talented, so `duration_ms` is
-- OPTIONAL (NULL = permanent), unlike an ability's timed aura.
--
-- KEYED by the IF-9 idmap numeric id (`content_id`), like every content family;
-- cross-references (talent_grant.ability_id, class.talent_tree_id,
-- talent_tree_tier_talent.talent_id) carry the same numeric id. No cross-content
-- FK (attribute-framework precedent: validated by mcc/L011, not FK-enforced).

-- ---------------------------------------------------------------------
-- talent — one perk a talent tree awards (meridian/talent@1).
-- ---------------------------------------------------------------------
CREATE TABLE talent (
  content_id  INT UNSIGNED  NOT NULL,                -- IF-9 numeric id (idmap)
  talent_ref  VARCHAR(96)   NOT NULL,                -- verbatim contentId (core:talent.battle_fury)
  name        VARCHAR(64)   NOT NULL,                -- displayName
  rank_max    SMALLINT UNSIGNED NOT NULL DEFAULT 1,  -- max points sinkable (schema default 1)

  PRIMARY KEY (content_id),
  UNIQUE KEY uq_talent_ref (talent_ref)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- talent_grant — talent.grants[] (a tagged union by `kind`). An `ability` grant
-- names an ability id; a `buff`/`debuff` grant is a passive stat effect over an
-- attribute ref (amount + modifier + optional duration/max_stacks). Ordered by
-- `ordinal` (array position) for a deterministic dump + application order.
-- ---------------------------------------------------------------------
CREATE TABLE talent_grant (
  talent_id     INT UNSIGNED NOT NULL,                     -- -> talent.content_id
  ordinal       SMALLINT UNSIGNED NOT NULL,                -- array position
  kind          ENUM('ability','buff','debuff') NOT NULL,
  ability_id    INT UNSIGNED NULL,                         -- kind=ability: -> ability.id
  attribute_ref VARCHAR(64)  NULL,                         -- kind=buff/debuff: attribute contentId
  amount        INT          NULL,                         -- kind=buff/debuff: signed modifier
  modifier      ENUM('flat','percent') NULL,               -- kind=buff/debuff: how amount applies
  duration_ms   INT UNSIGNED NULL,                         -- kind=buff/debuff: NULL = permanent passive
  max_stacks    SMALLINT UNSIGNED NULL,                    -- kind=buff/debuff: default 1

  PRIMARY KEY (talent_id, ordinal)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- talent_tree — a class's tiered talent progression (meridian/talent_tree@1).
-- ---------------------------------------------------------------------
CREATE TABLE talent_tree (
  content_id  INT UNSIGNED  NOT NULL,                -- IF-9 numeric id (idmap)
  tree_ref    VARCHAR(96)   NOT NULL,                -- verbatim contentId (core:talent_tree.vanguard_path)
  name        VARCHAR(64)   NOT NULL,                -- displayName
  description VARCHAR(500)  NULL,

  PRIMARY KEY (content_id),
  UNIQUE KEY uq_talent_tree_ref (tree_ref)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- talent_tree_tier — one row per tier (talent_tree.tiers[]). `required_points` is
-- the spent-point threshold that unlocks the tier (row-unlock by points, spec
-- §2.5). Ordered by `tier_ordinal` (array position, ascending threshold).
-- ---------------------------------------------------------------------
CREATE TABLE talent_tree_tier (
  talent_tree_id  INT UNSIGNED NOT NULL,             -- -> talent_tree.content_id
  tier_ordinal    SMALLINT UNSIGNED NOT NULL,        -- array position
  required_points SMALLINT UNSIGNED NOT NULL,        -- points spent to unlock this tier

  PRIMARY KEY (talent_tree_id, tier_ordinal)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- talent_tree_tier_talent — the talents a tier offers (tiers[].talents[]). One row
-- per talent in the tier, ordered by `ordinal`.
-- ---------------------------------------------------------------------
CREATE TABLE talent_tree_tier_talent (
  talent_tree_id INT UNSIGNED NOT NULL,              -- -> talent_tree.content_id
  tier_ordinal   SMALLINT UNSIGNED NOT NULL,         -- -> talent_tree_tier.tier_ordinal
  ordinal        SMALLINT UNSIGNED NOT NULL,         -- array position within the tier
  talent_id      INT UNSIGNED NOT NULL,              -- -> talent.content_id

  PRIMARY KEY (talent_tree_id, tier_ordinal, ordinal)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
