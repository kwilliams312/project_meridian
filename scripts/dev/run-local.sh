#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/run-local.sh — bring up a local Meridian realm on macOS (#223).
#
# Brings up: a throwaway MariaDB (auth+characters+world schemas) + a self-signed
# TLS cert + a test account (via meridian-account) + authd (IF-1, :7100) and
# worldd (IF-2, :7200) as background daemons. Prints the ports/PIDs and how to
# stop. This is the native analogue of `docker compose up` in deploy/docker/.
#
# Modes:
#   (default)   Start DB + authd + worldd in the BACKGROUND and return, printing
#               ports + a stop command. Logs under .dev-run/.
#   --smoke     Start everything, verify authd + worldd are listening and a TLS
#               1.3 client can reach each, then stop everything and exit.
#   --foreground  Start and block until Ctrl-C (then tear everything down).
#   --stop      Stop a previously-started background realm and remove the datadir.
#
# Requires a prior build (scripts/dev/build.sh).
#
# Ports/env (see docs/BUILDING-MACOS.md):
#   authd   IF-1  127.0.0.1:7100 (TLS 1.3)
#   worldd  IF-2  127.0.0.1:7200 (TLS 1.3)
#   MariaDB       127.0.0.1:3307 (throwaway, socket /tmp/mmdb.sock)
#
# Logging (OPS-05 #165): the daemons default to structured JSON logs (one Loki-
# ingestable object per line on stdout). This script launches them with
# `--log-format text` so the .dev-run/*.log files stay human-readable for local
# development. Drop the flag (or set MERIDIAN_LOG_FORMAT=json) to see the prod
# JSON shape. `--log-level trace|debug|info|warn|error` sets the floor.
set -euo pipefail

# shellcheck source=scripts/dev/_common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
# shellcheck source=scripts/dev/_db.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_db.sh"

MODE="background"
while [ $# -gt 0 ]; do
  case "$1" in
    --smoke)      MODE="smoke" ;;
    --foreground) MODE="foreground" ;;
    --stop)       MODE="stop" ;;
    -h|--help)
      grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'
      exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
  shift
done

require_macos
setup_cmake_env

AUTHD="${BUILD_ROOT}/server/authd/authd"
WORLDD="${BUILD_ROOT}/server/worldd/worldd"
ACCOUNT="${BUILD_ROOT}/server/tools/meridian-account/meridian-account"
CERT_DIR="${REPO_ROOT}/scripts/dev/certs"
CERT="${CERT_DIR}/server.crt"
KEY="${CERT_DIR}/server.key"
RUN_DIR="${REPO_ROOT}/.dev-run"
AUTHD_PIDFILE="${RUN_DIR}/authd.pid"
WORLDD_PIDFILE="${RUN_DIR}/worldd.pid"

AUTHD_PORT=7100
WORLDD_PORT=7200
TEST_USER="devtester"
TEST_PASS="devpassword"

# --- --stop: tear down a previously-started background realm and exit. --------
stop_realm() {
  for pf in "${AUTHD_PIDFILE}" "${WORLDD_PIDFILE}"; do
    if [ -f "$pf" ]; then
      local p; p="$(cat "$pf")"
      if kill -0 "$p" 2>/dev/null; then
        log "Stopping $(basename "$pf" .pid) (pid $p)"
        kill "$p" 2>/dev/null || true
      fi
      rm -f "$pf"
    fi
  done
  # Reuse the _db.sh datadir/socket/pidfile paths to stop a leftover DB.
  if [ -f "${REPO_ROOT}/.dev-run/mariadb.pid" ]; then
    local dbp; dbp="$(cat "${REPO_ROOT}/.dev-run/mariadb.pid")"
    if kill -0 "$dbp" 2>/dev/null; then
      log "Stopping MariaDB (pid $dbp)"
      mariadb-admin --no-defaults --socket="${MERIDIAN_DEV_DB_SOCKET}" --user=root shutdown >/dev/null 2>&1 \
        || kill "$dbp" 2>/dev/null || true
    fi
  fi
  rm -rf "${MERIDIAN_DEV_DB_DATADIR}"
  rm -f "${MERIDIAN_DEV_DB_SOCKET}" "${REPO_ROOT}/.dev-run/mariadb.pid"
  ok "Local realm stopped."
}

