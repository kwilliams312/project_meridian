#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# client/test/run_authd_login_it.sh — the CLIENT↔authd login INTEGRATION harness
# (IT-M0 auth path, #99).
#
# THE KILLER TEST driver. It proves the client login core interoperates with the
# REAL server auth path end to end:
#   1. boot a THROWAWAY MariaDB on a UNIQUE socket/port (NOT the dev default
#      /tmp/mmdb.sock:3307 — a distinct path so it never collides with a running
#      dev DB or a parallel server-test harness),
#   2. load the auth schema (server/db/auth/migrations/*.up.sql),
#   3. build authd + meridian-account (server tree) + the client IT test,
#   4. create a test account (meridian-account) + seed a realm,
#   5. launch the real authd (TLS 1.3 listener, self-signed cert),
#   6. run client/test/authd_login_it — the client login CORE over TLS — and assert
#      a full SRP login succeeds (M2 verifies), the realm list returns, RealmSelect
#      yields a single-use grant, and a wrong password is rejected,
#   7. verify the grant row in DB (persisted, bound to the account/realm, 32-byte
#      key) and that it is SINGLE-USE (first consume 1 row, second 0),
#   8. tear everything down.
#
# Requires MariaDB (mariadbd, mariadb-install-db, mariadb) + cmake + flatc +
# OpenSSL on PATH (all present on the dev box). Exits non-zero on any assertion
# failure; prints ACTUAL client + DB output so the run is honest about what ran.
#
# Usage:  client/test/run_authd_login_it.sh
#         MERIDIAN_IT_KEEP=1 client/test/run_authd_login_it.sh   # keep the datadir

set -euo pipefail

# --- Locate the repo root (this script lives at client/test/). ---------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

# --- UNIQUE, isolated MariaDB instance (NOT the dev default). -----------------
# A per-run socket + port keyed on the PID so this NEVER touches a system MySQL,
# the dev DB (scripts/dev/_db.sh, /tmp/mmdb.sock:3307), or a parallel run. The
# socket stays in /tmp for the macOS 104-char sun_path limit.
IT_TAG="mlogin99_$$"
DB_SOCKET="/tmp/${IT_TAG}.sock"
DB_PORT="$(( 3400 + (RANDOM % 200) ))"      # 3400..3599 — away from 3306/3307
DB_DATADIR="${REPO_ROOT}/.dev-run/${IT_TAG}-data"
DB_LOG="${REPO_ROOT}/.dev-run/${IT_TAG}.log"
DB_PIDFILE="${REPO_ROOT}/.dev-run/${IT_TAG}.pid"
DB_NAME="meridian_auth"

CERT_DIR="$(mktemp -d "/tmp/${IT_TAG}-cert.XXXXXX")"
CERT_PEM="${CERT_DIR}/cert.pem"
KEY_PEM="${CERT_DIR}/key.pem"

AUTHD_PORT="$(( 7300 + (RANDOM % 200) ))"   # away from the 7100/7200 defaults
AUTHD_PID=""

CLIENT_BUILD=1000
ACCOUNT="client_it_$$"
PASSWORD="correct horse battery staple"
WRONG_PASSWORD="Tr0ub4dour&3"
REALM_NAME="Client IT Realm $$"

log()  { printf '\033[1;34m[it]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ok]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[ERR]\033[0m %s\n' "$*" >&2; }

_dbc() { mariadb --no-defaults --socket="${DB_SOCKET}" --user=root "$@"; }

