#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# client/test/run_bot_client_it.sh — the HEADLESS BOT ↔ REAL servers INTEGRATION
# harness (IT-M0 full client path, #111). The bigger sibling of #99's
# run_authd_login_it.sh: that proves client↔authd LOGIN; this proves the WHOLE
# client protocol — login → enter-world → MOVE — against the REAL authd AND worldd.
#
# THE KILLER TEST driver. It proves the headless bot interoperates with the real
# server auth + world path end to end:
#   1. boot a THROWAWAY MariaDB on a UNIQUE socket/port (NOT the dev default
#      /tmp/mmdb.sock:3307 — a distinct PID-keyed path so it never collides with a
#      dev DB or a parallel harness),
#   2. load the auth schema (session_grant/account/realm) + the characters schema,
#   3. build authd + worldd + meridian-account (server tree) + meridian-bot (client),
#   4. create a test account (meridian-account) + seed a realm bound to worldd's port,
#   5. launch the real authd (IF-1 TLS) AND the real worldd (IF-2 TLS), both pointed
#      at the auth DB (authd WRITES the grant; worldd CONSUMES it),
#   6. run meridian-bot --authd ... --worldd ... --path square: it logs in (SRP over
#      TLS), carries the grant to worldd, sends WorldHello, gets HandshakeOk (ENTERS
#      THE WORLD), then drives MovementIntents the server validates (#86) and
#      answers with authoritative MovementStates,
#   7. assert the bot's BOT_RESULT line: handshake_ok=1 (entered world) AND
#      moves_accepted > 0 (the server accepted movement / advanced the authoritative
#      position), and that the grant row was CONSUMED by worldd (single-use spent),
#   8. tear everything down.
#
# Requires MariaDB (mariadbd, mariadb-install-db, mariadb) + cmake + flatc +
# OpenSSL on PATH (all present on the dev box). Exits non-zero on any assertion
# failure; prints the ACTUAL bot output so the run is honest about how far it got.
#
# Usage:  client/test/run_bot_client_it.sh
#         MERIDIAN_IT_KEEP=1 client/test/run_bot_client_it.sh   # keep the datadir

set -euo pipefail

# --- Locate the repo root (this script lives at client/test/). ---------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

# --- UNIQUE, isolated MariaDB instance (NOT the dev default). -----------------
IT_TAG="mbot111_$$"
DB_SOCKET="/tmp/${IT_TAG}.sock"
DB_PORT="$(( 3400 + (RANDOM % 200) ))"      # 3400..3599 — away from 3306/3307
DB_DATADIR="${REPO_ROOT}/.dev-run/${IT_TAG}-data"
DB_LOG="${REPO_ROOT}/.dev-run/${IT_TAG}.log"
DB_PIDFILE="${REPO_ROOT}/.dev-run/${IT_TAG}.pid"
AUTH_DB_NAME="meridian_auth"
CHAR_DB_NAME="meridian_characters"

CERT_DIR="$(mktemp -d "/tmp/${IT_TAG}-cert.XXXXXX")"
CERT_PEM="${CERT_DIR}/cert.pem"
KEY_PEM="${CERT_DIR}/key.pem"

AUTHD_PORT="$(( 7300 + (RANDOM % 200) ))"   # away from the 7100/7200 defaults
WORLDD_PORT="$(( 7500 + (RANDOM % 200) ))"  # distinct from authd's port
AUTHD_PID=""
WORLDD_PID=""

CLIENT_BUILD=1000
ACCOUNT="bot_it_$$"
PASSWORD="correct horse battery staple"
REALM_NAME="Bot IT Realm $$"

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

# --- 3. Build authd + worldd + meridian-account + meridian-bot. --------------
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
SRV_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-server-build"
CLI_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-client-build"

log "building authd + worldd + meridian-account (server tree)"
cmake -S "${REPO_ROOT}/server" -B "${SRV_BUILD}" >/dev/null
cmake --build "${SRV_BUILD}" --target authd worldd meridian-account -j >/dev/null
AUTHD_BIN="$(find "${SRV_BUILD}" -name authd -type f -perm -u+x | head -1)"
WORLDD_BIN="$(find "${SRV_BUILD}" -name worldd -type f -perm -u+x | head -1)"
ACCOUNT_BIN="$(find "${SRV_BUILD}" -name meridian-account -type f -perm -u+x | head -1)"
[ -x "${AUTHD_BIN}" ] || { err "authd not built"; exit 1; }
[ -x "${WORLDD_BIN}" ] || { err "worldd not built"; exit 1; }
[ -x "${ACCOUNT_BIN}" ] || { err "meridian-account not built"; exit 1; }
ok "built authd + worldd + meridian-account"

log "building the headless bot (client tree)"
cmake -S "${REPO_ROOT}/client" -B "${CLI_BUILD}" -DMERIDIAN_BOT=ON >/dev/null
cmake --build "${CLI_BUILD}" --target meridian-bot -j >/dev/null
BOT_BIN="$(find "${CLI_BUILD}" -name meridian-bot -type f -perm -u+x | head -1)"
[ -x "${BOT_BIN}" ] || { err "meridian-bot not built"; exit 1; }
ok "built meridian-bot"

