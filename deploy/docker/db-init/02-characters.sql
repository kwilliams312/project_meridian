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
SOURCE /schemas/characters/0004_realm_content_state.up.sql;

-- Migration tracking (#815). This FRESH-init path applies every *.up.sql above,
-- so it also OWNS first-init of the tracker: create schema_migrations and record
-- each version it just applied. The deploy-time character-DB migration runner
-- (deploy/docker/char-migrate.sh) then finds a fully-tracked DB and is a NO-OP
-- here — it only does work on a PERSISTENT datadir that predates a later
-- migration (where this wrapper never re-runs). Keep this VALUES list in lockstep
-- with the SOURCEd migrations above; the CI guard
-- tests/test_db_init_migration_coverage.py fails if they drift.
CREATE TABLE IF NOT EXISTS schema_migrations (
  version    VARCHAR(255) NOT NULL,
  applied_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
INSERT IGNORE INTO schema_migrations (version) VALUES
  ('0001'),
  ('0002'),
  ('0003'),
  ('0004');
