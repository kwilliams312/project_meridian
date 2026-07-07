#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/demo-networked.sh — WATCH A BOT MOVE IN THE GUI CLIENT (issue #301).
#
# The on-screen counterpart of client/test/run_client_sees_bot_it.sh (which proves
# the same data flow headlessly). One command brings up a local realm, launches ONE
# bot walking a square at the spawn, then opens the GUI client so YOU log in, pick a
# character, Enter World, and watch the bot's capsule move on screen — the networked
# world scene (scenes/world/world.tscn) rendering a remote entity live.
#
# It composes the existing dev scripts:
#   scripts/dev/build.sh      → server (authd/worldd/meridian-account) + editor GDExtension
#   scripts/dev/run-local.sh  → throwaway MariaDB + authd(:7100) + worldd(:7200) + account
#   (+ this script)           → seed a realm row, create a bot account, launch the bot,
#                               then run scripts/dev/run-client.sh (windowed, Metal).
#
# On exit (close the client, or Ctrl-C) the bot + realm are torn down.
#
# Requires macOS + MariaDB + a pinned Godot 4.7 (see scripts/dev/run-client.sh). The
# bot needs the client bot target, built here with -DMERIDIAN_BOT=ON.
#
# Usage:  scripts/dev/demo-networked.sh
#         MERIDIAN_DEMO_NO_BUILD=1 scripts/dev/demo-networked.sh   # skip the build step

set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/dev/_common.sh
source "${SELF_DIR}/_common.sh"
# shellcheck source=scripts/dev/_db.sh
source "${SELF_DIR}/_db.sh"

require_macos
setup_cmake_env

AUTHD_PORT=7100
WORLDD_PORT=7200
CLIENT_BUILD=1000
TEST_USER="devtester"           # the human logs in as this (from run-local.sh)
TEST_PASS="devpassword"
BOT_USER="demobot"
BOT_PASS="the bot walks all day long"
REALM_NAME="Meridian Dev Realm"
BOT_PID=""
CLI_BUILD_DIR="${REPO_ROOT}/build-client-bot"

