-- SPDX-License-Identifier: Apache-2.0
-- Project Meridian compose DB init (#177) — auth schema.
--
-- MariaDB's docker-entrypoint runs /docker-entrypoint-initdb.d/*.sql in
-- alphabetical order, ONCE, on first boot of an empty data volume. The auth
-- migrations (server/db/auth/migrations/*.up.sql) create tables but do NOT
-- CREATE/USE a database (they assume the connection is already on it), so this
-- wrapper creates the DB, selects it, and SOURCEs the UP migrations in order.
-- Only *.up.sql is applied — the *.down.sql rollbacks are deliberately skipped.
-- The migration dir is mounted read-only at /schemas/auth (see docker-compose.yml).
--
-- ⛔ Adding a migration? SOURCE it here too — this list is hand-maintained and a
-- missed file half-seeds the DB (the #479 class of bug). CI gate:
-- tests/test_db_init_migration_coverage.py enforces full coverage.
CREATE DATABASE IF NOT EXISTS meridian_auth
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE meridian_auth;
SOURCE /schemas/auth/0001_init_auth.up.sql;
SOURCE /schemas/auth/0002_realm_control.up.sql;
SOURCE /schemas/auth/0003_character_ban.up.sql;
