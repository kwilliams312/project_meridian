-- =====================================================================
-- Project Meridian — auth DB schema v1 (migration 0001, UP)
-- Serves: docs/sad/server-sad.md §4.1 (auth DB field list — authoritative),
--         §2.1 (authd), §5.3 (IF-3 session grant).
--
-- Owner: authd (SRP6a login, realm list, session-grant writer). gatewayd
-- reads/consumes grants and enforces bans (§2.2, §5.3); coordd heartbeats
-- the realm row and holds the realm_control lease (§2.1, §8.6) from M3.
--
-- CONTRAST WITH world DB (schema/sql/world/): the world DB is an mcc-produced,
-- read-only artifact replaced wholesale nightly, keyed by IF-9 uint32s with NO
-- AUTO_INCREMENT. The auth DB is the opposite: SERVER-MANAGED and WRITTEN AT
-- RUNTIME (accounts created, bans added, grants written/consumed/expired,
-- realm heartbeats, lease CAS). Therefore this schema uses:
--   * AUTO_INCREMENT surrogate PKs where the key is server-minted (account.id,
--     account_ban.id, ip_ban.id, realm.id);
--   * an explicitly-supplied random u64 PK for session_grant.grant_id
--     (NOT AUTO_INCREMENT — see below);
--   * created_at / updated_at audit timestamps on the mutable entities;
--   * reversible, numbered migrations (this DB evolves — 0001 is the M0 base).
--
-- DIALECT ...... MariaDB / MySQL, ENGINE=InnoDB, utf8mb4 (TD-05), per-table
--                CHARSET/COLLATE declared explicitly for portability.
-- TIME ......... All timestamps are DATETIME in UTC (the server writes UTC);
--                created_at/updated_at use CURRENT_TIMESTAMP defaults so a raw
--                INSERT that omits them is still audit-correct.
-- =====================================================================

SET NAMES utf8mb4;

