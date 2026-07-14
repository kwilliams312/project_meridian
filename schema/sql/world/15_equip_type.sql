-- =====================================================================
-- File 15: equip-type catalog — armor & weapon type vocabulary (SP2.7 #697)
-- Mirrors: schema/content/equip_type.schema.yaml (meridian/equip_type@1)
-- =====================================================================
--
-- The data-driven armor/weapon TYPE catalog (Cloth/Leather/Mail/Plate,
-- Two-Hand/One-Hand/Wand/Staff). ONE table; the `category` column distinguishes
-- an armor type from a weapon type. Items reference an entry via
-- item_template.equip_type_id, and a class's usable_armor_types /
-- usable_weapon_types (class_usable_equip_type) reference the same catalog. The
-- kernel (worldd) loads this at boot and equip-gates against it (SP2 design §2.4):
-- an item's equip_type must be one the character's class may use, AND its category
-- must match its paperdoll slot family (the SP1-deferred category-match, closed at
-- runtime here).
--
-- KEYED by the IF-9 idmap numeric id (`content_id`), like every other content
-- family (item/ability/quest): mcc emit-sql bakes the numeric id in, and every
-- reference column (item_template.equip_type_id, class_usable_equip_type
-- .equip_type_id) carries the SAME numeric id. No cross-content FK (matching the
-- attribute-framework precedent — cross-references are validated by mcc/L011, not
-- FK-enforced in the world DB).

CREATE TABLE equip_type (
  content_id  INT UNSIGNED NOT NULL,                 -- IF-9 numeric id (idmap)
  equip_ref   VARCHAR(96)  NOT NULL,                 -- verbatim contentId (core:equip_type.plate)
  name        VARCHAR(64)  NOT NULL,                 -- displayName
  category    ENUM('armor','weapon') NOT NULL,       -- gates equipping by slot family
  slot_class  VARCHAR(32)  NULL,                     -- informational grouping (free-form token)

  PRIMARY KEY (content_id),
  UNIQUE KEY uq_equip_type_ref (equip_ref)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
