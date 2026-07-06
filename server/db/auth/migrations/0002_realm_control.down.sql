-- =====================================================================
-- Project Meridian — auth DB (migration 0002, DOWN): drop realm_control
-- Reverses 0002_realm_control.up.sql.
-- =====================================================================

SET NAMES utf8mb4;

DROP TABLE IF EXISTS realm_control;
