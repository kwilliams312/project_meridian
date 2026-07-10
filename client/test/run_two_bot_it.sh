#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# client/test/run_two_bot_it.sh — the TWO-BOT AoI E2E INTEGRATION harness (#248).
# The capstone sibling of #111's run_bot_client_it.sh (single bot login→world→move):
# this proves the IT-M0 HEADLINE (§Step 3 / DC-4) — TWO clients SEE EACH OTHER MOVE
# — with two headless bots against the REAL authd + worldd:
#
#   1. boot a THROWAWAY MariaDB on a UNIQUE socket/port (a PID-keyed path, NOT the
#      dev default /tmp/mmdb.sock:3307 — never collides with a dev DB or a parallel
#      harness),
#   2. load the auth schema (session_grant/account/realm) + the characters schema,
#   3. build authd + worldd + meridian-account (server) + meridian-two-bot (client),
#   4. create TWO test accounts (meridian-account) + seed ONE realm bound to worldd,
#   5. launch the real authd (IF-1 TLS) AND the real worldd (IF-2 TLS), both on the
#      auth DB (authd WRITES grants; worldd CONSUMES them; worldd runs the #87 AoI
#      relay),
#   6. run meridian-two-bot: two bots log in (SRP over TLS), both carry their grant
#      to worldd, both enter world at the SAME bootstrap spawn (64,64 — within the
#      40 m AoI enter radius, #87), RENDEZVOUS at a barrier so both are in-world,
#      then both walk a square path. Each bot CAPTURES the OTHER's relayed
#      EntityEnter (saw the peer on login) + EntityUpdate (saw the peer MOVE),
#   7. assert the TWO_BOT_RESULT line: both_entered=1, both saw each other ENTER,
#      AND both saw each other MOVE (a_saw_b_move=1 && b_saw_a_move=1) — the
#      see-each-other-move proof — plus distinct per-session guids (the #87 fix),
#      and that BOTH grants were CONSUMED,
#   8. tear everything down.
#
# Requires MariaDB (mariadbd, mariadb-install-db, mariadb) + cmake + flatc +
# OpenSSL on PATH. Env-guarded / integration (like #111's) — NOT wired into a fast
# CI job. Exits non-zero on any assertion failure; prints the ACTUAL two-bot output
# so the run is HONEST about full vs partial mutual visibility.
#
# Usage:  client/test/run_two_bot_it.sh
#         MERIDIAN_IT_KEEP=1 client/test/run_two_bot_it.sh   # keep the datadir

set -euo pipefail

# --- Locate the repo root (this script lives at client/test/). ---------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

# --- UNIQUE, isolated MariaDB instance (NOT the dev default /tmp/mmdb.sock:3307). --
IT_TAG="mtwobot248_$$"
DB_SOCKET="/tmp/${IT_TAG}.sock"
DB_PORT="$(( 3600 + (RANDOM % 200) ))"      # 3600..3799 — away from 3306/3307 and #111's 3400s
DB_DATADIR="${REPO_ROOT}/.dev-run/${IT_TAG}-data"
DB_LOG="${REPO_ROOT}/.dev-run/${IT_TAG}.log"
DB_PIDFILE="${REPO_ROOT}/.dev-run/${IT_TAG}.pid"
AUTH_DB_NAME="meridian_auth"
CHAR_DB_NAME="meridian_characters"

CERT_DIR="$(mktemp -d "/tmp/${IT_TAG}-cert.XXXXXX")"
CERT_PEM="${CERT_DIR}/cert.pem"
KEY_PEM="${CERT_DIR}/key.pem"

AUTHD_PORT="$(( 7700 + (RANDOM % 200) ))"   # away from the 7100/7200/#111 7300 defaults
WORLDD_PORT="$(( 7900 + (RANDOM % 200) ))"  # distinct from authd's port
AUTHD_PID=""
WORLDD_PID=""

CLIENT_BUILD=1000
ACCOUNT_A="twobot_a_$$"
ACCOUNT_B="twobot_b_$$"
PASSWORD_A="correct horse battery staple"
PASSWORD_B="trust no one but the server"
REALM_NAME="Two-Bot IT Realm $$"

