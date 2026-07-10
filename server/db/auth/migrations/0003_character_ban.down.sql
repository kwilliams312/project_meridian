-- =====================================================================
-- Project Meridian — auth DB (migration 0003, DOWN): drop character_ban.
-- Reverses 0003_character_ban.up.sql.
-- =====================================================================

DROP TABLE IF EXISTS character_ban;
