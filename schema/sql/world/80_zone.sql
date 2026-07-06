-- =====================================================================
-- File 80: zones, areas, graveyards, navmesh refs
-- Mirrors: schema/content/zone.schema.yaml (WLD-01, WLD-03, AUD-02, CMB-03)
-- =====================================================================

-- ---------------------------------------------------------------------
-- zone — zone manifest.
--   level_range (intRange) -> level_min/max. music.* flattened. ambience
--   is an ambRef. chunk_manifest is RESERVED (A-08, type:null) — OMITTED
--   from columns until the IF-6 chunk-format contract is signed (see README
--   + navmesh_ref below). music/ambience asset refs kept as numeric ids.
-- ---------------------------------------------------------------------
CREATE TABLE zone (
  id                INT UNSIGNED NOT NULL,                  -- IF-9 numeric id
  name              VARCHAR(80)  NOT NULL,                  -- displayName
  level_min         SMALLINT UNSIGNED NOT NULL,             -- level_range (intRange)
  level_max         SMALLINT UNSIGNED NOT NULL,
  start_zone        BOOLEAN NOT NULL DEFAULT FALSE,         -- new characters may start here
  music_explore_id  INT UNSIGNED NULL,                      -- music.explore (musRef; required if music present)
  music_tension_id  INT UNSIGNED NULL,                      -- music.tension (musRef)
  music_combat_id   INT UNSIGNED NULL,                      -- music.combat (musRef)
  ambience_id       INT UNSIGNED NULL,                      -- ambience (ambRef)
  PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- area — points of interest, pois[] (WLD-03).
--   Map markers, discovery XP, explore-objective targets. `poi` id is a
--   zone-local string (matches quest explore objectives / spawn poi refs);
--   it is NOT an IF-9 global id, so it is stored as text keyed within zone.
--   position.{x,y,z} flattened.
--   (server-sad §4.3 names this family `area`; v1 field-set = zone.pois[].)
-- ---------------------------------------------------------------------
CREATE TABLE area (
  zone_id             INT UNSIGNED NOT NULL,                -- -> zone.id
  poi                 VARCHAR(64)  NOT NULL,                -- pois[].id (zone-local string)
  name                VARCHAR(80)  NOT NULL,                -- pois[].name (displayName)
  pos_x               FLOAT NOT NULL,                       -- position.x
  pos_y               FLOAT NOT NULL,                       -- position.y
  pos_z               FLOAT NOT NULL,                       -- position.z
  discovery_radius_m  FLOAT NOT NULL DEFAULT 40,
  discovery_xp        INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (zone_id, poi),
  CONSTRAINT fk_area_zone FOREIGN KEY (zone_id) REFERENCES zone (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- graveyard — graveyards[] (CMB-03 resurrection points).
--   First entry per zone is the zone default (ordinal 0). position.{x,y,z}
--   flattened; orientation_deg is the res-facing.
-- ---------------------------------------------------------------------
CREATE TABLE graveyard (
  zone_id          INT UNSIGNED NOT NULL,                   -- -> zone.id
  ordinal          SMALLINT UNSIGNED NOT NULL,              -- array position (0 = zone default)
  pos_x            FLOAT NOT NULL,                          -- position.x
  pos_y            FLOAT NOT NULL,                          -- position.y
  pos_z            FLOAT NOT NULL,                          -- position.z
  orientation_deg  FLOAT NOT NULL DEFAULT 0,                -- [0,360)
  PRIMARY KEY (zone_id, ordinal),
  CONSTRAINT fk_graveyard_zone FOREIGN KEY (zone_id) REFERENCES zone (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- area_trigger — server-sad §4.3 table family (M1+).
--   NOT YET SCHEMA-DRIVEN: zone.schema.yaml v1 has no trigger field-set.
--   Declared now so worldd's loader and the manifest schema_version are
--   stable; mcc leaves it empty until the trigger content schema lands.
--   Box/sphere volume in zone-local meters; on_enter action is opaque v1.
-- ---------------------------------------------------------------------
CREATE TABLE area_trigger (
  id            INT UNSIGNED NOT NULL,                      -- IF-9 numeric id
  zone_id       INT UNSIGNED NOT NULL,                      -- -> zone.id
  pos_x         FLOAT NOT NULL,
  pos_y         FLOAT NOT NULL,
  pos_z         FLOAT NOT NULL,
  radius_m      FLOAT NULL,                                 -- sphere trigger
  box_x         FLOAT NULL,                                 -- box half-extents (alt to radius)
  box_y         FLOAT NULL,
  box_z         FLOAT NULL,
  PRIMARY KEY (id),
  CONSTRAINT fk_atrigger_zone FOREIGN KEY (zone_id) REFERENCES zone (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- navmesh_ref — server-facing slice of IF-6 chunk data (server-sad §4.5/§4.6).
--   NOT YET SCHEMA-DRIVEN: zone.chunk_manifest is RESERVED until A-08. mcc
--   bakes per-chunk navmesh tile references here; the server never parses
--   Forge scene files. Declared now so the schema_version is stable.
-- ---------------------------------------------------------------------
CREATE TABLE navmesh_ref (
  zone_id       INT UNSIGNED NOT NULL,                      -- -> zone.id
  chunk_x       INT NOT NULL,                               -- chunk grid coordinate
  chunk_y       INT NOT NULL,
  tile_ref      VARCHAR(128) NOT NULL,                      -- baked navmesh tile reference (IF-6)
  PRIMARY KEY (zone_id, chunk_x, chunk_y),
  CONSTRAINT fk_navmesh_zone FOREIGN KEY (zone_id) REFERENCES zone (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
