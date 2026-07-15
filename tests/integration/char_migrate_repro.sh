#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# tests/integration/char_migrate_repro.sh — real-MariaDB repro + regression for the
# character-DB migration runner (#815, part B of #813).
#
# Reproduces the persistent-realm drift that crashlooped worldd on dev, then proves
# the runner (deploy/docker/char-migrate.sh) heals it additively:
#
#   1. Stand up a throwaway MariaDB; init meridian_characters through migration
#      0003 ONLY (simulates a persistent DB that predates 0004). Insert character
#      rows (the user data that MUST be preserved).
#   2. Show worldd's boot-gate read (SELECT ... FROM realm_content_state) FAILS —
#      the exact repro (missing table -> worldd hard-fails its boot gate).
#   3. Run the runner -> 0004 applies, schema_migrations records 0001-0004,
#      character rows preserved (count unchanged), realm_content_state exists.
#   4. Re-run the runner -> no-op (idempotent).
#   5. Fresh-init path (02-characters.sql applies+tracks everything) + runner ->
#      no double-apply.
#
# Self-contained: private mariadbd on a temp datadir + short /tmp socket, torn down
# on exit. Never touches a system MySQL/MariaDB. Requires mariadbd + mariadb client
# (Homebrew: `brew install mariadb`). Run: tests/integration/char_migrate_repro.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MIGRATIONS_DIR="${REPO_ROOT}/server/db/characters/migrations"
RUNNER="${REPO_ROOT}/deploy/docker/char-migrate.sh"
WRAPPER="${REPO_ROOT}/deploy/docker/db-init/02-characters.sql"

SOCK="/tmp/mm815.sock"
DATADIR="$(mktemp -d /tmp/mm815-data.XXXXXX)"
LOG="$(mktemp /tmp/mm815-log.XXXXXX)"
DB="meridian_characters"
PID=""

pass() { printf '  \033[32mPASS\033[0m %s\n' "$*"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; exit 1; }
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

