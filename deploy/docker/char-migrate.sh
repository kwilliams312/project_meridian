#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# meridian-db character-DB migration runner (#815, part B of #813).
#
# WHY THIS EXISTS: character migrations (server/db/characters/migrations/
# NNNN_*.up.sql) are applied ONLY by deploy/docker/db-init/02-characters.sql on a
# FRESH initdb datadir. On a PERSISTENT realm the datadir survives, so initdb
# never re-runs and any migration added AFTER first init never lands — e.g.
# worldd's #698 boot gate reads meridian_characters.realm_content_state (migration
# 0004); on a persistent dev DB that predated 0004 the table was missing and
# worldd hard-failed its boot gate ("world-DB boot refused ... realm_content_state
# doesn't exist") into a CrashLoopBackOff. This runner applies the UN-APPLIED
# *.up.sql to an already-initialized characters DB, in order, tracked in a
# schema_migrations table.
#
# ⛔ THE CHARACTERS DB HAS USER DATA. This runner is ADDITIVE ONLY: it SOURCEs
# forward *.up.sql migrations and NEVER drops/truncates/replaces. (Contrast the
# world DB, whose content-seed Job TRUNCATE+reloads a read-only mcc artifact.)
# Destructive/breaking migrations are NOT auto-run here — they are gated behind
# the operator migration tool referenced in 0004's header (out of scope).
#
# FIRST-INIT OWNERSHIP (how initdb and this runner coexist without double-apply):
#   * FRESH DB  -> 02-characters.sql owns it: it SOURCEs every *.up.sql AND seeds
#     schema_migrations with every version. This runner then finds a fully-tracked
#     DB and is a NO-OP. initdb owns first-init.
#   * LEGACY DB (initialized by an OLDER 02-characters.sql that had no
#     schema_migrations) -> this runner detects the missing tracking table,
#     BACKFILLS the versions already present (probing each migration's sentinel
#     object so it records exactly what is applied — e.g. 0001-0003 on a DB that
#     predates 0004), then applies the gap (0004+) and records each.
#   * UP-TO-DATE DB -> tracking table present, every version recorded -> NO-OP.
# Idempotent: safe to run on every deploy/sync.
#
# Env (all optional; defaults suit the Helm hook / a locally-baked image):
#   DB_HOST (default 127.0.0.1)   DB_PORT (default 3306)
#   DB_SOCKET (unset -> TCP; set -> connect via unix socket, for local testing)
#   DB_USER (default root)        MYSQL_PWD (password; unset -> no password)
#   DB_NAME (default meridian_characters)
#   SCHEMAS_DIR (default /schemas/characters) — dir holding NNNN_*.up.sql
#   WAIT_DB_TIMEOUT (default 60) — seconds to wait for the DB before SKIPPING
#     (a first-install PreSync runs before MariaDB is up; initdb then owns init).
set -eu

DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-3306}"
DB_USER="${DB_USER:-root}"
DB_NAME="${DB_NAME:-meridian_characters}"
SCHEMAS_DIR="${SCHEMAS_DIR:-/schemas/characters}"
WAIT_DB_TIMEOUT="${WAIT_DB_TIMEOUT:-60}"

log()  { echo "[char-migrate] $*"; }
warn() { echo "[char-migrate] WARNING: $*" >&2; }
err()  { echo "[char-migrate] ERROR: $*" >&2; }

# The mariadb client bound to OUR target only. DB_SOCKET (local tests) takes
# precedence over host/port when set.
if [ -n "${DB_SOCKET:-}" ]; then
  mysql() { mariadb --socket="$DB_SOCKET" -u"$DB_USER" "$@"; }
else
  mysql() { mariadb -h"$DB_HOST" -P"$DB_PORT" -u"$DB_USER" "$@"; }
fi

# Scalar query helper (silent, no headers).
q() { mysql -N -B -e "$1" 2>/dev/null; }

# ── 1. Bounded wait for the DB. A first-install PreSync hook runs BEFORE MariaDB
# is synced; rather than wedge the ArgoCD sync (#492) we SKIP after the timeout —
# initdb owns that fresh DB and applies everything itself. ────────────────────
waited=0
until mysql -e "SELECT 1" >/dev/null 2>&1; do
  if [ "$waited" -ge "$WAIT_DB_TIMEOUT" ]; then
    warn "database not reachable within ${WAIT_DB_TIMEOUT}s — SKIPPING char migration"
    warn "(a fresh install applies every migration via initdb/02-characters.sql; nothing to do here)"
    exit 0
  fi
  log "waiting for database ($DB_NAME) ... ${waited}s"
  sleep 2
  waited=$((waited + 2))
done

# The migrations directory ships in the meridian-db image; absent only if the
# pinned image predates this feature (#492 fail-safe: skip, don't wedge sync).
if [ ! -d "$SCHEMAS_DIR" ]; then
  warn "schemas dir $SCHEMAS_DIR missing from image — skipping char migration (#492)"
  exit 0
fi
UP_FILES=$(ls "$SCHEMAS_DIR"/[0-9]*.up.sql 2>/dev/null | sort || true)
if [ -z "$UP_FILES" ]; then
  warn "no *.up.sql migrations found in $SCHEMAS_DIR — nothing to do"
  exit 0
