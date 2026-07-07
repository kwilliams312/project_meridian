#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/test.sh — run the Meridian test suites natively on macOS (#223).
#
# Default (no flag): the DB-FREE suite —
#   * ctest --test-dir build/server   (DB integration tests SKIP without MERIDIAN_DB_*)
#   * ctest --test-dir build/mcc       (mcc unit tests)
#   * uv run pytest -q                 (Python validator/doc-sync tests)
#   * uv run tools/validate_content.py --assets error
#   * uv run tools/check_traceability.py
#
# With --db: additionally spins up a THROWAWAY local MariaDB (port 3307, socket
# /tmp/mmdb.sock, temp datadir), loads all schemas, re-runs ctest with
# MERIDIAN_DB_* set so the DB-backed tests (authd-login, worldd-session,
# worldd-relay, meridian-db, account) run for real, then tears the DB down.
#
# Requires a prior build (scripts/dev/build.sh). Fails if build/ is absent.
#
# Usage:
#   scripts/dev/test.sh            # DB-free suite
#   scripts/dev/test.sh --db       # + throwaway-MariaDB-backed tests
#   scripts/dev/test.sh --db --keep-db   # keep the datadir for inspection
#   scripts/dev/test.sh --help
set -euo pipefail

# shellcheck source=scripts/dev/_common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

WITH_DB=0
while [ $# -gt 0 ]; do
  case "$1" in
    --db)      WITH_DB=1 ;;
    --keep-db) export MERIDIAN_DEV_DB_KEEP=1 ;;
    -h|--help)
      grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'
      exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
  shift
done

require_macos
setup_cmake_env

[ -d "${BUILD_ROOT}/server" ] || die "no server build at ${BUILD_ROOT}/server. Run: scripts/dev/build.sh"
[ -d "${BUILD_ROOT}/mcc" ]    || die "no mcc build at ${BUILD_ROOT}/mcc. Run: scripts/dev/build.sh"

fail=0

# --- DB-free ctest (server): DB integration tests self-SKIP without env. ------
log "ctest — server (DB-free; DB tests SKIP without MERIDIAN_DB_*)"
# Make sure no stray MERIDIAN_DB_* leaks in and un-skips DB tests unexpectedly.
env -u MERIDIAN_DB_HOST -u MERIDIAN_DB_PORT -u MERIDIAN_DB_USER \
    -u MERIDIAN_DB_PASS -u MERIDIAN_DB_NAME -u MERIDIAN_DB_SOCKET \
  ctest --test-dir "${BUILD_ROOT}/server" --output-on-failure \
  || { warn "server ctest (DB-free) had failures"; fail=1; }

# --- ctest (mcc). ------------------------------------------------------------
log "ctest — mcc"
ctest --test-dir "${BUILD_ROOT}/mcc" --output-on-failure \
  || { warn "mcc ctest had failures"; fail=1; }

# --- Python suite. -----------------------------------------------------------
log "pytest — Python validator/doc-sync suite"
( cd "${REPO_ROOT}" && uv run pytest -q ) \
  || { warn "pytest had failures"; fail=1; }

log "content validator (--assets error)"
( cd "${REPO_ROOT}" && uv run tools/validate_content.py --assets error ) \
  || { warn "validate_content.py reported errors"; fail=1; }

log "traceability check"
( cd "${REPO_ROOT}" && uv run tools/check_traceability.py ) \
  || { warn "check_traceability.py reported errors"; fail=1; }

# --- DB-backed ctest (optional). ---------------------------------------------
if [ "$WITH_DB" -eq 1 ]; then
  # shellcheck source=scripts/dev/_db.sh
  source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_db.sh"
  # Always tear the DB down, even on failure/Ctrl-C.
  trap 'db_stop' EXIT INT TERM

  echo
  log "=== DB-backed tests (throwaway MariaDB) ==="
  db_start
  db_load_schemas

  # The DB tests read MERIDIAN_DB_* and connect over TCP to our instance. authd/
  # worldd/account/db tests use the auth schema; point NAME at meridian_auth.
  db_export_env meridian_auth

  log "ctest — server (DB-backed: authd-login, worldd-session, worldd-relay, meridian-db, account)"
  ctest --test-dir "${BUILD_ROOT}/server" --output-on-failure \
    || { warn "server ctest (DB-backed) had failures"; fail=1; }

  # db_stop runs via trap on EXIT.
fi

echo
if [ "$fail" -eq 0 ]; then
  ok "All suites passed."
else
  die "One or more suites failed (see output above)."
fi
