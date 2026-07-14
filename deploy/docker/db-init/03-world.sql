-- SPDX-License-Identifier: Apache-2.0
-- Project Meridian compose DB init (#177) — world schema.
--
-- The world DB is NOT migration-managed: it is a read-only, mcc-produced
-- content artifact whose hand-maintained DDL lives in schema/sql/world/ (server
-- SAD §4.3; that dir's README). It is replaced wholesale by the nightly build.
-- For local/dev bring-up we load the DDL so the `meridian_world` DB and its
-- (empty) template tables exist; a real deploy points worldd at an mcc-emitted
-- world DB. The DDL files are numbered (00_manifest .. 90_gossip) and SOURCEd in
-- that order; they do not CREATE/USE a database. Dir mounted ro at /schemas/world.
--
-- ⛔ Adding a numbered DDL file to schema/sql/world/? SOURCE it here too — CI gate
-- tests/test_db_init_migration_coverage.py enforces this wrapper covers them all.
CREATE DATABASE IF NOT EXISTS meridian_world
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE meridian_world;
SOURCE /schemas/world/00_manifest.sql;
SOURCE /schemas/world/10_npc.sql;
SOURCE /schemas/world/20_item.sql;
SOURCE /schemas/world/30_ability.sql;
SOURCE /schemas/world/35_roster.sql;
SOURCE /schemas/world/40_quest.sql;
SOURCE /schemas/world/50_loot.sql;
SOURCE /schemas/world/60_vendor.sql;
SOURCE /schemas/world/70_spawn.sql;
SOURCE /schemas/world/80_zone.sql;
SOURCE /schemas/world/90_gossip.sql;
