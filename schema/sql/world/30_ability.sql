-- =====================================================================
-- File 30: abilities / spells (players AND mobs)
-- Mirrors: schema/content/ability.schema.yaml (CMB-01, CMB-04)
-- =====================================================================

-- ---------------------------------------------------------------------
-- ability
--   Mirrors ability.schema.yaml. Flattening:
--     cast.*      -> cast_time_ms / cast_channel_ms
--     resource.*  -> resource_type / resource_amount
--     audio_visual.* -> av_* (client-facing anim + asset refs)
--     effects[]   -> effects_json (generic canonical-JSON payload; SP2.1)
--
--   SP2.1 (kernel ability engine): the ordered `effects[]` recipe is carried as a
--   single generic `effects_json` payload instead of exploded into per-kind
--   relational columns. The kernel deserializes the payload into a runtime
--   AbilityEffect tagged union covering the FULL effect-primitive palette
--   (damage/heal/aura/threat + dot/hot/buff/debuff/shield/cc/resource/movement/
--   summon). A new effect kind is now a schema + engine change, never a world-DDL
--   migration — the DB stays a transport for pack data rather than a rigid second
--   schema (SP2 design §2.1). This retired the restrictive per-kind `kind` ENUM
--   and the `ability_effect` / `ability_effect_stat_mod` child tables (the aura
--   stat mods fold into the effect payload), which is what unblocked the richer
--   palette the ability *schema* gained in #653 (the SP1.8 world-DB gap).
--
--   effects_json is authored-canonical JSON: the `effects[]` array with object
--   keys recursively sorted (deterministic byte order so content_hash/golden stay
--   stable), emitted by `mcc emit-sql`. `intRange` amounts serialize as
--   {"min":N,"max":M}; every field keeps its schema name (NOT the old flattened
--   column names). NOT NULL — the schema requires >=1 effect, so the array is
--   always present and non-empty. MariaDB JSON is LONGTEXT + a JSON_VALID CHECK,
--   so a malformed payload is rejected at load.
-- ---------------------------------------------------------------------
CREATE TABLE ability (
  id               INT UNSIGNED NOT NULL,                   -- IF-9 numeric id
  name             VARCHAR(80)  NOT NULL,                   -- displayName
  description      VARCHAR(500) NULL,
  school           ENUM('physical','fire','frost','nature','shadow','holy','arcane') NOT NULL,
  target           ENUM('self','enemy','friendly') NOT NULL,
  range_m          FLOAT NOT NULL DEFAULT 5,

  -- cast.*
  cast_time_ms     INT UNSIGNED NULL DEFAULT 0,             -- 0 = instant
  cast_channel_ms  INT UNSIGNED NULL,

  cooldown_ms      INT UNSIGNED NOT NULL DEFAULT 0,
  triggers_gcd     BOOLEAN NOT NULL DEFAULT TRUE,

  -- resource.* (optional sub-object; both required together when present)
  resource_type    ENUM('mana','rage','energy') NULL,
  resource_amount  INT UNSIGNED NULL,

  -- audio_visual.* (client-facing; asset refs kept as numeric ids)
  av_cast_anim     VARCHAR(64)  NULL,
  av_cast_vfx_id   INT UNSIGNED NULL,
  av_cast_sfx_id   INT UNSIGNED NULL,
  av_impact_vfx_id INT UNSIGNED NULL,
  av_impact_sfx_id INT UNSIGNED NULL,

  -- effects[] -> generic canonical-JSON recipe (SP2.1; see header note).
  effects_json     JSON NOT NULL,

  PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
