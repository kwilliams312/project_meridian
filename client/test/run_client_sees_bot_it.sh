#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# client/test/run_client_sees_bot_it.sh — THE see-a-bot-move proof for the GUI net
# path (#301). The sibling of run_two_bot_it.sh (#248), but instead of two bots it
# runs ONE headless bot AND the GUI client's WORLD-SESSION net path headlessly
# (meridian-client-probe — the SAME net stack scenes/world/world.gd drives:
# net::NetThreadCore + meridian-clientnet), and asserts the CLIENT receives the
# bot's EntityEnter + a stream of EntityUpdate (i.e. the client SEES the bot MOVE)
# via the #87 AoI relay. This proves the relay reaches the GUI client path with NO
# display — the data-flow half of the on-screen demo (scripts/dev/demo-networked.sh).
#
#   1. boot a THROWAWAY MariaDB on a UNIQUE socket/port (PID-keyed, never the dev
#      default /tmp/mmdb.sock:3307 — no collision with a dev DB or a parallel harness),
#   2. load the auth + characters schemas,
#   3. build authd + worldd + meridian-account (server) + meridian-bot AND
#      meridian-client-probe (client),
#   4. create TWO accounts (one for the bot, one for the client) + seed ONE realm,
#   5. launch the real authd (IF-1 TLS) AND the real worldd (IF-2, #87 AoI relay),
#   6. launch the BOT first (walks a square for a long window), then — while the bot
#      is in-world and moving at the shared bootstrap spawn (64,64, within the 40 m
#      AoI enter radius, #87) — launch the CLIENT PROBE, which enters the world on
#      the GUI net path and DRAINS the relay,
#   7. assert the CLIENT_PROBE_RESULT line: entered=1, saw_peer_enter=1 AND
#      saw_peer_move=1 (the client got the bot's EntityEnter + EntityUpdate stream),
#      and that the client's grant was CONSUMED,
#   8. tear everything down.
#
# Requires MariaDB (mariadbd, mariadb-install-db, mariadb) + cmake + flatc +
# OpenSSL + nc on PATH. Env-guarded / integration (like #111/#248) — NOT a fast CI
# job. Exits non-zero on any assertion failure; prints the ACTUAL probe output so a
# partial result is HONEST about what reached the client.
#
# Usage:  client/test/run_client_sees_bot_it.sh
#         MERIDIAN_IT_KEEP=1 client/test/run_client_sees_bot_it.sh   # keep the datadir

set -euo pipefail

# --- Locate the repo root (this script lives at client/test/). ---------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

# --- UNIQUE, isolated MariaDB instance. --------------------------------------
IT_TAG="mclientbot301_$$"
DB_SOCKET="/tmp/${IT_TAG}.sock"
DB_PORT="$(( 3800 + (RANDOM % 150) ))"      # 3800..3949 — away from 3306/3307 and the #248 3600s
DB_DATADIR="${REPO_ROOT}/.dev-run/${IT_TAG}-data"
DB_LOG="${REPO_ROOT}/.dev-run/${IT_TAG}.log"
DB_PIDFILE="${REPO_ROOT}/.dev-run/${IT_TAG}.pid"
AUTH_DB_NAME="meridian_auth"
CHAR_DB_NAME="meridian_characters"

CERT_DIR="$(mktemp -d "/tmp/${IT_TAG}-cert.XXXXXX")"
CERT_PEM="${CERT_DIR}/cert.pem"
KEY_PEM="${CERT_DIR}/key.pem"

AUTHD_PORT="$(( 7500 + (RANDOM % 150) ))"
WORLDD_PORT="$(( 7650 + (RANDOM % 150) ))"
AUTHD_PID=""
WORLDD_PID=""
BOT_PID=""

CLIENT_BUILD=1000
ACCOUNT_BOT="seenbot_$$"
ACCOUNT_CLI="seencli_$$"
PASSWORD_BOT="the bot walks a square all day"
PASSWORD_CLI="the client just wants to watch"
REALM_NAME="Client-Sees-Bot IT Realm $$"

