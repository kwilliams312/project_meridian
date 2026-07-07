-- SPDX-License-Identifier: Apache-2.0
-- Project Meridian compose DB init (#177) — app-user grants.
--
-- MariaDB's entrypoint creates MARIADB_USER but only grants it on
-- MARIADB_DATABASE (which we intentionally don't set — we create three DBs
-- ourselves in 01/02/03). So grant the app user access to all three meridian_*
-- schemas here, after they exist. '%' host = any container on the compose net.
-- Runs alphabetically last (04-) so the databases already exist.
GRANT ALL PRIVILEGES ON `meridian_auth`.*       TO 'meridian'@'%';
GRANT ALL PRIVILEGES ON `meridian_characters`.* TO 'meridian'@'%';
GRANT ALL PRIVILEGES ON `meridian_world`.*       TO 'meridian'@'%';
FLUSH PRIVILEGES;
