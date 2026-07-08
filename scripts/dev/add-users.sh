#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/add-users.sh — bulk-provision test accounts for concurrency testing (#330).
#
# Creates N zero-padded accounts (PREFIX0001 .. PREFIXnnnn) in the local/dev auth
# DB so multi-client concurrency tests have a pool of logins to drive. It wraps the
# existing meridian-account CLI (server/tools/meridian-account, #80) exactly the way
# run-local.sh does: pipe the password on stdin, create against MERIDIAN_DB_*.
#
# It expects a local realm to already be up (scripts/dev/run-local.sh) — it talks to
# that running throwaway MariaDB, it does NOT start/stop or wipe a DB of its own.
#
# Idempotent: re-running never errors on accounts that already exist. The account CLI
# exits 3 on a duplicate username; we treat that as "skipped" and keep going, then
# print a requested / created / skipped summary. So the natural way to grow the pool
# is to re-run with a larger --count.
#
# Usage:
#   scripts/dev/add-users.sh [--count N] [--prefix STR] [--password PW] [--width W]
#                            [--dry-run]
#
#   --count N      how many accounts to provision            (default 10)
#   --prefix STR   username prefix                           (default loadtest)
#   --password PW  shared password for every account         (default loadtestpass)
#   --width W      zero-pad width for the numeric suffix      (default 4)
#   --dry-run      print what would be created, touch nothing (no DB, no binary needed)
#
# Examples:
#   scripts/dev/run-local.sh                 # bring the realm up first
#   scripts/dev/add-users.sh --count 200     # loadtest0001 .. loadtest0200
#   scripts/dev/add-users.sh --count 200     # again → all 200 skipped (idempotent)
#   scripts/dev/add-users.sh --count 500     # 200 skipped, 300 created
#
# All accounts share PW, so a load driver can log in as PREFIXnnnn / PW.
set -euo pipefail

# shellcheck source=scripts/dev/_common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
# shellcheck source=scripts/dev/_db.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_db.sh"

# --- Defaults + arg parsing. -------------------------------------------------
COUNT=10
PREFIX="loadtest"
PASSWORD="loadtestpass"
WIDTH=4
DRY_RUN=0

# Consume "--flag value" pairs; validate as we go. Mirrors run-local.sh's style.
need_value() { [ $# -ge 2 ] || die "$1 requires a value"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --count)    need_value "$@"; COUNT="$2";    shift 2 ;;
    --prefix)   need_value "$@"; PREFIX="$2";   shift 2 ;;
    --password) need_value "$@"; PASSWORD="$2"; shift 2 ;;
    --width)    need_value "$@"; WIDTH="$2";    shift 2 ;;
    --dry-run)  DRY_RUN=1; shift ;;
    -h|--help)
      grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'
      exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
done

# --- Validate. ---------------------------------------------------------------
case "$COUNT" in ''|*[!0-9]*) die "--count must be a positive integer (got '${COUNT}')" ;; esac
[ "$COUNT" -ge 1 ] || die "--count must be >= 1 (got '${COUNT}')"
case "$WIDTH" in ''|*[!0-9]*) die "--width must be a positive integer (got '${WIDTH}')" ;; esac
[ "$WIDTH" -ge 1 ] || die "--width must be >= 1 (got '${WIDTH}')"
[ -n "$PREFIX" ]   || die "--prefix must not be empty"

require_macos

# --- Dry run: show exactly what we'd do, then exit without touching anything. -
if [ "$DRY_RUN" -eq 1 ]; then
  log "[dry-run] would provision ${COUNT} account(s) with prefix '${PREFIX}' (width ${WIDTH}) in meridian_auth"
  for i in $(seq 1 "$COUNT"); do
    user="$(printf '%s%0*d' "$PREFIX" "$WIDTH" "$i")"
    log "[dry-run] meridian-account create --username ${user}  (password on stdin)"
  done
  ok "[dry-run] ${COUNT} requested, 0 created, 0 skipped — nothing was written"
  exit 0
fi

setup_cmake_env

ACCOUNT="${BUILD_ROOT}/server/tools/meridian-account/meridian-account"
[ -x "$ACCOUNT" ] || die "meridian-account not built. Run: scripts/dev/build.sh"

# --- Preflight: the local realm DB must be reachable. ------------------------
# _dbc talks over the run-local.sh throwaway socket; a live SELECT proves the
# realm (and its meridian_auth schema) is up. This is the same instance the
# account CLI reaches over TCP via the MERIDIAN_DB_* env we export below.
if ! _dbc meridian_auth -e 'SELECT 1;' >/dev/null 2>&1; then
  die "no local realm DB reachable on ${MERIDIAN_DEV_DB_SOCKET}. Start it first: scripts/dev/run-local.sh"
fi

# Point the account CLI at OUR instance (127.0.0.1:PORT, root, meridian_auth).
db_export_env meridian_auth

# --- Provision. --------------------------------------------------------------
log "Provisioning ${COUNT} account(s) '${PREFIX}$(printf '%0*d' "$WIDTH" 1)'..'${PREFIX}$(printf '%0*d' "$WIDTH" "$COUNT")' in meridian_auth"

created=0
skipped=0
for i in $(seq 1 "$COUNT"); do
  user="$(printf '%s%0*d' "$PREFIX" "$WIDTH" "$i")"
  if printf '%s\n' "${PASSWORD}" | "$ACCOUNT" create --username "${user}" >/dev/null 2>&1; then
    created=$((created + 1))
  else
    rc=$?
    # rc 3 = duplicate username (already exists) — idempotent, count as skipped.
    if [ "$rc" -eq 3 ]; then
      skipped=$((skipped + 1))
    else
      die "meridian-account create failed for '${user}' (rc=${rc})"
    fi
  fi
done

# --- Summary. ----------------------------------------------------------------
echo
ok "Provisioning complete."
echo "  requested : ${COUNT}"
echo "  created   : ${created}"
echo "  skipped   : ${skipped}  (already existed)"
echo "  login     : ${PREFIX}$(printf '%0*d' "$WIDTH" 1)..${PREFIX}$(printf '%0*d' "$WIDTH" "$COUNT") / ${PASSWORD}"
