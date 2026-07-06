-- =====================================================================
-- Project Meridian — auth DB schema v1 (migration 0001, DOWN)
-- Reverses 0001_init_auth.up.sql: drops exactly what UP created.
--
-- Drop order is the reverse of create order so foreign keys never block a
-- DROP: children (session_grant, ip_ban, account_ban) before parents
-- (realm, account). IF EXISTS keeps the rollback idempotent.
-- =====================================================================

SET NAMES utf8mb4;

DROP TABLE IF EXISTS session_grant;
DROP TABLE IF EXISTS ip_ban;
DROP TABLE IF EXISTS account_ban;
DROP TABLE IF EXISTS realm;
DROP TABLE IF EXISTS account;
