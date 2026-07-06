-- =====================================================================
-- File 70: spawn placements & patrols
-- Mirrors: schema/content/spawn.schema.yaml (NPC-01)
-- A spawn file groups spawns under one zone; each spawns[] item becomes a
-- spawn_point row. position.{x,y,z} is flattened to pos_x/y/z (zone-local
-- meters, permanent per D-20). respawn_seconds (intRange) -> _min/_max.
-- =====================================================================

-- ---------------------------------------------------------------------
-- spawn_point — one placed creature.
--   wander_radius_m and patrol are mutually exclusive (schema `not`):
--   a row has either wander_radius_m set OR a patrol_path row, never both.
--   The numeric PK (id) is assigned by IF-9 to the placement itself
--   (spawn files carry a file-level id; per-spawn ids are minted by mcc).
-- ---------------------------------------------------------------------
CREATE TABLE spawn_point (
  id               INT UNSIGNED NOT NULL,                   -- IF-9 numeric id (placement)
  zone_ref_id      INT UNSIGNED NOT NULL,                   -- file-level zone (zoneRef) -> zone.id
  npc_id           INT UNSIGNED NOT NULL,                   -- spawns[].npc (npcRef)
  pos_x            FLOAT NOT NULL,                           -- position.x (zone-local m)
  pos_y            FLOAT NOT NULL,                           -- position.y
  pos_z            FLOAT NOT NULL,                           -- position.z
  orientation_deg  FLOAT NOT NULL DEFAULT 0,                -- [0,360)
  respawn_min      INT UNSIGNED NOT NULL,                   -- respawn_seconds (intRange)
  respawn_max      INT UNSIGNED NOT NULL,
  wander_radius_m  FLOAT NULL,                              -- XOR with a patrol_path row
  PRIMARY KEY (id),
  CONSTRAINT fk_spawn_zone FOREIGN KEY (zone_ref_id) REFERENCES zone (id),
  CONSTRAINT fk_spawn_npc  FOREIGN KEY (npc_id)      REFERENCES npc_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- patrol_path — spawns[].patrol (one per patrolling spawn_point).
--   loop=true cycle; false ping-pong. Waypoints in patrol_waypoint.
-- ---------------------------------------------------------------------
CREATE TABLE patrol_path (
  spawn_point_id  INT UNSIGNED NOT NULL,                    -- -> spawn_point.id (1:1)
  `loop`          BOOLEAN NOT NULL DEFAULT TRUE,            -- true=cycle, false=ping-pong
  PRIMARY KEY (spawn_point_id),
  CONSTRAINT fk_patrol_spawn FOREIGN KEY (spawn_point_id) REFERENCES spawn_point (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- patrol_waypoint — patrol.waypoints[] (>=2), ordered.
--   position.{x,y,z} flattened; wait_seconds is the dwell at the waypoint.
-- ---------------------------------------------------------------------
CREATE TABLE patrol_waypoint (
  spawn_point_id  INT UNSIGNED NOT NULL,                    -- -> patrol_path.spawn_point_id
  ordinal         SMALLINT UNSIGNED NOT NULL,               -- waypoint order
  pos_x           FLOAT NOT NULL,                           -- position.x
  pos_y           FLOAT NOT NULL,                           -- position.y
  pos_z           FLOAT NOT NULL,                           -- position.z
  wait_seconds    FLOAT NOT NULL DEFAULT 0,
  PRIMARY KEY (spawn_point_id, ordinal),
  CONSTRAINT fk_waypoint_patrol FOREIGN KEY (spawn_point_id) REFERENCES patrol_path (spawn_point_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
