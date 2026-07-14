-- =====================================================================
-- File 35: playable roster — races & classes (SP2.5 #695)
-- Mirrors: schema/content/race.schema.yaml, schema/content/class.schema.yaml
-- =====================================================================
--
-- The playable race/class roster, loaded from PACK DATA (retiring the compiled
-- server/characters/src/roster.h enum, SP2 design §2.2). mcc emit-sql walks the
-- `race`/`class` content entities and fills these tables; worldd loads them at
-- boot into a runtime meridian::characters::Roster (merged with a thin compiled
-- fallback for the entries not yet authorable in the pack). Character CREATE
-- validates the chosen race/class against that loaded roster.
--
-- KEY SCHEME (differs from the rest of the world DB): these two tables are keyed
-- by `roster_id`, the CANONICAL APPEND-ONLY roster id (the 1-based small id the
-- server persists in character.race / character.class and the client roster
-- mirror sends), NOT by the IF-9 idmap numeric id. That preserves the persisted
-- character.race/class contract (a stored id never changes meaning) across the
-- roster.h → pack-data migration. The IF-9 numeric id is still recorded in
-- `content_id` (UNIQUE) for traceability back to the pack entity.
--
-- SCOPE: SP2.5 loads the roster IDENTITY (id + name) — enough to validate a
-- create and name a race/class. SP2.7 (#697) extends the `class` family with the
-- equip-gating + role/talent rules columns: the `talent_tree_id` column below and
-- the `class_usable_equip_type` / `class_role` child tables. Ranges match TINYINT
-- UNSIGNED (roster_id 1..255).

-- ---------------------------------------------------------------------
-- race — a playable race (meridian/race@1). Cosmetic identity at M1.
-- ---------------------------------------------------------------------
CREATE TABLE race (
  roster_id   TINYINT UNSIGNED NOT NULL,   -- canonical roster id (character.race)
  content_id  INT UNSIGNED     NOT NULL,   -- IF-9 numeric id (idmap) — traceability
  name        VARCHAR(64)      NOT NULL,   -- displayName
  description VARCHAR(500)     NULL,

  PRIMARY KEY (roster_id),
  UNIQUE KEY uq_race_content (content_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- class — a playable class (meridian/class@1). The 7-field integrator; SP2.5
-- loads only the identity (id + name), later stories add the rules columns.
-- ---------------------------------------------------------------------
CREATE TABLE class (
  roster_id      TINYINT UNSIGNED NOT NULL,   -- canonical roster id (character.class)
  content_id     INT UNSIGNED     NOT NULL,   -- IF-9 numeric id (idmap) — traceability
  name           VARCHAR(64)      NOT NULL,   -- displayName
  description    VARCHAR(500)     NULL,
  talent_tree_id INT UNSIGNED     NULL,       -- talent_tree.content_id (SP2.7 #697); NULL = no tree

  PRIMARY KEY (roster_id),
  UNIQUE KEY uq_class_content (content_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- class_usable_equip_type — the class's usable_armor_types / usable_weapon_types
-- (SP2.7 #697). One row per (class, equip_type) proficiency; `list` records which
-- authoring list it came from (armor vs weapon), so the kernel can close the
-- SP1-deferred category-match at load (list must equal the equip_type's category)
-- AND equip-gate at runtime (an armor-slot item's equip_type must be in the class's
-- armor list). Keyed by roster_id + the equip_type's IF-9 numeric id.
-- ---------------------------------------------------------------------
CREATE TABLE class_usable_equip_type (
  class_roster_id TINYINT UNSIGNED NOT NULL,        -- -> class.roster_id
  equip_type_id   INT UNSIGNED     NOT NULL,        -- -> equip_type.content_id
  list            ENUM('armor','weapon') NOT NULL,  -- authoring list this came from
  PRIMARY KEY (class_roster_id, equip_type_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- class_role — the class's combat role(s) (SP2.7 #697). A single-role class has one
-- row; a hybrid class (schema `hybrid`) has 2-4 rows. Uniformly captures both the
-- `role` and `hybrid` branches of the class schema's role-XOR-hybrid rule. The
-- kernel reads the set to drive role hooks (e.g. a Tank-role class gets a threat
-- multiplier). Keyed by roster_id + the role token.
-- ---------------------------------------------------------------------
CREATE TABLE class_role (
  class_roster_id TINYINT UNSIGNED NOT NULL,        -- -> class.roster_id
  role            ENUM('healer','dps_melee','dps_ranged','tank') NOT NULL,
  PRIMARY KEY (class_roster_id, role)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