-- ---------------------------------------------------------------------
-- account — one row per login account (§4.1).
--   SRP6a verifier-based: the server stores {salt, verifier} ONLY, never a
--   password and never anything password-equivalent (§2.1). id is a
--   server-minted surrogate (BIGINT UNSIGNED AUTO_INCREMENT) — accounts are
--   created at runtime (ACC-03 registration), so a DB-assigned key is natural.
-- ---------------------------------------------------------------------
CREATE TABLE account (
  id            BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  username      VARCHAR(32)      NOT NULL,                     -- login name; case-insensitive UNIQUE via collation
  srp_salt      VARBINARY(32)    NOT NULL,                     -- SRP6a salt s
  srp_verifier  VARBINARY(256)   NOT NULL,                     -- SRP6a verifier v = g^x mod N (2048-bit group -> 256 bytes)
  gm_level      TINYINT UNSIGNED NOT NULL DEFAULT 0,           -- 0 player < helper < GM < admin (D-16 permission model)
  email         VARCHAR(254)     NULL,                         -- optional; RFC 5321 max local+domain length
  locked        TINYINT(1)       NOT NULL DEFAULT 0,           -- admin lock flag (distinct from a time-boxed ban)
  created_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  last_login    DATETIME         NULL,                         -- set by authd on a successful SRP proof
  PRIMARY KEY (id),
  UNIQUE KEY uq_account_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- account_ban — time-boxed or permanent ban on an account (§4.1).
--   Enforced in authd (login refusal) and gatewayd (grant refusal). A NULL
--   expires_at is a permanent ban. History is retained (multiple rows per
--   account are allowed); the active ban is the row with the latest/NULL
--   expires_at. ON DELETE CASCADE: a ban has no meaning without its account,
--   so purging an account purges its ban history.
-- ---------------------------------------------------------------------
CREATE TABLE account_ban (
  id          BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  account_id  BIGINT UNSIGNED  NOT NULL,                       -- target account (§4.1 "account/IP")
  expires_at  DATETIME         NULL,                           -- NULL = permanent
  reason      VARCHAR(255)     NOT NULL,
  banned_by   BIGINT UNSIGNED  NULL,                           -- acting GM account.id; NULL = system/automated
  created_at  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_account_ban_account (account_id),
  KEY idx_account_ban_expires (expires_at),
  CONSTRAINT fk_account_ban_account
    FOREIGN KEY (account_id) REFERENCES account (id) ON DELETE CASCADE,
  CONSTRAINT fk_account_ban_banned_by
    FOREIGN KEY (banned_by)  REFERENCES account (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- ip_ban — time-boxed or permanent ban on a client IP / CIDR (§4.1).
--   Checked at accept in authd (cached 60 s TTL, §2.1) and in gatewayd. The
--   target is stored as text (VARBINARY-free) to carry both IPv4 and IPv6,
--   optionally with a CIDR suffix (e.g. "203.0.113.0/24"). No account FK —
--   an IP ban is deliberately account-independent.
-- ---------------------------------------------------------------------
CREATE TABLE ip_ban (
  id          BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  target      VARCHAR(64)      NOT NULL,                       -- IPv4/IPv6 address or CIDR (§4.1 "account/IP")
  expires_at  DATETIME         NULL,                           -- NULL = permanent
  reason      VARCHAR(255)     NOT NULL,
  banned_by   BIGINT UNSIGNED  NULL,                           -- acting GM account.id; NULL = system/automated
  created_at  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_ip_ban_target (target),
  KEY idx_ip_ban_expires (expires_at),
  CONSTRAINT fk_ip_ban_banned_by
    FOREIGN KEY (banned_by) REFERENCES account (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- realm — realm-list row served by authd (§2.1, §4.1).
--   address = gateway address from M2. population is heartbeat-written per
--   worldd M0–M2, then aggregated and written by coordd from M3. build_min/max
--   express the accepted client_build range. id is a server-minted surrogate.
-- ---------------------------------------------------------------------
CREATE TABLE realm (
  id            INT UNSIGNED     NOT NULL AUTO_INCREMENT,
  name          VARCHAR(64)      NOT NULL,                     -- display name in the realm list
  address       VARCHAR(255)     NOT NULL,                     -- gateway host (M2+); hostname or IP literal
  port          SMALLINT UNSIGNED NOT NULL,                    -- TCP port (0..65535)
  build_min     INT UNSIGNED     NOT NULL,                     -- lowest accepted client_build
  build_max     INT UNSIGNED     NOT NULL,                     -- highest accepted client_build
  population    FLOAT            NOT NULL DEFAULT 0,           -- population bucket/aggregate for the list UI
  flags         INT UNSIGNED     NOT NULL DEFAULT 0,           -- bitfield: PvP/normal, locked, recommended, offline...
  heartbeat_at  DATETIME         NULL,                         -- last liveness write; stale => list shows offline
  created_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_realm_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- session_grant — IF-3 session handoff (§4.1, §5.3, §3.1).
--   Written by authd on realm select; consumed single-use by gatewayd from M2
--   (worldd in the M0–M1 --monolith build). Bound to {account, realm,
--   client_build}. 30 s expiry; an expiry sweep deletes stale rows.
--
--   grant_id is a RANDOM u64 supplied by authd, NOT AUTO_INCREMENT. Rationale:
--   the grant_id travels to the client and is presented back over the wire in
--   WorldHello (§3.1). A sequential AUTO_INCREMENT id would be guessable /
--   enumerable, letting an attacker race the legitimate client to consume a
--   grant. A cryptographically-random 64-bit id makes grants unguessable; the
--   session_key still gates the HMAC proof, so grant_id secrecy is defence in
--   depth, not the sole guard. Column is BIGINT UNSIGNED = full u64 range;
--   the app inserts the random value explicitly.
--
--   Single-use consume is one atomic UPDATE (§3.1):
--     UPDATE session_grant SET consumed_at = UTC_TIMESTAMP()
--       WHERE grant_id = ? AND consumed_at IS NULL AND expires_at > UTC_TIMESTAMP();
--   1 row => accept; 0 rows => reject + close.
--
--   FK ON DELETE CASCADE on both parents: a grant is meaningless without its
--   account and realm, and purging either should not strand grant rows. Grants
--   are short-lived and swept, so cascade churn is negligible.
-- ---------------------------------------------------------------------
CREATE TABLE session_grant (
  grant_id      BIGINT UNSIGNED  NOT NULL,                     -- random u64 minted by authd (NOT AUTO_INCREMENT)
  account_id    BIGINT UNSIGNED  NOT NULL,
  realm_id      INT UNSIGNED     NOT NULL,
  session_key   BINARY(32)       NOT NULL,                     -- 256-bit key material; never leaves authd/gatewayd (§5.3)
  client_build  INT UNSIGNED     NOT NULL,                     -- bound build; re-checked at handshake
  expires_at    DATETIME         NOT NULL,                     -- created + 30 s (§2.1)
  consumed_at   DATETIME         NULL,                         -- NULL = unconsumed; set once, atomically
  created_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (grant_id),
  KEY idx_session_grant_account (account_id),
  KEY idx_session_grant_realm (realm_id),
  KEY idx_session_grant_expires (expires_at),        -- expiry sweep scans by expires_at
  CONSTRAINT fk_session_grant_account
    FOREIGN KEY (account_id) REFERENCES account (id) ON DELETE CASCADE,
  CONSTRAINT fk_session_grant_realm
    FOREIGN KEY (realm_id)   REFERENCES realm (id)   ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
