-- =====================================================================
-- Project Meridian — characters DB migration 0003 (UP): appearance + dyes
-- meridian:applied-probe column:character.appearance  -- deploy-time runner backfill sentinel (#815)
-- Serves: contract ① §5.2 (per-character appearance record) + §6 (reserved
--         dye state). Additive to the 0001 base — no existing column changes.
--
-- character.appearance — a versioned JSON record describing the character's
--   authored look: {"v":1,"hair":1,"face":1,"skin":1,"morphs":[]}. It is
--   OPAQUE to gameplay and never gameplay-authoritative (spec §9): the server
--   treats it as bounded data, defaulting an absent/NULL value to
--   {v:1,hair:1,face:1,skin:1}. JSON (MariaDB JSON = LONGTEXT alias, exactly
--   like character_quest.objective_counts) so the record can grow additive
--   keys — extra morphs, dye channels — with NO future migration (A-03/D-32).
--   NULL = never customised ⇒ the server materialises the versioned default.
--
-- item_instance.dyes — RESERVED per-item dye state, a JSON map
--   {"<channel>":"<dye content id>"}. Written when dye acquisition lands (M2);
--   NULL = the item wears its authored default colours. Reserved now (the
--   COLUMN belongs to the item row per the contract) even though the write
--   path is M2 — same "reserve the column at its milestone" pattern 0001 uses
--   for character.save_epoch.
--
-- Both are additive nullable columns: currently-deployed code that never names
-- them keeps working (zero-downtime), and a plain INSERT that omits them stores
-- NULL. Reversible — see 0003_appearance_dyes.down.sql.
-- =====================================================================

SET NAMES utf8mb4;

ALTER TABLE `character` ADD COLUMN appearance JSON NULL AFTER `class`;
ALTER TABLE item_instance ADD COLUMN dyes JSON NULL AFTER suffix_id;