if [ "$MODE" = "stop" ]; then
  stop_realm
  exit 0
fi

[ -x "$AUTHD" ]   || die "authd not built. Run: scripts/dev/build.sh"
[ -x "$WORLDD" ]  || die "worldd not built. Run: scripts/dev/build.sh"
[ -x "$ACCOUNT" ] || die "meridian-account not built. Run: scripts/dev/build.sh"

mkdir -p "${RUN_DIR}"

# --- Self-signed TLS cert (generate once, reuse). ----------------------------
if [ ! -f "$CERT" ] || [ ! -f "$KEY" ]; then
  log "Generating self-signed TLS cert (${CERT_DIR}/server.{crt,key})"
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$KEY" -out "$CERT" \
    -days 365 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 \
    || die "openssl cert generation failed"
  ok "TLS cert generated"
else
  ok "Reusing TLS cert at ${CERT}"
fi

# --- Track daemon PIDs for cleanup. ------------------------------------------
AUTHD_PID=""
WORLDD_PID=""

cleanup_daemons() {
  # `disown` first so the shell's job-control monitor doesn't print a noisy
  # "Terminated: 15" line when we kill each backgrounded daemon.
  [ -n "$AUTHD_PID" ]  && { disown "$AUTHD_PID"  2>/dev/null || true; kill "$AUTHD_PID"  2>/dev/null || true; }
  [ -n "$WORLDD_PID" ] && { disown "$WORLDD_PID" 2>/dev/null || true; kill "$WORLDD_PID" 2>/dev/null || true; }
  rm -f "${AUTHD_PIDFILE}" "${WORLDD_PIDFILE}"
}

# --- Bring up the DB. --------------------------------------------------------
db_start
db_load_schemas

# --- Create a test account (auth DB). ----------------------------------------
log "Creating test account '${TEST_USER}' via meridian-account"
db_export_env meridian_auth
if printf '%s\n' "${TEST_PASS}" | "$ACCOUNT" create --username "${TEST_USER}"; then
  ok "Test account '${TEST_USER}' created (password '${TEST_PASS}')"
else
  rc=$?
  # rc 3 = duplicate (already exists from a prior run with --keep-db) — tolerate.
  if [ "$rc" -eq 3 ]; then
    ok "Test account '${TEST_USER}' already exists"
  else
    db_stop; die "meridian-account create failed (rc=$rc)"
  fi
fi

# --- Launch daemons. authd needs the auth DB env; worldd needs auth+chars DB. -
log "Launching authd (IF-1) on 127.0.0.1:${AUTHD_PORT}"
env MERIDIAN_DB_HOST=127.0.0.1 MERIDIAN_DB_PORT="${MERIDIAN_DEV_DB_PORT}" \
    MERIDIAN_DB_USER=root MERIDIAN_DB_NAME=meridian_auth \
  "$AUTHD" --cert "$CERT" --key "$KEY" --bind 127.0.0.1 --port "${AUTHD_PORT}" \
    --login.grant_ttl_seconds="${MERIDIAN_GRANT_TTL_SECONDS:-600}" \
    --log-format text \
  >"${RUN_DIR}/authd.log" 2>&1 &
AUTHD_PID=$!
echo "$AUTHD_PID" > "${AUTHD_PIDFILE}"

log "Launching worldd (IF-2) on 127.0.0.1:${WORLDD_PORT}"
env MERIDIAN_DB_HOST=127.0.0.1 MERIDIAN_DB_PORT="${MERIDIAN_DEV_DB_PORT}" \
    MERIDIAN_DB_USER=root MERIDIAN_DB_NAME=meridian_auth \
    MERIDIAN_CHARDB_HOST=127.0.0.1 MERIDIAN_CHARDB_PORT="${MERIDIAN_DEV_DB_PORT}" \
    MERIDIAN_CHARDB_USER=root MERIDIAN_CHARDB_NAME=meridian_characters \
  "$WORLDD" --cert "$CERT" --key "$KEY" --bind 127.0.0.1 --port "${WORLDD_PORT}" \
    --log-format text \
  >"${RUN_DIR}/worldd.log" 2>&1 &
