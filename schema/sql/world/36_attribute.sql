-- =====================================================================
-- File 36: attribute framework — base vocabulary + class/race mods (SP2.4 #694)
-- Mirrors: schema/content/attribute.schema.yaml,
--          schema/content/common.defs.yaml $defs/attributeMods (on race/class)
-- =====================================================================
--
-- The kernel-blessed base attribute VOCABULARY (meridian/attribute@1) plus the
-- per-class and per-race `attribute_mods` operators tune (SP2 design §2.3). mcc
-- emit-sql walks the `attribute` content entities and each race/class's
-- attribute_mods and fills these tables; worldd loads them at boot into a runtime
-- meridian::worldd::AttributeCatalog (effective_stats.h) that computes a
-- character's EFFECTIVE stats = base + class mods + race mods + the live
-- buff/debuff layer. The kernel formulas + the buff/debuff primitives read/write
-- effective stats through that framework.
--
-- KEY SCHEME: unlike the numeric-id-keyed content tables, these are keyed by the
-- attribute's contentId REF string (`attr_ref`, e.g. 'core:attribute.strength').
-- That is the SAME verbatim ref the ability effect payload carries for a
-- buff/debuff `attribute` (schema/sql/world/30_ability.sql effects_json) and the
-- SAME key the runtime aura ledger uses — so every layer (pack mods, buff/debuff
-- auras, the effective-stat framework) joins on one string with no id round-trip.
-- The IF-9 numeric id is still recorded in `content_id` (UNIQUE) for traceability
-- back to the pack entity. The mods key on the canonical roster_id (character.race
-- / character.class), matching the `race`/`class` roster tables (35_roster.sql).
--
-- SCOPE: kernel owns the base attribute set (operators reference & tune, they do
-- not add primaries — new attributes are a later extension, umbrella §5). A race
-- or class with no attribute_mods (the chibi races zero theirs) simply has no rows
-- in its mod table.

-- ---------------------------------------------------------------------
-- attribute — one base attribute (meridian/attribute@1). `kind` splits the
-- primaries (strength/agility/stamina/intellect/spirit — also on the StatKey
-- layer) from the deriveds (crit/haste/armor — framework-only).
-- ---------------------------------------------------------------------
CREATE TABLE attribute (
  attr_ref    VARCHAR(64)  NOT NULL,            -- contentId ref ('core:attribute.strength')
  content_id  INT UNSIGNED NOT NULL,            -- IF-9 numeric id (idmap) — traceability
  name        VARCHAR(64)  NOT NULL,            -- displayName
  kind        ENUM('primary','derived') NOT NULL,

  PRIMARY KEY (attr_ref),
  UNIQUE KEY uq_attribute_content (content_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- class_attribute_mod — a class's flat per-attribute adjustment ($defs/
-- attributeMods on meridian/class@1). `value` may be negative.
-- ---------------------------------------------------------------------
CREATE TABLE class_attribute_mod (
  class_roster_id TINYINT UNSIGNED NOT NULL,    -- class.roster_id (character.class)
  attr_ref        VARCHAR(64)      NOT NULL,    -- attribute.attr_ref
  value           INT              NOT NULL,    -- signed flat adjustment

  PRIMARY KEY (class_roster_id, attr_ref),
  CONSTRAINT fk_classmod_class FOREIGN KEY (class_roster_id) REFERENCES class (roster_id),
  CONSTRAINT fk_classmod_attr  FOREIGN KEY (attr_ref)        REFERENCES attribute (attr_ref)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- race_attribute_mod — a race's flat per-attribute adjustment ($defs/
-- attributeMods on meridian/race@1). The chibi seed races carry none.
-- ---------------------------------------------------------------------
CREATE TABLE race_attribute_mod (
  race_roster_id TINYINT UNSIGNED NOT NULL,     -- race.roster_id (character.race)
  attr_ref       VARCHAR(64)      NOT NULL,     -- attribute.attr_ref
  value          INT              NOT NULL,     -- signed flat adjustment

  PRIMARY KEY (race_roster_id, attr_ref),
  CONSTRAINT fk_racemod_race FOREIGN KEY (race_roster_id) REFERENCES race (roster_id),
  CONSTRAINT fk_racemod_attr FOREIGN KEY (attr_ref)       REFERENCES attribute (attr_ref)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
