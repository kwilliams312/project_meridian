# SPDX-License-Identifier: Apache-2.0
# shellcheck shell=bash
#
# scripts/dev/_db.sh — throwaway local MariaDB lifecycle for the dev toolkit (#223).
#
# Sourced by test.sh (--db) and run-local.sh. Spins up a private MariaDB in a
# TEMP datadir on a NON-STANDARD port (3307) + a short /tmp socket, loads the
# auth + characters + world schemas, and tears it down cleanly. It NEVER touches
# a system MySQL/MariaDB: separate datadir, separate port, no --defaults.
#
# Why /tmp/mmdb.sock and not a socket under the datadir: the Unix domain socket
# path has a hard ~104-char limit on macOS (sockaddr_un.sun_path). A datadir deep
# under the repo worktree easily blows past that, so we force a short /tmp path.
#
# Config (override via env before sourcing/calling):
#   MERIDIAN_DEV_DB_PORT   (default 3307)
#   MERIDIAN_DEV_DB_SOCKET (default /tmp/mmdb.sock)
#   MERIDIAN_DEV_DB_DATADIR(default $REPO_ROOT/.dev-run/mariadb-data)

: "${MERIDIAN_DEV_DB_PORT:=3307}"
: "${MERIDIAN_DEV_DB_SOCKET:=/tmp/mmdb.sock}"
: "${MERIDIAN_DEV_DB_DATADIR:=${REPO_ROOT}/.dev-run/mariadb-data}"

_DB_PIDFILE="${REPO_ROOT}/.dev-run/mariadb.pid"
_DB_LOGFILE="${REPO_ROOT}/.dev-run/mariadb.log"
_DB_PID=""

# mariadb client/admin invoked against OUR instance only (never system defaults).
_dbc()     { mariadb       --no-defaults --socket="${MERIDIAN_DEV_DB_SOCKET}" --user=root "$@"; }
_dbadmin() { mariadb-admin --no-defaults --socket="${MERIDIAN_DEV_DB_SOCKET}" --user=root "$@"; }

# PIDs of mariadbd processes that belong to THIS realm — matched by our datadir OR
# socket appearing in their argv. This is the safe alternative to a blanket
# `pkill mariadbd`: a system MariaDB (or another dev realm on a different
# port/socket, e.g. a concurrent run-local under a different override) has a
# different datadir/socket and is never selected. pgrep -x scopes to the exact
# process name; the case-glob then keeps only the ones that are ours.
_db_realm_pids() {
  local pids p cmd
  pids="$(pgrep -x mariadbd 2>/dev/null || true)"
  for p in $pids; do
    cmd="$(ps -o command= -p "$p" 2>/dev/null || true)"
    case "$cmd" in
      *"${MERIDIAN_DEV_DB_DATADIR}"*|*"${MERIDIAN_DEV_DB_SOCKET}"*)
        printf '%s\n' "$p" ;;
    esac
  done
}

# Bring THIS realm's mariadbd down for good, WITHOUT touching datadir/socket
# (callers remove those only after this confirms the server is gone). Robust to
# the orphaned-server / stale-pidfile case that pidfile-only shutdown misses:
#   1. graceful `shutdown` over our socket if it still answers,
#   2. SIGTERM → poll up to ~10s → SIGKILL every candidate pid: the tracked
#      _DB_PID, the pidfile pid, AND any orphan matched by datadir/socket,
#   3. return non-zero if the socket is STILL answering afterwards, so callers
#      know not to delete the datadir out from under a live server.
# Kills are always realm-scoped (see _db_realm_pids) — never a bare pkill.
_db_force_down() {
  if [ -S "${MERIDIAN_DEV_DB_SOCKET}" ] && _dbadmin ping >/dev/null 2>&1; then
    log "Shutting down MariaDB (socket ${MERIDIAN_DEV_DB_SOCKET})"
    _dbadmin shutdown >/dev/null 2>&1 || true
  fi

  local pidfile_pid="" targets p i alive
  [ -f "${_DB_PIDFILE}" ] && pidfile_pid="$(cat "${_DB_PIDFILE}" 2>/dev/null || true)"
  targets="$(printf '%s\n%s\n%s\n' "${_DB_PID}" "${pidfile_pid}" "$(_db_realm_pids)" \
             | grep -E '^[0-9]+$' | sort -u || true)"

  for p in $targets; do kill -0 "$p" 2>/dev/null && kill "$p" 2>/dev/null || true; done
  # shellcheck disable=SC2034  # i is the retry-budget loop counter
  for i in $(seq 1 20); do
    alive=""
    for p in $targets; do kill -0 "$p" 2>/dev/null && alive="${alive} $p"; done
    [ -z "$alive" ] && break
    sleep 0.5
  done
  for p in $targets; do kill -0 "$p" 2>/dev/null && kill -9 "$p" 2>/dev/null || true; done

  if [ -S "${MERIDIAN_DEV_DB_SOCKET}" ] && _dbadmin ping >/dev/null 2>&1; then
    warn "MariaDB still answering on ${MERIDIAN_DEV_DB_SOCKET} after shutdown + SIGKILL"
    return 1
  fi
  return 0
}

