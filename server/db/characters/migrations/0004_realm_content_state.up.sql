-- =====================================================================
-- Project Meridian — characters DB migration 0004 (UP): realm_content_state
-- Serves: docs/superpowers/specs/2026-07-13-sp2-kernel-class-character-system-
--         design.md §2.5 (boot-time compat/migration gate) + the umbrella
--         2026-07-13-moddable-theme-platform-design.md §6.5 (a realm records
--         the pack compatibility_version it booted with; a breaking change
--         refuses to boot until an operator migrates). Story #698 (SP2.8).
--
-- WHY THE CHARACTERS DB (and NOT the world DB): the boot gate compares the
-- LOADED pack (this compile's compatibility_version, which mcc stamps into the
-- world DB `world_manifest`) against the realm's PERSISTED state (the version
-- the realm last successfully booted with). That persisted marker MUST survive
-- a content reload — but the world DB is an mcc-produced, read-only artifact
-- REPLACED WHOLESALE on every nightly build (schema/sql/world/*.sql header;
-- 0001_init_characters.up.sql "CONTRAST WITH world DB"). Persisting the marker
-- there would make it reset to the loaded value on every load and the gate a
-- no-op. The characters DB is the opposite — SERVER-MANAGED, WRITTEN AT
-- RUNTIME, durable across content swaps — so the realm's compatibility marker
-- lives here, alongside the other durable per-realm state worldd owns.
--
-- One row PER PACK NAMESPACE (mirroring world_manifest's one-row-per-pack
-- granularity): a realm can load a multi-pack content set and each pack carries
-- its own append-only/compatibility contract. worldd upserts a row on every
-- SAFE boot (a fresh pack, or a pack whose loaded compatibility_version equals
-- the recorded one — additive changes never bump it). On a BREAKING change
-- (loaded compatibility_version differs from the recorded one) worldd REFUSES
-- to boot and does NOT touch the row; the operator migration tool (out of scope
-- for #698) advances the recorded version after migrating character data.
--
-- All numeric/string columns are plain server-managed values (no IF-9 numeric
-- id, no AUTO_INCREMENT surrogate) — pack_namespace is the natural key, exactly
-- as in world_manifest. No cross-DB FK (§4.4): pack_namespace is a soft name
-- shared with the world DB world_manifest, not an enforced reference.
-- Reversible — see 0004_realm_content_state.down.sql.
-- =====================================================================

SET NAMES utf8mb4;

CREATE TABLE realm_content_state (
  pack_namespace         VARCHAR(32)  NOT NULL,               -- pack.namespace (soft name; matches world_manifest)
  compatibility_version  INT UNSIGNED NOT NULL,               -- the compatibility_version this realm booted with
  content_hash           CHAR(64)     NOT NULL,               -- BLAKE3 of the pack this realm last booted (context/forensics)
  pack_version           VARCHAR(32)  NOT NULL,               -- pack.version (semver) last booted (context)
  recorded_at            DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
                                      ON UPDATE CURRENT_TIMESTAMP,  -- when worldd last recorded this pack's state
  PRIMARY KEY (pack_namespace)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