cleanup() {
  [ -n "$PID" ] && kill "$PID" 2>/dev/null || true
  mariadb-admin --no-defaults --socket="$SOCK" -uroot shutdown 2>/dev/null || true
  sleep 1
  [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null || true
  rm -rf "$DATADIR" "$LOG" "$SOCK"
}
trap cleanup EXIT

dbc() { mariadb --no-defaults --socket="$SOCK" -uroot "$@"; }

step "boot throwaway MariaDB (datadir $DATADIR)"
rm -f "$SOCK"
mariadb-install-db --no-defaults --datadir="$DATADIR" \
  --auth-root-authentication-method=normal >>"$LOG" 2>&1
mariadbd --no-defaults --datadir="$DATADIR" --socket="$SOCK" \
  --skip-networking --pid-file="$DATADIR/mm.pid" >>"$LOG" 2>&1 &
PID=$!
for _ in $(seq 1 60); do
  mariadb-admin --no-defaults --socket="$SOCK" -uroot ping >/dev/null 2>&1 && break
  kill -0 "$PID" 2>/dev/null || { tail -20 "$LOG"; fail "mariadbd exited on startup"; }
  sleep 0.5
done
pass "MariaDB up ($(dbc -N -e 'SELECT VERSION();'))"

# ── Step 1: simulate a stale persistent DB initialized through 0003 only ──────
step "1. init meridian_characters through 0003 (stale persistent DB) + user data"
dbc -e "CREATE DATABASE IF NOT EXISTS \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
for v in 0001 0002 0003; do
  f=$(ls "$MIGRATIONS_DIR"/${v}_*.up.sql)
  dbc "$DB" < "$f"
  echo "   sourced $(basename "$f")"
done
dbc "$DB" -e "INSERT INTO \`character\`
  (account_id,name,race,class,map_id,pos_x,pos_y,pos_z)
  VALUES (1,'Aldric',1,1,0,10,20,30),
         (1,'Brenna',2,2,0,11,21,31),
         (2,'Corin', 3,3,0,12,22,32);"
BEFORE=$(dbc -N "$DB" -e "SELECT COUNT(*) FROM \`character\`;")
[ "$BEFORE" = "3" ] && pass "3 character rows inserted" || fail "expected 3 rows, got $BEFORE"

# ── Step 2: repro — worldd's boot-gate read fails on the missing table ────────
step "2. repro: worldd boot-gate read of realm_content_state FAILS"
# The exact query server/worldd/world_boot.cpp read_realm_compat_state() runs.
if dbc -N "$DB" -e "SELECT pack_namespace, compatibility_version, content_hash, pack_version FROM realm_content_state ORDER BY pack_namespace;" 2>"$LOG.gate"; then
  fail "realm_content_state unexpectedly readable before the runner"
else
  pass "boot-gate read failed as expected: $(grep -o "Table.*doesn't exist" "$LOG.gate" | head -1)"
fi
# There is no tracker on this legacy DB (initialized by the old wrapper path).
HAS_TRACKER=$(dbc -N "$DB" -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='$DB' AND table_name='schema_migrations';")
[ "$HAS_TRACKER" = "0" ] && pass "no schema_migrations tracker yet (legacy DB)" || fail "unexpected tracker present"

# ── Step 3: run the runner — applies 0004, backfills, preserves data ──────────
step "3. run the migration runner"
DB_SOCKET="$SOCK" DB_USER=root DB_NAME="$DB" \
  SCHEMAS_DIR="$MIGRATIONS_DIR" WAIT_DB_TIMEOUT=10 bash "$RUNNER"

RECORDED=$(dbc -N "$DB" -e "SELECT GROUP_CONCAT(version ORDER BY version) FROM schema_migrations;")
[ "$RECORDED" = "0001,0002,0003,0004" ] && pass "schema_migrations records 0001-0004" \
  || fail "tracker=$RECORDED (expected 0001,0002,0003,0004)"

HAS_RCS=$(dbc -N "$DB" -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='$DB' AND table_name='realm_content_state';")
[ "$HAS_RCS" = "1" ] && pass "realm_content_state now exists" || fail "realm_content_state missing after runner"

# The boot-gate read now succeeds (empty set, no error) — worldd would boot.
dbc -N "$DB" -e "SELECT pack_namespace FROM realm_content_state;" >/dev/null 2>&1 \
  && pass "boot-gate read now succeeds (worldd boots)" || fail "boot-gate read still failing"

AFTER=$(dbc -N "$DB" -e "SELECT COUNT(*) FROM \`character\`;")
[ "$AFTER" = "$BEFORE" ] && pass "character rows preserved ($AFTER == $BEFORE — additive, no data loss)" \
  || fail "DATA LOSS: character count $BEFORE -> $AFTER"
NAMES=$(dbc -N "$DB" -e "SELECT GROUP_CONCAT(name ORDER BY name) FROM \`character\`;")
[ "$NAMES" = "Aldric,Brenna,Corin" ] && pass "character contents intact ($NAMES)" || fail "row contents changed: $NAMES"

# ── Step 4: idempotency — a second run is a no-op ─────────────────────────────
step "4. re-run the runner (idempotent no-op)"
OUT=$(DB_SOCKET="$SOCK" DB_USER=root DB_NAME="$DB" \
  SCHEMAS_DIR="$MIGRATIONS_DIR" WAIT_DB_TIMEOUT=10 bash "$RUNNER" 2>&1)
echo "$OUT" | grep -q "already up to date" && pass "second run is a no-op" || { echo "$OUT"; fail "expected no-op on re-run"; }
RECORDED2=$(dbc -N "$DB" -e "SELECT GROUP_CONCAT(version ORDER BY version) FROM schema_migrations;")
[ "$RECORDED2" = "0001,0002,0003,0004" ] && pass "tracker unchanged after re-run" || fail "tracker drifted: $RECORDED2"

# ── Step 5: fresh-init path (wrapper owns first-init) + runner = no double-apply ─
step "5. fresh init via 02-characters.sql, then runner = no double-apply"
dbc -e "DROP DATABASE IF EXISTS \`$DB\`;"
# Apply the wrapper the way the meridian-db image does: SOURCE resolves against
# /schemas/characters, so run it with that as CWD substitute via a rewrite.
FRESH_SQL="$(mktemp /tmp/mm815-fresh.XXXXXX.sql)"
sed "s#/schemas/characters#${MIGRATIONS_DIR}#g" "$WRAPPER" > "$FRESH_SQL"
dbc < "$FRESH_SQL"
rm -f "$FRESH_SQL"
FRESH_TRACK=$(dbc -N "$DB" -e "SELECT GROUP_CONCAT(version ORDER BY version) FROM schema_migrations;")
[ "$FRESH_TRACK" = "0001,0002,0003,0004" ] && pass "fresh init tracked 0001-0004 (wrapper owns first-init)" \
  || fail "fresh-init tracker=$FRESH_TRACK"
OUT5=$(DB_SOCKET="$SOCK" DB_USER=root DB_NAME="$DB" \
  SCHEMAS_DIR="$MIGRATIONS_DIR" WAIT_DB_TIMEOUT=10 bash "$RUNNER" 2>&1)
echo "$OUT5" | grep -q "already up to date" && pass "runner no-ops on a fresh-initialized DB (no double-apply)" \
  || { echo "$OUT5"; fail "runner did work on a fresh DB (double-apply risk)"; }

step "ALL CHECKS PASSED"