log()  { printf '\033[1;34m[it]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ok]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[ERR]\033[0m %s\n' "$*" >&2; }

_dbc() { mariadb --no-defaults --socket="${DB_SOCKET}" --user=root "$@"; }

cleanup() {
  set +e
  [ -n "${WORLDD_PID}" ] && kill "${WORLDD_PID}" 2>/dev/null
  [ -n "${AUTHD_PID}" ] && kill "${AUTHD_PID}" 2>/dev/null
  if [ -f "${DB_PIDFILE}" ]; then
    mariadb-admin --no-defaults --socket="${DB_SOCKET}" --user=root shutdown 2>/dev/null \
      || kill "$(cat "${DB_PIDFILE}" 2>/dev/null)" 2>/dev/null
    sleep 1
    kill -9 "$(cat "${DB_PIDFILE}" 2>/dev/null)" 2>/dev/null
  fi
  rm -f "${DB_SOCKET}" "${DB_PIDFILE}"
  rm -rf "${CERT_DIR}"
  if [ "${MERIDIAN_IT_KEEP:-0}" = "1" ]; then
    log "kept datadir: ${DB_DATADIR}"
  else
    rm -rf "${DB_DATADIR}"
  fi
}
trap cleanup EXIT

# --- 1. Boot the throwaway MariaDB. ------------------------------------------
mkdir -p "${REPO_ROOT}/.dev-run"
rm -rf "${DB_DATADIR}"; mkdir -p "${DB_DATADIR}"
if [ -S "${DB_SOCKET}" ]; then err "socket ${DB_SOCKET} already exists"; exit 1; fi

log "initializing MariaDB datadir (${DB_DATADIR})"
mariadb-install-db --no-defaults --datadir="${DB_DATADIR}" \
  --auth-root-authentication-method=normal >>"${DB_LOG}" 2>&1 \
  || { tail -20 "${DB_LOG}" >&2; err "mariadb-install-db failed"; exit 1; }

log "starting mariadbd on port ${DB_PORT} (socket ${DB_SOCKET})"
mariadbd --no-defaults --datadir="${DB_DATADIR}" --socket="${DB_SOCKET}" \
  --port="${DB_PORT}" --bind-address=127.0.0.1 --pid-file="${DB_PIDFILE}" \
  >>"${DB_LOG}" 2>&1 &
for i in $(seq 1 60); do
  mariadb-admin --no-defaults --socket="${DB_SOCKET}" --user=root ping >/dev/null 2>&1 && break
  sleep 0.5
  [ "$i" = "60" ] && { tail -20 "${DB_LOG}" >&2; err "MariaDB did not start"; exit 1; }
done
ok "MariaDB up ($(_dbc -N -e 'SELECT VERSION();'))"

