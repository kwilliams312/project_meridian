#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# client/test/run_authd_realm_list_soak_it.sh — #510 realm-list SOAK harness.
#
# The single-shot client↔authd login IT (run_authd_login_it.sh) passes on loopback,
# but the live dev realm sees an INTERMITTENT, PER-ACCOUNT "empty realm list" at the
# post-SRP RealmListRequest->RealmList step (#510). This harness reproduces (or
# rules out on loopback) that class of failure by driving MANY distinct accounts —
# and many fresh SRP sessions — through the real client core over a real TLS socket:
#
#   1. boot a THROWAWAY MariaDB on a UNIQUE socket/port (never the dev default),
#   2. load the auth schema,
#   3. build authd + meridian-account + the client soak IT (authd-realm-list-soak-it),
#   4. provision N accounts (prefix0000..prefix{N-1}) + seed ONE realm,
#   5. launch the real authd,
#   6. run the soak binary: log in every account (optionally --repeat R times) and
#      assert each reaches a SessionGrant with a NON-EMPTY realm list,
#   7. tear everything down.
#
# N accounts cover the PER-ACCOUNT SRP verifier width (~1/256 have a zero MSB — the
# #440 class); R repeats cover the PER-SESSION ephemeral B / premaster S / key K
# width (~1/256 sessions). Any account that clears SRP but gets an empty realm list
# is printed with its exact wire outcome.
#
# Requires MariaDB + cmake + flatc + OpenSSL on PATH. Exits non-zero on ANY soak
# login failure.
#
# Usage:  client/test/run_authd_realm_list_soak_it.sh                 # 512 accounts x1
#         MERIDIAN_SOAK_ACCOUNTS=300 MERIDIAN_SOAK_REPEAT=2 client/test/run_authd_realm_list_soak_it.sh
#         MERIDIAN_IT_KEEP=1 client/test/run_authd_realm_list_soak_it.sh   # keep datadir

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

SOAK_ACCOUNTS="${MERIDIAN_SOAK_ACCOUNTS:-512}"
SOAK_REPEAT="${MERIDIAN_SOAK_REPEAT:-1}"
SOAK_WIDTH=4
SOAK_PREFIX="soakprobe"
SOAK_PASSWORD="correct horse battery staple"

IT_TAG="msoak510_$$"
DB_SOCKET="/tmp/${IT_TAG}.sock"
DB_PORT="$(( 3600 + (RANDOM % 200) ))"      # 3600..3799 — away from 3306/3307 and the login-IT band
DB_DATADIR="${REPO_ROOT}/.dev-run/${IT_TAG}-data"
DB_LOG="${REPO_ROOT}/.dev-run/${IT_TAG}.log"
DB_PIDFILE="${REPO_ROOT}/.dev-run/${IT_TAG}.pid"
DB_NAME="meridian_auth"

CERT_DIR="$(mktemp -d "/tmp/${IT_TAG}-cert.XXXXXX")"
CERT_PEM="${CERT_DIR}/cert.pem"
KEY_PEM="${CERT_DIR}/key.pem"

AUTHD_PORT="$(( 7500 + (RANDOM % 200) ))"   # away from 7100/7200 and the login-IT band
AUTHD_PID=""
AUTHD_LOG="${REPO_ROOT}/.dev-run/${IT_TAG}-authd.log"

CLIENT_BUILD=1000
REALM_NAME="Soak Realm $$"

log()  { printf '\033[1;34m[soak]\033[0m %s\n' "$*"; }
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
    log "kept datadir: ${DB_DATADIR}  authd log: ${AUTHD_LOG}"
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
ok "auth schema loaded"

# --- 3. Build authd + meridian-account + the soak IT. ------------------------
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

log "building the client realm-list soak IT (client tree)"
cmake -S "${REPO_ROOT}/client" -B "${CLI_BUILD}" -DMERIDIAN_CLIENT_TESTS=ON >/dev/null
cmake --build "${CLI_BUILD}" --target authd-realm-list-soak-it -j >/dev/null
IT_BIN="$(find "${CLI_BUILD}" -name authd-realm-list-soak-it -type f -perm -u+x | head -1)"
[ -x "${IT_BIN}" ] || { err "authd-realm-list-soak-it not built"; exit 1; }
ok "built client authd-realm-list-soak-it"

# --- 4. Provision N accounts + seed ONE realm. -------------------------------
export MERIDIAN_DB_HOST=127.0.0.1
export MERIDIAN_DB_PORT="${DB_PORT}"
export MERIDIAN_DB_USER=root
export MERIDIAN_DB_NAME="${DB_NAME}"
unset MERIDIAN_DB_PASS MERIDIAN_DB_SOCKET