WORLDD_PID=$!
echo "$WORLDD_PID" > "${WORLDD_PIDFILE}"

# --- Wait for both to bind (poll the port, not a fixed sleep). ---------------
wait_listen() {
  local name="$1" port="$2" pid="$3" logf="$4" i
  # shellcheck disable=SC2034  # i is the loop counter for the retry budget
  for i in $(seq 1 40); do
    if ! kill -0 "$pid" 2>/dev/null; then
      warn "${name} exited during startup — log:"; tail -15 "$logf" >&2; return 1
    fi
    # A TLS 1.3 handshake proves the listener is up AND serving TLS (a plain
    # port-open check would pass before the cert is loaded).
    if echo | openssl s_client -connect "127.0.0.1:${port}" -tls1_3 >/dev/null 2>&1; then
      ok "${name} listening + TLS 1.3 reachable on 127.0.0.1:${port} (pid ${pid})"
      return 0
    fi
    sleep 0.25
  done
  warn "${name} did not become reachable on :${port} — log:"; tail -15 "$logf" >&2
  return 1
}

authd_ok=0; worldd_ok=0
wait_listen "authd"  "${AUTHD_PORT}"  "$AUTHD_PID"  "${RUN_DIR}/authd.log"  && authd_ok=1
wait_listen "worldd" "${WORLDD_PORT}" "$WORLDD_PID" "${RUN_DIR}/worldd.log" && worldd_ok=1

# --- Mode dispatch. ----------------------------------------------------------
case "$MODE" in
  smoke)
    log "=== --smoke: verifying reachability, then tearing down ==="
    rc=0
    [ "$authd_ok" -eq 1 ]  || { warn "authd smoke FAILED";  rc=1; }
    [ "$worldd_ok" -eq 1 ] || { warn "worldd smoke FAILED"; rc=1; }
    cleanup_daemons
    db_stop
    echo
    if [ "$rc" -eq 0 ]; then
      ok "SMOKE PASSED — authd + worldd start, bind, and accept TLS 1.3 clients."
    else
      die "SMOKE FAILED (see logs in ${RUN_DIR})."
    fi
    ;;

  foreground)
    if [ "$authd_ok" -ne 1 ] || [ "$worldd_ok" -ne 1 ]; then
      cleanup_daemons; db_stop; die "a daemon failed to start (see ${RUN_DIR})."
    fi
    trap 'echo; log "Shutting down..."; cleanup_daemons; db_stop; exit 0' INT TERM
    echo
    ok "Local realm UP (foreground). Ctrl-C to stop."
    print_summary() {
      echo "  authd    127.0.0.1:${AUTHD_PORT}   (log: ${RUN_DIR}/authd.log)"
      echo "  worldd   127.0.0.1:${WORLDD_PORT}   (log: ${RUN_DIR}/worldd.log)"
      echo "  MariaDB  127.0.0.1:${MERIDIAN_DEV_DB_PORT}   account: ${TEST_USER} / ${TEST_PASS}"
    }
    print_summary
    # Block until a daemon dies or Ctrl-C.
    wait "$AUTHD_PID" "$WORLDD_PID"
    ;;

  background)
    if [ "$authd_ok" -ne 1 ] || [ "$worldd_ok" -ne 1 ]; then
      cleanup_daemons; db_stop; die "a daemon failed to start (see ${RUN_DIR})."
    fi
    # Keep the DB datadir; the daemons keep running detached. Do NOT run db_stop.
    echo
    ok "Local realm UP (background)."
    echo "  authd    127.0.0.1:${AUTHD_PORT}   (pid $(cat "${AUTHD_PIDFILE}"),  log: ${RUN_DIR}/authd.log)"
    echo "  worldd   127.0.0.1:${WORLDD_PORT}   (pid $(cat "${WORLDD_PIDFILE}"), log: ${RUN_DIR}/worldd.log)"
    echo "  MariaDB  127.0.0.1:${MERIDIAN_DEV_DB_PORT}   account: ${TEST_USER} / ${TEST_PASS}"
    echo
    echo "  Stop with:  scripts/dev/run-local.sh --stop"
    # Detach: don't let EXIT kill the backgrounded daemons.
    AUTHD_PID=""; WORLDD_PID=""; _DB_PID=""
    ;;
esac
