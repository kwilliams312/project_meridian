-- =====================================================================
-- Project Meridian — characters DB schema v1 (migration 0001, DOWN)
-- Reverses 0001_init_characters.up.sql: drops exactly what UP created.
--
-- Drop order is the reverse of create order so foreign keys never block a
-- DROP: children before parents. character_inventory and item_instance both
-- reference `character` (and inventory references item_instance), so the leaf
-- tables (auras, talents, spells, quests, inventory) drop first, then
-- item_instance, then `character` last. IF EXISTS keeps the rollback idempotent.
-- =====================================================================

SET NAMES utf8mb4;

DROP TABLE IF EXISTS character_aura;
DROP TABLE IF EXISTS character_talent;
DROP TABLE IF EXISTS character_spell;
DROP TABLE IF EXISTS character_quest;
DROP TABLE IF EXISTS character_inventory;
DROP TABLE IF EXISTS item_instance;
DROP TABLE IF EXISTS `character`;