# --- 4. Create the account + seed a realm bound to worldd's port. ------------
export MERIDIAN_DB_HOST=127.0.0.1
export MERIDIAN_DB_PORT="${DB_PORT}"
export MERIDIAN_DB_USER=root
export MERIDIAN_DB_NAME="${AUTH_DB_NAME}"
unset MERIDIAN_DB_PASS MERIDIAN_DB_SOCKET

log "creating test account '${ACCOUNT}' via meridian-account"
printf '%s' "${PASSWORD}" | "${ACCOUNT_BIN}" create --username "${ACCOUNT}" >/dev/null
ACCOUNT_ID="$(_dbc -N "${AUTH_DB_NAME}" -e "SELECT id FROM account WHERE username='${ACCOUNT}';")"
[ -n "${ACCOUNT_ID}" ] || { err "account not created"; exit 1; }
ok "account created (id ${ACCOUNT_ID})"

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
# the seeded realm id so the grant's realm binding is actually exercised. No world
# DB wired (MERIDIAN_WORLDDB_* unset -> world-DB boot check skipped); characters DB
# wired so enter-world loads a character (else the deterministic placeholder).
export MERIDIAN_WORLDD_REALM_ID="${REALM_ID}"
export MERIDIAN_CHARDB_HOST=127.0.0.1
export MERIDIAN_CHARDB_PORT="${DB_PORT}"
export MERIDIAN_CHARDB_USER=root
export MERIDIAN_CHARDB_NAME="${CHAR_DB_NAME}"

log "launching worldd on 127.0.0.1:${WORLDD_PORT} (realm_id=${REALM_ID})"
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

# --- 6. Drive the BOT against the real authd + worldd. -----------------------
log "running meridian-bot (login -> enter-world -> move, over TLS 1.3)"
set +e
BOT_OUT="$("${BOT_BIN}" \
  --authd "127.0.0.1:${AUTHD_PORT}" \
  --worldd "127.0.0.1:${WORLDD_PORT}" \
  --user "${ACCOUNT}" --password "${PASSWORD}" \
  --realm "${REALM_ID}" --build "${CLIENT_BUILD}" \
  --duration 5 --path square 2>&1)"
BOT_RC=$?
set -e
echo "${BOT_OUT}"

# --- 7. Assert how far the bot got + the grant was consumed. -----------------
RESULT_LINE="$(printf '%s\n' "${BOT_OUT}" | grep '^BOT_RESULT ' | head -1)"
[ -n "${RESULT_LINE}" ] || { err "bot emitted no BOT_RESULT line"; exit 1; }

_field() { printf '%s\n' "${RESULT_LINE}" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"; }
HANDSHAKE="$(_field handshake_ok)"
INTENTS="$(_field intents_sent)"
STATES="$(_field states_received)"
ACCEPTED="$(_field moves_accepted)"
MOVED="$(_field moved_distance)"

it_fail=0
[ "${HANDSHAKE}" = "1" ] && ok "bot ENTERED THE WORLD (HandshakeOk from real worldd)" \
  || { err "bot did NOT enter the world (handshake_ok=${HANDSHAKE})"; it_fail=1; }
[ "${INTENTS:-0}" -gt 0 ] 2>/dev/null && ok "bot sent MovementIntents (${INTENTS})" \
  || { err "bot sent no MovementIntents"; it_fail=1; }
[ "${STATES:-0}" -gt 0 ] 2>/dev/null && ok "real worldd returned MovementStates (${STATES})" \
  || { err "worldd returned no MovementState"; it_fail=1; }
[ "${ACCEPTED:-0}" -gt 0 ] 2>/dev/null && ok "worldd ACCEPTED movement — authoritative position advanced (${ACCEPTED} states, ${MOVED} m)" \
  || { err "worldd accepted NO movement (moves_accepted=${ACCEPTED})"; it_fail=1; }

# The bot's grant must have been CONSUMED by worldd (single-use spent on enter-world).
GRANT_STATE="$(_dbc -N "${AUTH_DB_NAME}" -e \
  "SELECT consumed_at IS NOT NULL FROM session_grant WHERE account_id=${ACCOUNT_ID} ORDER BY grant_id DESC LIMIT 1;")"
[ "${GRANT_STATE}" = "1" ] && ok "grant CONSUMED by worldd (single-use spent on enter-world)" \
  || { err "grant was not consumed by worldd (consumed=${GRANT_STATE})"; it_fail=1; }

[ "${BOT_RC}" -eq 0 ] && ok "meridian-bot exited 0 (reached its milestone)" \
  || { err "meridian-bot exited ${BOT_RC}"; it_fail=1; }

[ "${it_fail}" -eq 0 ] || { err "BOT INTEGRATION assertions FAILED"; exit 1; }

echo
ok "HEADLESS BOT ↔ REAL authd + worldd (login -> enter-world -> MOVE): ALL ASSERTIONS PASSED"
