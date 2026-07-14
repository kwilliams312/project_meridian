-- =====================================================================
-- Project Meridian — characters DB migration 0004 (DOWN): realm_content_state
-- Reverses 0004_realm_content_state.up.sql: drops the table UP created.
--
-- The table has no incoming keys (no other table references it) and no
-- generated columns, so the DROP is self-contained and restores the 0003 state.
-- Dropping it disables the boot compat gate (worldd treats an absent table /
-- empty state as a fresh realm on the next boot and re-records).
-- =====================================================================

SET NAMES utf8mb4;

DROP TABLE IF EXISTS realm_content_state;