# Timing: the bot must be in-world + moving while the client is connected. The bot
# walks for BOT_DURATION s; the client connects after CLIENT_DELAY s and observes
# for CLIENT_DURATION s (finishing before the bot leaves).
BOT_DURATION=22
CLIENT_DELAY=3
CLIENT_DURATION=10

log()  { printf '\033[1;34m[it]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ok]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[ERR]\033[0m %s\n' "$*" >&2; }

_dbc() { mariadb --no-defaults --socket="${DB_SOCKET}" --user=root "$@"; }

cleanup() {
  set +e
  [ -n "${BOT_PID}" ] && kill "${BOT_PID}" 2>/dev/null
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
ok "auth schema loaded"

_dbc -e "CREATE DATABASE IF NOT EXISTS ${CHAR_DB_NAME} CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
for f in "${REPO_ROOT}"/server/db/characters/migrations/*.up.sql; do
  _dbc "${CHAR_DB_NAME}" < "$f" || { err "characters migration failed: $(basename "$f")"; exit 1; }
done
ok "characters schema loaded"

# --- 3. Build authd + worldd + meridian-account + meridian-bot + probe. ------
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

log "building meridian-bot + meridian-client-probe (client tree)"
cmake -S "${REPO_ROOT}/client" -B "${CLI_BUILD}" -DMERIDIAN_BOT=ON -DMERIDIAN_CLIENT_PROBE=ON >/dev/null
cmake --build "${CLI_BUILD}" --target meridian-bot meridian-client-probe -j >/dev/null
BOT_BIN="$(find "${CLI_BUILD}" -name meridian-bot -type f -perm -u+x | head -1)"
PROBE_BIN="$(find "${CLI_BUILD}" -name meridian-client-probe -type f -perm -u+x | head -1)"
[ -x "${BOT_BIN}" ] || { err "meridian-bot not built"; exit 1; }
[ -x "${PROBE_BIN}" ] || { err "meridian-client-probe not built"; exit 1; }
ok "built meridian-bot + meridian-client-probe"

# --- 4. Create the two accounts + seed one realm bound to worldd. ------------
export MERIDIAN_DB_HOST=127.0.0.1
export MERIDIAN_DB_PORT="${DB_PORT}"
export MERIDIAN_DB_USER=root
export MERIDIAN_DB_NAME="${AUTH_DB_NAME}"
unset MERIDIAN_DB_PASS MERIDIAN_DB_SOCKET

log "creating accounts '${ACCOUNT_BOT}' (bot) and '${ACCOUNT_CLI}' (client)"
printf '%s' "${PASSWORD_BOT}" | "${ACCOUNT_BIN}" create --username "${ACCOUNT_BOT}" >/dev/null
printf '%s' "${PASSWORD_CLI}" | "${ACCOUNT_BIN}" create --username "${ACCOUNT_CLI}" >/dev/null
CLI_ACCOUNT_ID="$(_dbc -N "${AUTH_DB_NAME}" -e "SELECT id FROM account WHERE username='${ACCOUNT_CLI}';")"
[ -n "${CLI_ACCOUNT_ID}" ] || { err "client account not created"; exit 1; }
ok "accounts created (client id ${CLI_ACCOUNT_ID})"

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

# --- 6. Launch the BOT (long walk), then the CLIENT PROBE while it moves. -----
BOT_LOG="${REPO_ROOT}/.dev-run/${IT_TAG}-bot.log"
log "launching meridian-bot (walks a square for ${BOT_DURATION}s at the spawn)"
"${BOT_BIN}" \
  --authd "127.0.0.1:${AUTHD_PORT}" \
  --worldd "127.0.0.1:${WORLDD_PORT}" \
  --user "${ACCOUNT_BOT}" --password "${PASSWORD_BOT}" \
  --realm "${REALM_ID}" --build "${CLIENT_BUILD}" \
  --path square --duration "${BOT_DURATION}" >"${BOT_LOG}" 2>&1 &
BOT_PID=$!

log "waiting ${CLIENT_DELAY}s for the bot to enter world + start moving"
sleep "${CLIENT_DELAY}"
kill -0 "${BOT_PID}" 2>/dev/null || { err "bot exited early"; cat "${BOT_LOG}" >&2; exit 1; }

log "running meridian-client-probe (GUI net path enters world + drains the relay for ${CLIENT_DURATION}s)"
set +e
PROBE_OUT="$("${PROBE_BIN}" \
  --authd "127.0.0.1:${AUTHD_PORT}" \
  --worldd "127.0.0.1:${WORLDD_PORT}" \
  --user "${ACCOUNT_CLI}" --pass "${PASSWORD_CLI}" \
  --realm "${REALM_ID}" --build "${CLIENT_BUILD}" \
  --duration "${CLIENT_DURATION}" 2>&1)"
PROBE_RC=$?
set -e
echo "${PROBE_OUT}"

# --- 7. Assert the client saw the bot enter + move. --------------------------
RESULT_LINE="$(printf '%s\n' "${PROBE_OUT}" | grep '^CLIENT_PROBE_RESULT ' | head -1)"
[ -n "${RESULT_LINE}" ] || { err "probe emitted no CLIENT_PROBE_RESULT line"; exit 1; }

_field() { printf '%s\n' "${RESULT_LINE}" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"; }
LOGIN_OK="$(_field login_ok)"
ENTERED="$(_field entered)"
SAW_ENTER="$(_field saw_peer_enter)"
SAW_MOVE="$(_field saw_peer_move)"
UPDATES="$(_field updates)"
TRACKED="$(_field tracked_remote)"

echo
it_fail=0

[ "${LOGIN_OK}" = "1" ] && ok "CLIENT LOGGED IN (SRP over TLS to real authd)" \
  || { err "client did not log in (login_ok=${LOGIN_OK})"; it_fail=1; }

[ "${ENTERED}" = "1" ] && ok "CLIENT ENTERED THE WORLD (HandshakeOk from real worldd, GUI net path)" \
  || { err "client did not enter the world (entered=${ENTERED})"; it_fail=1; }

[ "${SAW_ENTER}" = "1" ] && ok "CLIENT SAW THE BOT ENTER (EntityEnter over the #87 relay)" \
  || { err "client did NOT see the bot enter (saw_peer_enter=${SAW_ENTER})"; it_fail=1; }

# THE HEADLINE — the client saw the bot MOVE: a stream of EntityUpdate.
if [ "${SAW_MOVE}" = "1" ]; then
  ok "CLIENT SAW THE BOT MOVE — ${UPDATES} EntityUpdate received on the GUI net path (${TRACKED} tracked)"
else
  err "CLIENT did NOT see the bot move (saw_peer_move=${SAW_MOVE}, updates=${UPDATES})"
  it_fail=1
fi

# The client's grant was CONSUMED by worldd (single-use spent on enter-world).
GRANT_CLI="$(_dbc -N "${AUTH_DB_NAME}" -e \
  "SELECT consumed_at IS NOT NULL FROM session_grant WHERE account_id=${CLI_ACCOUNT_ID} ORDER BY grant_id DESC LIMIT 1;")"
[ "${GRANT_CLI}" = "1" ] && ok "client grant CONSUMED by worldd (single-use spent on enter-world)" \
  || { err "client grant was not consumed (GRANT_CLI=${GRANT_CLI})"; it_fail=1; }

[ "${PROBE_RC}" -eq 0 ] && ok "meridian-client-probe exited 0 (saw a bot enter + move)" \
  || { err "meridian-client-probe exited ${PROBE_RC} (see the report)"; it_fail=1; }

wait "${BOT_PID}" 2>/dev/null || true
BOT_PID=""

[ "${it_fail}" -eq 0 ] || { err "CLIENT-SEES-BOT E2E assertions FAILED"; exit 1; }

echo
ok "GUI CLIENT NET PATH ↔ REAL authd + worldd — SEES A BOT MOVE: ALL ASSERTIONS PASSED"
