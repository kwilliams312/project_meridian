-- =====================================================================
-- Project Meridian — world DB DDL v1 (IF-4)
-- File 00: manifest + shared conventions
--
-- Owner: Tools track (mcc emit-sql fills these tables); this DDL is
-- hand-maintained and co-reviewed with the Server track (worldd reads it).
-- Source of truth: docs/sad/tools-sad.md §2.6, docs/sad/server-sad.md §4.3.
--
-- DIALECT ...... MariaDB / MySQL, InnoDB, utf8mb4 (worldd's DB, TD-05).
-- KEY SCHEME ... Every numeric primary key is a uint32 assigned by IF-9
--                (idmap.lock), NOT the database. Therefore:
--                  * PKs are INT UNSIGNED with NO AUTO_INCREMENT.
--                  * String content IDs (core:npc.foo) are NEVER stored as
--                    keys; they live only in idmap.lock. mcc bakes the
--                    numeric id into every row and every ref column.
-- REFS ......... Foreign keys WITHIN the world DB where they add value.
--                References into the characters DB are numeric-only with
--                NO cross-DB FK (server-sad §4.4).
-- LOAD ......... world is read-only at runtime; replaced wholesale by the
--                nightly build. worldd loads it into immutable in-memory
--                template stores and never writes it.
-- =====================================================================

-- Session/connection defaults the build applies before running this DDL.
SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;   -- tables are declared in dependency order, but
                              -- the wholesale reload drops/creates in bulk.

-- ---------------------------------------------------------------------
-- world_manifest — boot handshake + three-way content-hash tie
--   Serves: docs/sad/tools-sad.md §2.6, server-sad §4.3 (boot handshake),
--           §4.6 (per-namespace ID bands / TLS-08 pack-band validation).
--   One row per content pack merged into this DB. worldd reads every row at
--   boot, refuses to start on a schema_version mismatch, and logs/propagates
--   content_hash (sent in HandshakeOk so client .pck and server data are
--   provably the same compile).
--
--   IF-4 PRODUCER SIDE: worldd's boot-time READ + VERIFY of these columns is
--   implemented (server/worldd/world_boot.*), and mcc `emit-sql` populates this
--   table (one INSERT per pack with the BLAKE3 content_hash, pack_version,
--   id_band, schema_version, compatibility_version, mcc_version, built_at).
--   worldd reads exactly these EIGHT columns; mcc must emit exactly these eight
--   columns. The content-schema major mcc stamps here MUST equal worldd's
--   kSupportedContentSchemaVersion (currently 1 — the world DDL v1).
--
--   compatibility_version (SP2.8 #698): the pack CONTRACT version, distinct from
--   the semver pack_version — it bumps ONLY on a breaking (non-additive) id
--   change (a removed/renumbered id), never on additive edits (schema/content/
--   pack.schema.yaml `compatibility_version`; `mcc diff` classifies + enforces
--   the bump). worldd's boot-time compat gate compares THIS loaded value against
--   the realm's persisted `realm_content_state.compatibility_version` (characters
--   DB) and refuses to boot on a breaking change until an operator migrates. An
--   absent pack value defaults to the baseline 1 (mcc stamps 1).
-- ---------------------------------------------------------------------
CREATE TABLE world_manifest (
  pack_namespace         VARCHAR(32)  NOT NULL,           -- pack.namespace; owns an ID band
  pack_version           VARCHAR(32)  NOT NULL,           -- pack.version (semver)
  id_band                INT UNSIGNED NOT NULL,           -- IF-9 numeric band base for this pack
  content_hash           CHAR(64)     NOT NULL,           -- BLAKE3 of the pack's canonical source tree (hex)
  schema_version         INT UNSIGNED NOT NULL,           -- content schema major (pack.content_schema_version)
  compatibility_version  INT UNSIGNED NOT NULL DEFAULT 1, -- pack contract version (pack.compatibility_version); boot compat gate (#698)
  mcc_version            VARCHAR(32)  NOT NULL,           -- compiler version that produced this row
  built_at               DATETIME     NOT NULL,           -- build timestamp (server-sad §4.3 manifest field)
  PRIMARY KEY (pack_namespace)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
