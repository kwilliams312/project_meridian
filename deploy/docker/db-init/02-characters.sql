-- SPDX-License-Identifier: Apache-2.0
-- Project Meridian compose DB init (#177) — characters schema.
--
-- Creates the characters DB and applies its UP migrations in order (down
-- rollbacks skipped). Migration dir mounted read-only at /schemas/characters.
CREATE DATABASE IF NOT EXISTS meridian_characters
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE meridian_characters;
SOURCE /schemas/characters/0001_init_characters.up.sql;
