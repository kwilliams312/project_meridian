-- =====================================================================
-- Project Meridian — auth DB (migration 0002, UP): realm_control (M3)
-- Serves: docs/sad/server-sad.md §4.1 (realm_control), §8.6 (coordinator
--         failover — warm standby + DB-fenced leadership).
--
-- Separate migration (not folded into 0001) because realm_control lands at M3,
-- after the M0 auth base — the migration sequence is the timeline of the DB.
--
-- realm_control holds the coordinator leader lease + fencing epoch. One row per
-- realm (realm_id PK, 1:1 with realm). The leader renews lease_expires_at every
-- 2 s by CAS on coord_epoch; the standby claims on lease expiry (6 s) by
-- CAS-incrementing coord_epoch. The CAS on coord_epoch is what makes
-- split-brain impossible at the arbiter (§8.6). Updated by CAS only, e.g.:
--   UPDATE realm_control
--      SET coord_epoch = ?, leader_id = ?, lease_expires_at = ?
--    WHERE realm_id = ? AND coord_epoch = ?;   -- expected-current epoch
-- 1 row => won/renewed; 0 rows => a newer epoch exists, back off.
--
-- realm_id FK -> realm(id) ON DELETE CASCADE: the control row is meaningless
-- without its realm.
-- =====================================================================

SET NAMES utf8mb4;

CREATE TABLE realm_control (
  realm_id          INT UNSIGNED     NOT NULL,                 -- 1:1 with realm.id
  coord_epoch       BIGINT           NOT NULL DEFAULT 0,       -- fencing epoch; monotonically CAS-incremented on takeover
  leader_id         VARCHAR(64)      NULL,                     -- current leader coordd node id; NULL = no leader yet
  lease_expires_at  DATETIME         NULL,                     -- lease deadline; expiry (6 s) => standby may claim
  created_at        DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at        DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (realm_id),
  CONSTRAINT fk_realm_control_realm
    FOREIGN KEY (realm_id) REFERENCES realm (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
