-- =====================================================================
-- Project Meridian — characters DB (migration 0002, UP): character_mute (M1)
-- Serves: docs/sad/server-sad.md §4.2 (per-character durable state), server PRD
--         §4-M1 (OPS-02 moderation), story #419 (chat-mute enforcement).
--
-- Separate migration (not folded into 0001) because chat mutes land with the
-- OPS-02c moderation story, after the M0/M1 characters base — the migration
-- sequence is the timeline of the DB.
--
-- A mute is CHARACTER-scoped (silences one character's chat, distinct from an
-- account/character BAN which blocks login/entry). It lives in the characters DB
-- because it is per-character durable state (§4.2) keyed by character.id. worldd
-- enforces it at the chat router (CHAT_MESSAGE handler): a message from a muted
-- character is dropped with a "you are muted" system reply.
--
-- A NULL expires_at is a permanent mute; the enforcement compares expires_at to
-- the server's UTC clock, so an elapsed mute simply stops matching (no sweep
-- required for correctness). muted_by is a SOFT numeric reference into the auth
-- DB (account.id) with NO cross-DB FK (§4.4). char_id IS a within-characters-DB
-- FK to character(id) (ON DELETE CASCADE — a deleted character drops its mutes).
-- =====================================================================

SET NAMES utf8mb4;

CREATE TABLE character_mute (
  id          BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
  char_id     BIGINT UNSIGNED  NOT NULL,                       -- owning character (in-DB FK)
  expires_at  DATETIME         NULL,                           -- NULL = permanent
  reason      VARCHAR(255)     NOT NULL,
  muted_by    BIGINT UNSIGNED  NULL,                           -- acting GM account.id (SOFT ref -> auth DB, NO cross-DB FK, §4.4)
  created_at  DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_character_mute_char (char_id),
  KEY idx_character_mute_expires (expires_at),
  CONSTRAINT fk_character_mute_char
    FOREIGN KEY (char_id) REFERENCES `character` (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