cleanup() {
  set +e
  [ -n "${BOT_PID}" ] && { log "stopping the demo bot"; kill "${BOT_PID}" 2>/dev/null; }
  log "tearing down the local realm"
  "${SELF_DIR}/run-local.sh" --stop >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

# --- 1. Build server + bot + editor GDExtension. -----------------------------
# `build.sh --client` builds the server/mcc AND the **editor** GDExtension framework
# (libmeridian.macos.editor.framework) — the exact variant run-client.sh dlopen()s
# below. Rebuilding it here keeps the demo self-contained + reproducible: a stale
# framework (the class of failure that once needed a hand rebuild, #300) can never
# silently break the demo again. MERIDIAN_DEMO_NO_BUILD=1 skips the whole build.
if [ "${MERIDIAN_DEMO_NO_BUILD:-0}" != "1" ]; then
  log "building server + editor GDExtension (scripts/dev/build.sh --client)"
  "${SELF_DIR}/build.sh" --client
fi

BOT_BIN=""
if [ "${MERIDIAN_DEMO_NO_BUILD:-0}" != "1" ] || [ ! -x "${CLI_BUILD_DIR}/bot/meridian-bot" ]; then
  log "building the headless bot (meridian-bot, -DMERIDIAN_BOT=ON)"
  cmake -S "${REPO_ROOT}/client" -B "${CLI_BUILD_DIR}" -DMERIDIAN_BOT=ON >/dev/null
  cmake --build "${CLI_BUILD_DIR}" --target meridian-bot -j >/dev/null
fi
BOT_BIN="$(find "${CLI_BUILD_DIR}" -name meridian-bot -type f -perm -u+x 2>/dev/null | head -1)"
[ -x "${BOT_BIN}" ] || die "meridian-bot not built at ${CLI_BUILD_DIR}"
ok "bot built: ${BOT_BIN}"

# --- 2. Bring up the local realm (DB + authd + worldd + devtester account). ---
log "bringing up the local realm (scripts/dev/run-local.sh)"
"${SELF_DIR}/run-local.sh" >/dev/null
ok "realm up — authd 127.0.0.1:${AUTHD_PORT}, worldd 127.0.0.1:${WORLDD_PORT}"

# --- 3. Seed a realm row pointing at worldd + a bot account. -----------------
# The GUI login needs a realm in the auth DB whose address:port is worldd, and whose
# build range admits the client build. Insert one idempotently.
log "seeding realm '${REALM_NAME}' (127.0.0.1:${WORLDD_PORT})"
_dbc meridian_auth -e \
  "INSERT INTO realm (name, address, port, build_min, build_max, population, flags) \
   SELECT '${REALM_NAME}', '127.0.0.1', ${WORLDD_PORT}, 0, 100000, 0, 0 \
   WHERE NOT EXISTS (SELECT 1 FROM realm WHERE address='127.0.0.1' AND port=${WORLDD_PORT});"
REALM_ID="$(_dbc -N meridian_auth -e \
  "SELECT id FROM realm WHERE address='127.0.0.1' AND port=${WORLDD_PORT} ORDER BY id LIMIT 1;")"
[ -n "${REALM_ID}" ] || die "realm not seeded"
ok "realm id ${REALM_ID}"

ACCOUNT_BIN="${BUILD_ROOT}/server/tools/meridian-account/meridian-account"
[ -x "${ACCOUNT_BIN}" ] || die "meridian-account not built. Run scripts/dev/build.sh"
log "creating bot account '${BOT_USER}'"
env MERIDIAN_DB_HOST=127.0.0.1 MERIDIAN_DB_PORT="${MERIDIAN_DEV_DB_PORT}" \
    MERIDIAN_DB_USER=root MERIDIAN_DB_NAME=meridian_auth \
  bash -c "printf '%s\n' \"\$1\" | \"\$0\" create --username \"\$2\"" \
  "${ACCOUNT_BIN}" "${BOT_PASS}" "${BOT_USER}" >/dev/null 2>&1 || true
ok "bot account ready"

# --- 4. Launch the bot walking a square at the spawn (long window). ----------
BOT_LOG="${REPO_ROOT}/.dev-run/demo-bot.log"
log "launching the demo bot (walks a square at the spawn for ~10 min)"
"${BOT_BIN}" \
  --authd "127.0.0.1:${AUTHD_PORT}" \
  --worldd "127.0.0.1:${WORLDD_PORT}" \
  --user "${BOT_USER}" --password "${BOT_PASS}" \
  --realm "${REALM_ID}" --build "${CLIENT_BUILD}" \
  --path square --duration 600 >"${BOT_LOG}" 2>&1 &
BOT_PID=$!
sleep 2
kill -0 "${BOT_PID}" 2>/dev/null || { err_log="$(tail -5 "${BOT_LOG}" 2>/dev/null)"; die "bot exited early: ${err_log}"; }
ok "bot walking (pid ${BOT_PID}, log ${BOT_LOG})"

# --- 5. Open the GUI client for the human. -----------------------------------
cat <<EOF

$(ok "READY — watch the bot move in the GUI")
  In the client window that opens:
    1. Host      : 127.0.0.1
       Port      : ${AUTHD_PORT}
       Account   : ${TEST_USER}
       Password  : ${TEST_PASS}
    2. Log in → Character Select → Create a character → Enter World
    3. You spawn at the bootstrap point; a SECOND capsule (the bot, labelled
       'guid …') walks a square nearby — that is the networked world scene
       rendering a remote entity live over worldd's #87 AoI relay.
       (Use WASD to move your own capsule; hold RMB to steer the camera.)

  Close the client window (or press Ctrl-C here) to tear everything down.

EOF

log "launching the GUI client into the LOGIN flow (scripts/dev/run-client.sh, windowed Metal)"
# Boot the login screen (the project main_scene), NOT the default camera_demo — the
# login → char-select → Enter World chain is what hands world.tscn its real session
# (grant + worldd addr + session_key) so it connects to worldd and renders the bot.
"${SELF_DIR}/run-client.sh" --scene res://scenes/login/login_screen.tscn || true

ok "client closed — cleaning up"
