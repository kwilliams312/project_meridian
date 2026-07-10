-- =====================================================================
-- Project Meridian — characters DB migration 0003 (DOWN): appearance + dyes
-- Reverses 0003_appearance_dyes.up.sql: drops exactly the two columns UP added.
--
-- Neither column is referenced by a key, view, or generated column, so the
-- DROPs are independent; order does not matter. This restores the 0001 base.
-- =====================================================================

SET NAMES utf8mb4;

ALTER TABLE item_instance DROP COLUMN dyes;
ALTER TABLE `character` DROP COLUMN appearance;