log "provisioning ${SOAK_ACCOUNTS} accounts '${SOAK_PREFIX}$(printf '%0*d' "${SOAK_WIDTH}" 0)'..'${SOAK_PREFIX}$(printf '%0*d' "${SOAK_WIDTH}" $((SOAK_ACCOUNTS-1)))'"
for i in $(seq 0 $((SOAK_ACCOUNTS - 1))); do
  u="$(printf '%s%0*d' "${SOAK_PREFIX}" "${SOAK_WIDTH}" "$i")"
  printf '%s' "${SOAK_PASSWORD}" | "${ACCOUNT_BIN}" create --username "${u}" >/dev/null \
    || { err "account create failed for ${u}"; exit 1; }
done
PROVISIONED="$(_dbc -N "${DB_NAME}" -e \
  "SELECT COUNT(*) FROM account WHERE username LIKE '${SOAK_PREFIX}%';")"
ok "provisioned ${PROVISIONED} accounts"
[ "${PROVISIONED}" = "${SOAK_ACCOUNTS}" ] || { err "expected ${SOAK_ACCOUNTS} accounts, have ${PROVISIONED}"; exit 1; }

# Report how many of the provisioned verifiers are SHORT (zero-MSB) — the #440
# class of length-sensitivity — so the run states the coverage it actually hit.
SHORT_VERIFIERS="$(_dbc -N "${DB_NAME}" -e \
  "SELECT COUNT(*) FROM account WHERE username LIKE '${SOAK_PREFIX}%' AND LENGTH(srp_verifier) < 256;")"
log "verifiers shorter than 256 bytes (zero-MSB, #440 class): ${SHORT_VERIFIERS}/${PROVISIONED}"

log "seeding realm '${REALM_NAME}'"
_dbc "${DB_NAME}" -e \
  "INSERT INTO realm (name, address, port, build_min, build_max, population, flags) \
   VALUES ('${REALM_NAME}', '127.0.0.1', 7200, 0, 100000, 0, 0);"
REALM_ID="$(_dbc -N "${DB_NAME}" -e "SELECT id FROM realm WHERE name='${REALM_NAME}';")"
[ -n "${REALM_ID}" ] || { err "realm not seeded"; exit 1; }
ok "realm seeded (id ${REALM_ID})"

# --- 5. Self-signed cert + launch the real authd. ----------------------------
log "generating self-signed TLS cert for authd"
openssl req -x509 -newkey rsa:2048 -keyout "${KEY_PEM}" -out "${CERT_PEM}" \
  -days 1 -nodes -subj "/CN=meridian-authd-soak" >/dev/null 2>&1

log "launching authd on 127.0.0.1:${AUTHD_PORT} (log: ${AUTHD_LOG})"
"${AUTHD_BIN}" --cert "${CERT_PEM}" --key "${KEY_PEM}" \
  --bind 127.0.0.1 --port "${AUTHD_PORT}" --realm reference \
  --log-level debug >"${AUTHD_LOG}" 2>&1 &
AUTHD_PID=$!
for i in $(seq 1 40); do
  if nc -z 127.0.0.1 "${AUTHD_PORT}" 2>/dev/null; then break; fi
  kill -0 "${AUTHD_PID}" 2>/dev/null || { err "authd exited during startup"; tail -20 "${AUTHD_LOG}" >&2; exit 1; }
  sleep 0.25
  [ "$i" = "40" ] && { err "authd did not open its port"; exit 1; }
done
ok "authd listening on ${AUTHD_PORT}"

# --- 6. Drive the SOAK. ------------------------------------------------------
log "running realm-list soak: ${SOAK_ACCOUNTS} accounts x ${SOAK_REPEAT} repeat(s)"
set +e
"${IT_BIN}" \
  --host 127.0.0.1 --port "${AUTHD_PORT}" \
  --soak-prefix "${SOAK_PREFIX}" --soak-width "${SOAK_WIDTH}" \
  --soak-count "${SOAK_ACCOUNTS}" --repeat "${SOAK_REPEAT}" \
  --password "${SOAK_PASSWORD}" \
  --realm-id "${REALM_ID}" --build "${CLIENT_BUILD}"
SOAK_RC=$?
set -e

echo
# Surface any authd-side realm-list diagnostics captured during the soak (#510
# instrumentation: row count + frame bytes; malformed RealmListRequest paths).
log "authd realm-list diagnostics from this run (${AUTHD_LOG}):"
grep -E 'realm[_-]?list|RealmListRequest|malformed|realm_rows|realmlist' "${AUTHD_LOG}" 2>/dev/null \
  | tail -20 || true

if [ "${SOAK_RC}" -ne 0 ]; then
  err "SOAK FAILED (rc=${SOAK_RC}) — an account that cleared SRP got an empty/failed realm list (see above)"
  exit 1
fi

echo
ok "REALM-LIST SOAK: every login reached a grant with a non-empty realm list"
