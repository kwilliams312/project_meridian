-- SPDX-License-Identifier: Apache-2.0
-- Project Meridian compose DB init (#177) — characters schema.
--
-- Creates the characters DB and applies its UP migrations in order (down
-- rollbacks skipped). Migration dir mounted read-only at /schemas/characters.
--
-- ⛔ Adding a migration? SOURCE it here too. This list is hand-maintained (MariaDB
-- SOURCE cannot glob a directory), so a new server/db/characters/migrations/*.up.sql
-- that is NOT wired in below ships a half-seeded `character` table to every hosted
-- realm — exactly the #479 regression (0003 was missed, so deploy DBs had no
-- `appearance` column and worldd's CHAR_LIST silently emptied). CI gate:
-- tests/test_db_init_migration_coverage.py fails if this wrapper does not SOURCE
-- every *.up.sql present in the migrations dir.
CREATE DATABASE IF NOT EXISTS meridian_characters
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE meridian_characters;
SOURCE /schemas/characters/0001_init_characters.up.sql;
SOURCE /schemas/characters/0002_character_mute.up.sql;
SOURCE /schemas/characters/0003_appearance_dyes.up.sql;