fi

# ── 2. Ensure the characters DB + tracking table exist. Detect whether the
# tracking table PRE-EXISTED (fresh/up-to-date DB) or we are creating it now
# (legacy DB needing a backfill). ─────────────────────────────────────────────
mysql -e "CREATE DATABASE IF NOT EXISTS \`$DB_NAME\`
            CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"

tracked_before=$(q "SELECT COUNT(*) FROM information_schema.tables
                    WHERE table_schema='$DB_NAME' AND table_name='schema_migrations'")
tracked_before="${tracked_before:-0}"

mysql "$DB_NAME" -e "CREATE TABLE IF NOT EXISTS schema_migrations (
  version    VARCHAR(255) NOT NULL,
  applied_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci"

# Parse the version token (leading digits) from a migration filename.
version_of() { basename "$1" | sed -n 's/^\([0-9][0-9]*\)_.*/\1/p'; }

# Read a migration's applied-probe directive:
#   -- meridian:applied-probe table:<name>
#   -- meridian:applied-probe column:<table>.<col>
# The migration file OWNS its own probe (single source of truth); the CI guard
# tests/test_db_init_migration_coverage.py fails if any *.up.sql lacks one.
probe_of() {
  # Capture only the first whitespace-delimited token after the directive keyword
  # (the spec, e.g. "table:character"); any trailing inline "-- ..." is ignored.
  sed -n 's/^--[[:space:]]*meridian:applied-probe[[:space:]]*\([^[:space:]][^[:space:]]*\).*/\1/p' "$1" \
    | head -1
}

# Does a migration's sentinel object already exist in the live DB? (backfill.)
probe_present() {
  spec="$1"
  kind="${spec%%:*}"; rest="${spec#*:}"
  case "$kind" in
    table)
      cnt=$(q "SELECT COUNT(*) FROM information_schema.tables
               WHERE table_schema='$DB_NAME' AND table_name='$rest'") ;;
    column)
      tbl="${rest%%.*}"; col="${rest#*.}"
      cnt=$(q "SELECT COUNT(*) FROM information_schema.columns
               WHERE table_schema='$DB_NAME' AND table_name='$tbl' AND column_name='$col'") ;;
    *)
      err "unknown applied-probe kind '$kind' (spec: '$spec')"; return 2 ;;
  esac
  [ "${cnt:-0}" -gt 0 ]
}

# ── 3. Backfill for a LEGACY DB (tracking table did not exist). Walk migrations
# ascending; record each whose sentinel object is present as already-applied.
# Migrations are strictly ordered and cumulative, so the applied set is a
# contiguous prefix — stop at the first migration whose sentinel is ABSENT. ────
if [ "$tracked_before" -eq 0 ]; then
  log "no schema_migrations table found — backfilling applied state from schema sentinels"
  for f in $UP_FILES; do
    ver=$(version_of "$f")
    spec=$(probe_of "$f")
    [ -n "$spec" ] || { err "migration $(basename "$f") has no 'meridian:applied-probe' directive"; exit 1; }
    if probe_present "$spec"; then
      mysql "$DB_NAME" -e "INSERT IGNORE INTO schema_migrations (version) VALUES ('$ver')"
      log "  backfill: $ver already applied (sentinel $spec present)"
    else
      log "  backfill: $ver NOT applied (sentinel $spec absent) — stopping backfill"
      break
    fi
  done
fi

# ── 4. Apply every migration whose version is not recorded, ascending, each in a
# transaction, recording it on success. A real apply error is FATAL: we exit
# non-zero so the PreSync hook FAILS and blocks the rollout, rather than let
# worldd roll onto a half-migrated schema and CrashLoop indefinitely. (This
# deliberately diverges from the world-content-seed Job, which soft-skips because
# worldd DEGRADES gracefully on missing CONTENT — it does NOT degrade on a
# missing migrated TABLE.) ────────────────────────────────────────────────────
applied_count=0
for f in $UP_FILES; do
  ver=$(version_of "$f")
  already=$(q "SELECT COUNT(*) FROM \`$DB_NAME\`.schema_migrations WHERE version='$ver'")
  if [ "${already:-0}" -gt 0 ]; then
    continue
  fi
  log "applying migration $ver ($(basename "$f")) ..."
  # Wrap the file + its tracking INSERT in one non-interactive session; the
  # client aborts on the first error (default), so a failed migration never
  # records a schema_migrations row.
  {
    echo "START TRANSACTION;"
    cat "$f"
    echo ";"
    echo "INSERT INTO schema_migrations (version) VALUES ('$ver');"
    echo "COMMIT;"
  } | mysql "$DB_NAME" || { err "migration $ver FAILED — aborting (DB left at previous version)"; exit 1; }
  applied_count=$((applied_count + 1))
  log "  applied $ver"
done

if [ "$applied_count" -eq 0 ]; then
  log "characters DB already up to date — no migrations applied"
else
  log "characters DB migrated — $applied_count migration(s) applied"
fi
