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
-- create and name a race/class. SP2.6 (#696) adds the class `race_limits`
-- content gate (the `class_race_limit` child table below), consumed by
-- character CREATE (race ∈ class race_limits). The remaining richer class fields
-- (the spellbook, usable armor/weapon types, role/talents) that #697
-- (equip-gating) consumes stay deferred to that story, which extends these
-- tables. Ranges match TINYINT UNSIGNED (roster_id 1..255).

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
  roster_id   TINYINT UNSIGNED NOT NULL,   -- canonical roster id (character.class)
  content_id  INT UNSIGNED     NOT NULL,   -- IF-9 numeric id (idmap) — traceability
  name        VARCHAR(64)      NOT NULL,   -- displayName
  description VARCHAR(500)     NULL,

  PRIMARY KEY (roster_id),
  UNIQUE KEY uq_class_content (content_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- class_race_limit — the class `race_limits` content gate (#696). One row per
-- (class, permitted race) pair, BOTH keyed by roster_id (character.class /
-- character.race). mcc emit-sql resolves each class's race_limits[] refs to race
-- roster_ids and emits a row per permitted race. worldd loads these into the
-- runtime Roster; character CREATE refuses a race NOT permitted for its class.
--
-- SEMANTICS: a class with NO rows here permits ALL races (empty/omitted
-- race_limits = all races, per class.schema.yaml) — the ABSENCE of rows is the
-- "no gate" signal, so a class is never required to enumerate every race. A
-- class WITH rows permits only the races listed.
-- ---------------------------------------------------------------------
CREATE TABLE class_race_limit (
  class_roster_id TINYINT UNSIGNED NOT NULL,  -- class.roster_id (character.class)
  race_roster_id  TINYINT UNSIGNED NOT NULL,  -- permitted race.roster_id (character.race)

  PRIMARY KEY (class_roster_id, race_roster_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
