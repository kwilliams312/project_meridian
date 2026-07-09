-- =====================================================================
-- Project Meridian — auth DB (migration 0003, UP): character_ban (M1)
-- Serves: docs/sad/server-sad.md §4.1 (bans live in the auth DB), server PRD
--         §4-M1 (OPS-02 moderation), story #419 (account/character/IP bans).
--
-- Separate migration (not folded into 0001) because character-level bans land
-- with the OPS-02c moderation story, after the M0 auth base — the migration
-- sequence is the timeline of the DB. The account_ban and ip_ban tables were
-- already defined in 0001; this adds the third ban subject: an individual
-- CHARACTER (a ban that blocks entering the world with one character without
-- locking the whole account).
--
-- character_id is a SOFT numeric reference into the characters DB
-- (characters.character.id) with NO cross-DB FK (§4.4 — a cross-DB FK is
-- impossible in MariaDB and forbidden). banned_by IS a within-auth-DB FK to
-- account(id) (ON DELETE SET NULL — a purged GM account leaves the ban intact,
-- only the attribution is nulled), mirroring account_ban / ip_ban.
--
-- A NULL expires_at is a permanent ban; history is retained (multiple rows per
-- character are allowed). Enforced in worldd at ENTER_WORLD (the session already
-- loaded + proved ownership of the character), refusing the spawn.
-- =====================================================================

SET NAMES utf8mb4;

CREATE TABLE character_ban (
  id            BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  character_id  BIGINT UNSIGNED  NOT NULL,                     -- SOFT ref -> characters DB character.id (NO cross-DB FK, §4.4)
  expires_at    DATETIME         NULL,                         -- NULL = permanent
  reason        VARCHAR(255)     NOT NULL,
  banned_by     BIGINT UNSIGNED  NULL,                         -- acting GM account.id; NULL = system/automated
  created_at    DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_character_ban_character (character_id),
  KEY idx_character_ban_expires (expires_at),
  CONSTRAINT fk_character_ban_banned_by
    FOREIGN KEY (banned_by) REFERENCES account (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