cleanup() {
  set +e
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

# --- 2. Load the auth schema. ------------------------------------------------
_dbc -e "CREATE DATABASE IF NOT EXISTS ${DB_NAME} CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
for f in "${REPO_ROOT}"/server/db/auth/migrations/*.up.sql; do
  _dbc "${DB_NAME}" < "$f" || { err "migration failed: $(basename "$f")"; exit 1; }
done
ok "auth schema loaded ($(_dbc -N "${DB_NAME}" -e \
  'SELECT COUNT(*) FROM information_schema.tables WHERE table_schema="'"${DB_NAME}"'";') tables)"

# --- 3. Build authd + meridian-account + the client IT test. -----------------
# authd + meridian-account come from the server tree (one CMake configure); the
# client IT test comes from the client test tree (its own configure). Both are
# out-of-source builds under .dev-run so they never touch the committed tree.
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
SRV_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-server-build"
CLI_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-client-build"

log "building authd + meridian-account (server tree)"
cmake -S "${REPO_ROOT}/server" -B "${SRV_BUILD}" >/dev/null
cmake --build "${SRV_BUILD}" --target authd meridian-account -j >/dev/null
AUTHD_BIN="$(find "${SRV_BUILD}" -name authd -type f -perm -u+x | head -1)"
ACCOUNT_BIN="$(find "${SRV_BUILD}" -name meridian-account -type f -perm -u+x | head -1)"
[ -x "${AUTHD_BIN}" ] || { err "authd not built"; exit 1; }
[ -x "${ACCOUNT_BIN}" ] || { err "meridian-account not built"; exit 1; }
ok "built authd + meridian-account"

log "building the client login integration test (client tree)"
cmake -S "${REPO_ROOT}/client" -B "${CLI_BUILD}" -DMERIDIAN_CLIENT_TESTS=ON >/dev/null
cmake --build "${CLI_BUILD}" --target authd-login-it -j >/dev/null
IT_BIN="$(find "${CLI_BUILD}" -name authd-login-it -type f -perm -u+x | head -1)"
[ -x "${IT_BIN}" ] || { err "authd-login-it not built"; exit 1; }
ok "built client authd-login-it"

# --- 4. Create the account + seed a realm. -----------------------------------
export MERIDIAN_DB_HOST=127.0.0.1
export MERIDIAN_DB_PORT="${DB_PORT}"
export MERIDIAN_DB_USER=root
export MERIDIAN_DB_NAME="${DB_NAME}"
unset MERIDIAN_DB_PASS MERIDIAN_DB_SOCKET

log "creating test account '${ACCOUNT}' via meridian-account"
printf '%s' "${PASSWORD}" | "${ACCOUNT_BIN}" create --username "${ACCOUNT}" >/dev/null
ACCOUNT_ID="$(_dbc -N "${DB_NAME}" -e \
  "SELECT id FROM account WHERE username='${ACCOUNT}';")"
[ -n "${ACCOUNT_ID}" ] || { err "account not created"; exit 1; }
ok "account created (id ${ACCOUNT_ID})"

log "seeding realm '${REALM_NAME}'"
_dbc "${DB_NAME}" -e \
  "INSERT INTO realm (name, address, port, build_min, build_max, population, flags) \
   VALUES ('${REALM_NAME}', '127.0.0.1', 7200, 0, 100000, 0, 0);"
REALM_ID="$(_dbc -N "${DB_NAME}" -e \
  "SELECT id FROM realm WHERE name='${REALM_NAME}';")"
[ -n "${REALM_ID}" ] || { err "realm not seeded"; exit 1; }
ok "realm seeded (id ${REALM_ID})"

# --- 5. Self-signed cert + launch the real authd. ----------------------------
log "generating self-signed TLS cert for authd"
openssl req -x509 -newkey rsa:2048 -keyout "${KEY_PEM}" -out "${CERT_PEM}" \
  -days 1 -nodes -subj "/CN=meridian-authd-it" >/dev/null 2>&1

log "launching authd on 127.0.0.1:${AUTHD_PORT}"
"${AUTHD_BIN}" --cert "${CERT_PEM}" --key "${KEY_PEM}" \
  --bind 127.0.0.1 --port "${AUTHD_PORT}" --realm reference &
AUTHD_PID=$!
# Wait for the listener to accept TCP connections.
for i in $(seq 1 40); do
  if nc -z 127.0.0.1 "${AUTHD_PORT}" 2>/dev/null; then break; fi
  kill -0 "${AUTHD_PID}" 2>/dev/null || { err "authd exited during startup"; exit 1; }
  sleep 0.25
  [ "$i" = "40" ] && { err "authd did not open its port"; exit 1; }
done
ok "authd listening on ${AUTHD_PORT}"

# --- 6. Drive the CLIENT login core against the real authd. ------------------
log "running client authd-login-it (client core over TLS 1.3)"
CLIENT_OUT="$("${IT_BIN}" \
  --host 127.0.0.1 --port "${AUTHD_PORT}" \
  --account "${ACCOUNT}" --password "${PASSWORD}" \
  --wrong-password "${WRONG_PASSWORD}" \
  --realm-id "${REALM_ID}" --build "${CLIENT_BUILD}")"
CLIENT_RC=$?
echo "${CLIENT_OUT}"
[ "${CLIENT_RC}" -eq 0 ] || { err "client integration test FAILED (rc=${CLIENT_RC})"; exit 1; }

# --- 7. Verify the grant row in DB (persisted + single-use). -----------------
GRANT_ID="$(printf '%s\n' "${CLIENT_OUT}" | sed -n 's/^GRANT_ID=//p' | head -1)"
[ -n "${GRANT_ID}" ] || { err "client did not emit a GRANT_ID"; exit 1; }
log "verifying grant ${GRANT_ID} in DB (persisted, bound, single-use)"

ROW="$(_dbc -N "${DB_NAME}" -e \
  "SELECT account_id, realm_id, LENGTH(session_key), consumed_at IS NULL \
   FROM session_grant WHERE grant_id=${GRANT_ID};")"
[ -n "${ROW}" ] || { err "grant row not found in DB"; exit 1; }
DB_ACCOUNT="$(echo "${ROW}" | awk '{print $1}')"
DB_REALM="$(echo "${ROW}" | awk '{print $2}')"
DB_KEYLEN="$(echo "${ROW}" | awk '{print $3}')"
DB_UNCONSUMED="$(echo "${ROW}" | awk '{print $4}')"
grant_fail=0
[ "${DB_ACCOUNT}" = "${ACCOUNT_ID}" ] && ok "grant bound to the right account (${DB_ACCOUNT})" || { err "grant account ${DB_ACCOUNT} != ${ACCOUNT_ID}"; grant_fail=1; }
[ "${DB_REALM}" = "${REALM_ID}" ]     && ok "grant bound to the selected realm (${DB_REALM})" || { err "grant realm ${DB_REALM} != ${REALM_ID}"; grant_fail=1; }
[ "${DB_KEYLEN}" = "32" ]             && ok "stored session_key is 32 bytes"                  || { err "session_key len ${DB_KEYLEN} != 32"; grant_fail=1; }
[ "${DB_UNCONSUMED}" = "1" ]          && ok "grant is initially unconsumed"                   || { err "grant already consumed"; grant_fail=1; }

# Single-use: first atomic consume affects 1 row, second affects 0.
C1="$(_dbc -N "${DB_NAME}" -e \
  "UPDATE session_grant SET consumed_at=UTC_TIMESTAMP() WHERE grant_id=${GRANT_ID} AND consumed_at IS NULL; SELECT ROW_COUNT();")"
C2="$(_dbc -N "${DB_NAME}" -e \
  "UPDATE session_grant SET consumed_at=UTC_TIMESTAMP() WHERE grant_id=${GRANT_ID} AND consumed_at IS NULL; SELECT ROW_COUNT();")"
[ "${C1}" = "1" ] && ok "first consume affects exactly 1 row"        || { err "first consume affected ${C1} rows"; grant_fail=1; }
[ "${C2}" = "0" ] && ok "second consume affects 0 rows (single-use)" || { err "second consume affected ${C2} rows"; grant_fail=1; }

# Wrong password wrote no grant: exactly ONE grant exists for the account.
GRANT_COUNT="$(_dbc -N "${DB_NAME}" -e \
  "SELECT COUNT(*) FROM session_grant WHERE account_id=${ACCOUNT_ID};")"
[ "${GRANT_COUNT}" = "1" ] && ok "wrong password wrote NO extra grant (exactly 1 total)" \
  || { err "expected 1 grant for the account, found ${GRANT_COUNT}"; grant_fail=1; }

[ "${grant_fail}" -eq 0 ] || { err "grant DB assertions FAILED"; exit 1; }

echo
ok "CLIENT ↔ REAL authd LOGIN INTEGRATION: ALL ASSERTIONS PASSED"