# Start a fresh throwaway MariaDB. Wipes any prior datadir (throwaway by design).
db_start() {
  local sock_len=${#MERIDIAN_DEV_DB_SOCKET}
  if [ "$sock_len" -ge 104 ]; then
    die "socket path too long (${sock_len} ≥ 104): ${MERIDIAN_DEV_DB_SOCKET}. Set MERIDIAN_DEV_DB_SOCKET to a short /tmp path."
  fi

  # Guard against a stale instance from a crashed prior run (or a second
  # concurrent invocation) still holding our socket/port — otherwise we would
  # silently talk to the wrong server and load schemas into a datadir we don't
  # manage. Reap any leftover mariadbd that is demonstrably OURS — a live
  # pidfile OR an orphan whose datadir/socket matches this realm (the pidfile
  # having drifted from the real server pid is exactly the #841 failure). Only
  # after that, if the socket STILL answers, it's a server we don't recognise:
  # refuse rather than clobber it.
  if [ -f "${_DB_PIDFILE}" ] || [ -n "$(_db_realm_pids)" ]; then
    warn "reaping stale mariadbd from a prior run before starting fresh"
    _db_force_down || true
    rm -f "${_DB_PIDFILE}"
  fi
  if [ -S "${MERIDIAN_DEV_DB_SOCKET}" ] && \
     mariadb-admin --no-defaults --socket="${MERIDIAN_DEV_DB_SOCKET}" --user=root ping >/dev/null 2>&1; then
    die "another MariaDB is already answering on ${MERIDIAN_DEV_DB_SOCKET}. Stop it (scripts/dev/run-local.sh --stop) or remove the socket, then retry."
  fi
  rm -f "${MERIDIAN_DEV_DB_SOCKET}"

  mkdir -p "${REPO_ROOT}/.dev-run"
  rm -rf "${MERIDIAN_DEV_DB_DATADIR}"
  mkdir -p "${MERIDIAN_DEV_DB_DATADIR}"

  log "Initializing throwaway MariaDB datadir (${MERIDIAN_DEV_DB_DATADIR})"
  mariadb-install-db --no-defaults \
    --datadir="${MERIDIAN_DEV_DB_DATADIR}" \
    --auth-root-authentication-method=normal \
    >>"${_DB_LOGFILE}" 2>&1 \
    || { tail -20 "${_DB_LOGFILE}" >&2; die "mariadb-install-db failed"; }

  log "Starting mariadbd on port ${MERIDIAN_DEV_DB_PORT} (socket ${MERIDIAN_DEV_DB_SOCKET})"
  mariadbd --no-defaults \
    --datadir="${MERIDIAN_DEV_DB_DATADIR}" \
    --socket="${MERIDIAN_DEV_DB_SOCKET}" \
    --port="${MERIDIAN_DEV_DB_PORT}" \
    --bind-address=127.0.0.1 \
    --pid-file="${_DB_PIDFILE}" \
    >>"${_DB_LOGFILE}" 2>&1 &
  _DB_PID=$!

  # Wait for it to accept connections (poll the condition, don't sleep-guess).
  local i
  # shellcheck disable=SC2034  # i is the retry-budget loop counter
  for i in $(seq 1 60); do
    if _dbadmin ping >/dev/null 2>&1; then
      ok "MariaDB up ($(_dbc -N -e 'SELECT VERSION();' 2>/dev/null))"
      return 0
    fi
    if ! kill -0 "${_DB_PID}" 2>/dev/null; then
      tail -20 "${_DB_LOGFILE}" >&2; die "mariadbd exited during startup"
    fi
    sleep 0.5
  done
  tail -20 "${_DB_LOGFILE}" >&2
  die "MariaDB did not become ready within 30s"
}

# Load all three schemas: auth (up migrations) + characters (up) + world (DDL).
db_load_schemas() {
  log "Creating databases + loading schemas (auth, characters, world)"

  _dbc -e "CREATE DATABASE IF NOT EXISTS meridian_auth       CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
  _dbc -e "CREATE DATABASE IF NOT EXISTS meridian_characters CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
  _dbc -e "CREATE DATABASE IF NOT EXISTS meridian_world      CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"

  # Auth: apply *.up.sql migrations in numeric order (never *.down.sql).
  local f
  for f in "${REPO_ROOT}"/server/db/auth/migrations/*.up.sql; do
    _dbc meridian_auth < "$f" || die "auth migration failed: $(basename "$f")"
  done
  ok "auth schema loaded ($(_dbc -N meridian_auth -e 'SELECT COUNT(*) FROM information_schema.tables WHERE table_schema="meridian_auth";') tables)"

  # Characters: apply *.up.sql migrations in order.
  for f in "${REPO_ROOT}"/server/db/characters/migrations/*.up.sql; do
    _dbc meridian_characters < "$f" || die "characters migration failed: $(basename "$f")"
  done
  ok "characters schema loaded ($(_dbc -N meridian_characters -e 'SELECT COUNT(*) FROM information_schema.tables WHERE table_schema="meridian_characters";') tables)"

  # World: hand-maintained DDL, numbered 00..90. 00_manifest sets
  # FOREIGN_KEY_CHECKS=0, so pipe ALL files through ONE connection (in filename
  # order) to keep that session setting in effect across the whole load.
  cat "${REPO_ROOT}"/schema/sql/world/[0-9]*.sql | _dbc meridian_world \
    || die "world DDL load failed"
  ok "world schema loaded ($(_dbc -N meridian_world -e 'SELECT COUNT(*) FROM information_schema.tables WHERE table_schema="meridian_world";') tables)"
}

# Stop the instance and (unless MERIDIAN_DEV_DB_KEEP=1) remove the datadir.
# Single source of truth for DB teardown — used by test.sh's EXIT trap, the
# run-local.sh --smoke/--foreground paths, AND run-local.sh --stop. Delegates the
# actual shutdown to _db_force_down so an orphaned mariadbd / stale pidfile is
# reaped by datadir+socket, not just the tracked pid; the datadir/socket are
# removed ONLY once the server is confirmed down (never out from under a live one).
db_stop() {
  if ! _db_force_down; then
    warn "MariaDB still up after teardown — leaving datadir/socket intact (would strand a live server)"
    _DB_PID=""
    return 1
  fi
  _DB_PID=""
  if [ "${MERIDIAN_DEV_DB_KEEP:-0}" != "1" ]; then
    rm -rf "${MERIDIAN_DEV_DB_DATADIR}"
    rm -f "${MERIDIAN_DEV_DB_SOCKET}" "${_DB_PIDFILE}"
    ok "MariaDB stopped, datadir removed"
  else
    ok "MariaDB stopped, datadir kept (MERIDIAN_DEV_DB_KEEP=1): ${MERIDIAN_DEV_DB_DATADIR}"
  fi
}

# Export the MERIDIAN_DB_* env the daemons/tests read, pointed at OUR instance
# over TCP (127.0.0.1:PORT). Callers append the DB name they want as $1.
db_export_env() {
  local dbname="${1:-meridian_auth}"
  export MERIDIAN_DB_HOST=127.0.0.1
  export MERIDIAN_DB_PORT="${MERIDIAN_DEV_DB_PORT}"
  export MERIDIAN_DB_USER=root
  export MERIDIAN_DB_NAME="${dbname}"
  unset MERIDIAN_DB_PASS MERIDIAN_DB_SOCKET
}