# --- 2. Load the auth + characters schemas. ----------------------------------
_dbc -e "CREATE DATABASE IF NOT EXISTS ${AUTH_DB_NAME} CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
for f in "${REPO_ROOT}"/server/db/auth/migrations/*.up.sql; do
  _dbc "${AUTH_DB_NAME}" < "$f" || { err "auth migration failed: $(basename "$f")"; exit 1; }
done
ok "auth schema loaded ($(_dbc -N "${AUTH_DB_NAME}" -e \
  'SELECT COUNT(*) FROM information_schema.tables WHERE table_schema="'"${AUTH_DB_NAME}"'";') tables)"

_dbc -e "CREATE DATABASE IF NOT EXISTS ${CHAR_DB_NAME} CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
for f in "${REPO_ROOT}"/server/db/characters/migrations/*.up.sql; do
  _dbc "${CHAR_DB_NAME}" < "$f" || { err "characters migration failed: $(basename "$f")"; exit 1; }
done
ok "characters schema loaded"

# --- 3. Build authd + worldd + meridian-account + meridian-two-bot. ----------
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
SRV_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-server-build"
CLI_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-client-build"

log "building authd + worldd + meridian-account + meridian-character (server tree)"
cmake -S "${REPO_ROOT}/server" -B "${SRV_BUILD}" >/dev/null
cmake --build "${SRV_BUILD}" --target authd worldd meridian-account meridian-character -j >/dev/null
AUTHD_BIN="$(find "${SRV_BUILD}" -name authd -type f -perm -u+x | head -1)"
WORLDD_BIN="$(find "${SRV_BUILD}" -name worldd -type f -perm -u+x | head -1)"
ACCOUNT_BIN="$(find "${SRV_BUILD}" -name meridian-account -type f -perm -u+x | head -1)"
CHARACTER_BIN="$(find "${SRV_BUILD}" -name meridian-character -type f -perm -u+x | head -1)"
[ -x "${AUTHD_BIN}" ] || { err "authd not built"; exit 1; }
[ -x "${WORLDD_BIN}" ] || { err "worldd not built"; exit 1; }
[ -x "${ACCOUNT_BIN}" ] || { err "meridian-account not built"; exit 1; }
[ -x "${CHARACTER_BIN}" ] || { err "meridian-character not built"; exit 1; }
ok "built authd + worldd + meridian-account + meridian-character"

log "building the two-bot AoI driver (client tree)"
cmake -S "${REPO_ROOT}/client" -B "${CLI_BUILD}" -DMERIDIAN_BOT=ON >/dev/null
cmake --build "${CLI_BUILD}" --target meridian-two-bot -j >/dev/null
TWOBOT_BIN="$(find "${CLI_BUILD}" -name meridian-two-bot -type f -perm -u+x | head -1)"
[ -x "${TWOBOT_BIN}" ] || { err "meridian-two-bot not built"; exit 1; }
ok "built meridian-two-bot"

# --- 4. Create TWO accounts + seed one realm bound to worldd's port. ---------
export MERIDIAN_DB_HOST=127.0.0.1
export MERIDIAN_DB_PORT="${DB_PORT}"
export MERIDIAN_DB_USER=root
export MERIDIAN_DB_NAME="${AUTH_DB_NAME}"
unset MERIDIAN_DB_PASS MERIDIAN_DB_SOCKET

log "creating test accounts '${ACCOUNT_A}' and '${ACCOUNT_B}' via meridian-account"
printf '%s' "${PASSWORD_A}" | "${ACCOUNT_BIN}" create --username "${ACCOUNT_A}" >/dev/null
printf '%s' "${PASSWORD_B}" | "${ACCOUNT_BIN}" create --username "${ACCOUNT_B}" >/dev/null
ACCOUNT_A_ID="$(_dbc -N "${AUTH_DB_NAME}" -e "SELECT id FROM account WHERE username='${ACCOUNT_A}';")"
ACCOUNT_B_ID="$(_dbc -N "${AUTH_DB_NAME}" -e "SELECT id FROM account WHERE username='${ACCOUNT_B}';")"
[ -n "${ACCOUNT_A_ID}" ] || { err "account A not created"; exit 1; }
[ -n "${ACCOUNT_B_ID}" ] || { err "account B not created"; exit 1; }
ok "accounts created (A id ${ACCOUNT_A_ID}, B id ${ACCOUNT_B_ID})"

# Pre-create ONE character per account (server-authoritative characters, D-35/#341),
# mirroring the production harness's add-characters step (which runs BEFORE the bots).
# The bots find these via CharList and ENTER_WORLD as them — no concurrent self-create
# (two bots creating simultaneously can deadlock the one-per-account cap transaction;
# the real harness always pre-creates, so the bot's CharCreate-if-empty is a fallback).
# meridian-character resolves account_id via meridian_auth.account (cross-DB) and
# inserts into meridian_characters, so point its DB NAME at the characters schema.
log "pre-creating one character per account (meridian-character)"
env MERIDIAN_DB_NAME="${CHAR_DB_NAME}" "${CHARACTER_BIN}" create --username "${ACCOUNT_A}" >/dev/null \
  || { err "character create for A failed"; exit 1; }
env MERIDIAN_DB_NAME="${CHAR_DB_NAME}" "${CHARACTER_BIN}" create --username "${ACCOUNT_B}" >/dev/null \
  || { err "character create for B failed"; exit 1; }
ok "characters pre-created for ${ACCOUNT_A} and ${ACCOUNT_B}"

log "seeding realm '${REALM_NAME}' (address 127.0.0.1:${WORLDD_PORT})"
_dbc "${AUTH_DB_NAME}" -e \
  "INSERT INTO realm (name, address, port, build_min, build_max, population, flags) \
   VALUES ('${REALM_NAME}', '127.0.0.1', ${WORLDD_PORT}, 0, 100000, 0, 0);"
REALM_ID="$(_dbc -N "${AUTH_DB_NAME}" -e "SELECT id FROM realm WHERE name='${REALM_NAME}';")"
[ -n "${REALM_ID}" ] || { err "realm not seeded"; exit 1; }
ok "realm seeded (id ${REALM_ID})"

# --- 5. Self-signed cert + launch authd AND worldd. --------------------------
log "generating self-signed TLS cert (shared by authd + worldd)"
openssl req -x509 -newkey rsa:2048 -keyout "${KEY_PEM}" -out "${CERT_PEM}" \
  -days 1 -nodes -subj "/CN=meridian-it" >/dev/null 2>&1

log "launching authd on 127.0.0.1:${AUTHD_PORT}"
"${AUTHD_BIN}" --cert "${CERT_PEM}" --key "${KEY_PEM}" \
  --bind 127.0.0.1 --port "${AUTHD_PORT}" --realm reference &
AUTHD_PID=$!
for i in $(seq 1 40); do
  nc -z 127.0.0.1 "${AUTHD_PORT}" 2>/dev/null && break
  kill -0 "${AUTHD_PID}" 2>/dev/null || { err "authd exited during startup"; exit 1; }
  sleep 0.25
  [ "$i" = "40" ] && { err "authd did not open its port"; exit 1; }
done
ok "authd listening on ${AUTHD_PORT}"

# worldd validates the grant against the SAME auth DB (MERIDIAN_DB_*) and binds to
# the seeded realm id. Characters DB wired so enter-world loads a character (else the
# deterministic placeholder). BOTH bots' placeholder sessions get DISTINCT synthetic
# guids server-side (#87), which the driver cross-checks.
export MERIDIAN_WORLDD_REALM_ID="${REALM_ID}"
export MERIDIAN_CHARDB_HOST=127.0.0.1
export MERIDIAN_CHARDB_PORT="${DB_PORT}"
export MERIDIAN_CHARDB_USER=root
export MERIDIAN_CHARDB_NAME="${CHAR_DB_NAME}"

log "launching worldd on 127.0.0.1:${WORLDD_PORT} (realm_id=${REALM_ID}, #87 AoI relay)"
"${WORLDD_BIN}" --cert "${CERT_PEM}" --key "${KEY_PEM}" \
  --bind 127.0.0.1 --port "${WORLDD_PORT}" --realm reference &
WORLDD_PID=$!
for i in $(seq 1 40); do
  nc -z 127.0.0.1 "${WORLDD_PORT}" 2>/dev/null && break
  kill -0 "${WORLDD_PID}" 2>/dev/null || { err "worldd exited during startup"; exit 1; }
  sleep 0.25
  [ "$i" = "40" ] && { err "worldd did not open its port"; exit 1; }
done
ok "worldd listening on ${WORLDD_PORT}"

# --- 6. Drive the TWO BOTS against the real authd + worldd. ------------------
log "running meridian-two-bot (two bots: login -> enter-world -> move, over TLS 1.3)"
set +e
TWOBOT_OUT="$("${TWOBOT_BIN}" \
  --authd "127.0.0.1:${AUTHD_PORT}" \
  --worldd "127.0.0.1:${WORLDD_PORT}" \
  --userA "${ACCOUNT_A}" --passA "${PASSWORD_A}" \
  --userB "${ACCOUNT_B}" --passB "${PASSWORD_B}" \
  --realm "${REALM_ID}" --build "${CLIENT_BUILD}" \
  --duration 6 2>&1)"
TWOBOT_RC=$?
set -e
echo "${TWOBOT_OUT}"

# --- 7. Assert mutual visibility + grants consumed. --------------------------
RESULT_LINE="$(printf '%s\n' "${TWOBOT_OUT}" | grep '^TWO_BOT_RESULT ' | head -1)"
[ -n "${RESULT_LINE}" ] || { err "two-bot emitted no TWO_BOT_RESULT line"; exit 1; }

_field() { printf '%s\n' "${RESULT_LINE}" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"; }
BOTH_ENTERED="$(_field both_entered)"
BOTH_MOVED="$(_field both_moved)"
A_SAW_B_ENTER="$(_field a_saw_b_enter)"
B_SAW_A_ENTER="$(_field b_saw_a_enter)"
A_SAW_B_MOVE="$(_field a_saw_b_move)"
B_SAW_A_MOVE="$(_field b_saw_a_move)"
A_UPDATES="$(_field a_updates)"
B_UPDATES="$(_field b_updates)"
DISTINCT_GUIDS="$(_field distinct_guids)"

echo
it_fail=0

# (a) both entered world.
[ "${BOTH_ENTERED}" = "1" ] && ok "BOTH bots ENTERED THE WORLD (HandshakeOk from real worldd)" \
  || { err "both bots did NOT enter the world (both_entered=${BOTH_ENTERED})"; it_fail=1; }

# (b) both moved (server-accepted).
[ "${BOTH_MOVED}" = "1" ] && ok "BOTH bots MOVED (worldd accepted movement for each)" \
  || { err "not both bots moved (both_moved=${BOTH_MOVED})"; it_fail=1; }

# (c) mutual ENTER — they see each other on login (the #87 relay, login-time visibility).
if [ "${A_SAW_B_ENTER}" = "1" ] && [ "${B_SAW_A_ENTER}" = "1" ]; then
  ok "MUTUAL ENTER — A saw B enter AND B saw A enter (login-time AoI visibility)"
else
  err "mutual ENTER incomplete (a_saw_b_enter=${A_SAW_B_ENTER}, b_saw_a_enter=${B_SAW_A_ENTER})"
  it_fail=1
fi

# (d) THE HEADLINE — mutual MOVE: each receives the other's EntityUpdate as it moves.
if [ "${A_SAW_B_MOVE}" = "1" ] && [ "${B_SAW_A_MOVE}" = "1" ]; then
  ok "SEE-EACH-OTHER-MOVE — A saw B move (${A_UPDATES} EntityUpdate) AND B saw A move (${B_UPDATES} EntityUpdate)"
else
  err "SEE-EACH-OTHER-MOVE INCOMPLETE (a_saw_b_move=${A_SAW_B_MOVE} [${A_UPDATES} upd], b_saw_a_move=${B_SAW_A_MOVE} [${B_UPDATES} upd])"
  it_fail=1
fi

# (e) distinct per-session guids (#87 fix: two placeholder sessions are not one entity).
[ "${DISTINCT_GUIDS}" = "1" ] && ok "distinct per-session guids (the two bots are NOT one entity — #87 fix)" \
  || { err "the two bots did not present distinct guids (distinct_guids=${DISTINCT_GUIDS})"; it_fail=1; }

# (f) BOTH grants consumed by worldd (single-use spent on enter-world).
GRANT_A="$(_dbc -N "${AUTH_DB_NAME}" -e \
  "SELECT consumed_at IS NOT NULL FROM session_grant WHERE account_id=${ACCOUNT_A_ID} ORDER BY grant_id DESC LIMIT 1;")"
GRANT_B="$(_dbc -N "${AUTH_DB_NAME}" -e \
  "SELECT consumed_at IS NOT NULL FROM session_grant WHERE account_id=${ACCOUNT_B_ID} ORDER BY grant_id DESC LIMIT 1;")"
if [ "${GRANT_A}" = "1" ] && [ "${GRANT_B}" = "1" ]; then
  ok "BOTH grants CONSUMED by worldd (single-use spent on enter-world)"
else
  err "a grant was not consumed (A=${GRANT_A}, B=${GRANT_B})"; it_fail=1
fi

[ "${TWOBOT_RC}" -eq 0 ] && ok "meridian-two-bot exited 0 (full mutual visibility)" \
  || { err "meridian-two-bot exited ${TWOBOT_RC} (mutual visibility incomplete — see verdict)"; it_fail=1; }

[ "${it_fail}" -eq 0 ] || { err "TWO-BOT AoI E2E assertions FAILED"; exit 1; }

echo
ok "TWO HEADLESS BOTS ↔ REAL authd + worldd — SEE EACH OTHER MOVE: ALL ASSERTIONS PASSED"
