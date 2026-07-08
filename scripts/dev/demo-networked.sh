#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/demo-networked.sh — WATCH N BOTS MOVE IN THE GUI CLIENT (issue #301).
#
# The on-screen counterpart of client/test/run_client_sees_bot_it.sh (which proves
# the same data flow headlessly). One command brings up a local realm, provisions N
# bot accounts, seeds each a character with a ROTATING class, launches N bots walking
# a square at the spawn, then opens the GUI client so YOU log in, pick a character,
# Enter World, and watch every bot's capsule move on screen — each one CLASS-COLORED
# (#328) so multi-client concurrency reads at a glance.
#
# It composes the existing dev scripts:
#   scripts/dev/build.sh      → server (authd/worldd/meridian-account) + editor GDExtension
#   scripts/dev/run-local.sh  → throwaway MariaDB + authd(:7100) + worldd(:7200) + account
#   scripts/dev/add-users.sh  → bulk-provision the N bot accounts (#330)
#   (+ this script)           → seed a realm row, seed N class-rotated characters,
#                               launch N bots, then run scripts/dev/run-client.sh.
#
# The N seeded characters rotate through the M0-frozen class roster
# (server/characters/src/roster.h): 1=Vanguard(red) 2=Runcaller(blue) 3=Warden(green)
# 4=Mender(gold). worldd's load_placeholder_character() reads each bot account's
# character row and relays its class on the wire; the client colors the capsule via
# client/project/scenes/world/player_class_colors.gd (#328). With --bots 4 all four
# colors appear.
#
# On exit (close the client, or Ctrl-C) the bots + realm are torn down.
#
# Requires macOS + MariaDB + a pinned Godot 4.7 (see scripts/dev/run-client.sh). The
# bot needs the client bot target, built here with -DMERIDIAN_BOT=ON.
#
# Usage:
#   scripts/dev/demo-networked.sh                      # 4 bots (all class colors) + GUI
#   scripts/dev/demo-networked.sh --bots 1             # single bot (original behaviour)
#   scripts/dev/demo-networked.sh --bots 8             # 8 bots (classes cycle 1..4,1..4)
#   scripts/dev/demo-networked.sh --no-client          # headless: prove classes, no GUI
#   MERIDIAN_DEMO_NO_BUILD=1 scripts/dev/demo-networked.sh   # skip the build step
#   MERIDIAN_DEMO_BOTS=4 MERIDIAN_DEMO_NO_CLIENT=1 scripts/dev/demo-networked.sh
#
#   --bots N       how many bots/accounts/characters to run   (default 4; env
#                  MERIDIAN_DEMO_BOTS; flag wins over env)
#   --no-client    run everything EXCEPT the GUI client, wait for the bots to enter
#                  world, then grep the worldd log for the per-session class_id and
#                  print the DISTINCT classes seen — proves the colors will differ
#                  without needing a display (env MERIDIAN_DEMO_NO_CLIENT=1).

set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/dev/_common.sh
source "${SELF_DIR}/_common.sh"
# shellcheck source=scripts/dev/_db.sh
source "${SELF_DIR}/_db.sh"

# --- Defaults + arg parsing. -------------------------------------------------
# Flags override env; env overrides the built-in default. Mirrors the existing
# MERIDIAN_DEMO_NO_BUILD env-override style.
BOTS="${MERIDIAN_DEMO_BOTS:-4}"
NO_CLIENT="${MERIDIAN_DEMO_NO_CLIENT:-0}"

need_value() { [ $# -ge 2 ] || die "$1 requires a value"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --bots)      need_value "$@"; BOTS="$2"; shift 2 ;;
    --no-client) NO_CLIENT=1; shift ;;
    -h|--help)
      grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'
      exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
done

case "$BOTS" in ''|*[!0-9]*) die "--bots must be a positive integer (got '${BOTS}')" ;; esac
[ "$BOTS" -ge 1 ] || die "--bots must be >= 1 (got '${BOTS}')"

require_macos
setup_cmake_env

AUTHD_PORT=7100
WORLDD_PORT=7200
CLIENT_BUILD=1000
TEST_USER="devtester"           # the human logs in as this (from run-local.sh)
TEST_PASS="devpassword"
BOT_PREFIX="demobot"            # account/username prefix (add-users.sh --prefix)
BOT_WIDTH=4                     # zero-pad width for the numeric suffix (add-users.sh default)
BOT_PASS="the bot walks all day long"
REALM_NAME="Meridian Dev Realm"
# map_id is NOT NULL with no default in the characters schema; the M0 placeholder
# spawn is chosen by worldd (it does not read character.map_id), so any valid
# non-zero id satisfies the column. Use 1 (the D-19 flat bootstrap map).
SEED_MAP_ID=1
BOT_PIDS=()
CLI_BUILD_DIR="${REPO_ROOT}/build-client-bot"

# Zero-padded username for bot i (demobot0001, demobot0002, ...). Matches how
# add-users.sh names the accounts it provisions.
bot_user() { printf '%s%0*d' "${BOT_PREFIX}" "${BOT_WIDTH}" "$1"; }
# Rotating class id for bot i: 1..4, 1..4, ... (roster.h Vanguard/Runcaller/Warden/Mender).
bot_class() { echo $(( ( ($1 - 1) % 4 ) + 1 )); }

cleanup() {
  set +e
  # Kill every bot we launched (the array-expansion guard keeps `set -u` happy
  # when no bot was launched yet).
  for p in ${BOT_PIDS[@]+"${BOT_PIDS[@]}"}; do
    [ -n "$p" ] && kill "$p" 2>/dev/null
  done
  [ "${#BOT_PIDS[@]}" -gt 0 ] && log "stopping ${#BOT_PIDS[@]} demo bot(s)"
  log "tearing down the local realm"
  "${SELF_DIR}/run-local.sh" --stop >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

# --- 1. Build server + bot (+ editor GDExtension for the GUI). ---------------
# For the GUI demo, `build.sh --client` builds the server/mcc AND the **editor**
# GDExtension framework (libmeridian.macos.editor.framework) — the exact variant
# run-client.sh dlopen()s below. Rebuilding it here keeps the demo self-contained +
# reproducible: a stale framework (the class of failure that once needed a hand
# rebuild, #300) can never silently break the demo again.
#
# In --no-client (headless verify) mode we never open the GUI, so we DON'T build the
# editor GDExtension — a plain `build.sh` (server + mcc) is enough, and it needs no
# godot-cpp submodule / Godot toolchain. That keeps the verify path runnable on a
# display-less / CI box. MERIDIAN_DEMO_NO_BUILD=1 skips the whole build.
if [ "${MERIDIAN_DEMO_NO_BUILD:-0}" != "1" ]; then
  if [ "${NO_CLIENT}" = "1" ]; then
    log "building server (scripts/dev/build.sh) — headless verify, no GUI/GDExtension"
    "${SELF_DIR}/build.sh"
  else
    log "building server + editor GDExtension (scripts/dev/build.sh --client)"
    "${SELF_DIR}/build.sh" --client
  fi
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

# --- 3. Seed a realm row pointing at worldd. ---------------------------------
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

# --- 4. Provision N bot accounts (reuse add-users.sh, #330). -----------------
# add-users.sh wraps meridian-account exactly the way run-local.sh does and is
# idempotent (existing accounts are skipped). Accounts land as demobot0001..N in
# meridian_auth, all sharing BOT_PASS.
log "provisioning ${BOTS} bot account(s) '$(bot_user 1)'..'$(bot_user "${BOTS}")' (scripts/dev/add-users.sh)"
"${SELF_DIR}/add-users.sh" --count "${BOTS}" --prefix "${BOT_PREFIX}" \
  --password "${BOT_PASS}" --width "${BOT_WIDTH}" >/dev/null
ok "${BOTS} bot account(s) ready"

# --- 5. Seed one class-rotated character per bot account. --------------------
# worldd's load_placeholder_character(char_db, account_id) reads the FIRST character
# row for the account and relays its `class` on the wire (#328), so a per-account
# character with a rotating class is what colors each capsule differently. This is
# intentional test setup via direct SQL: it bypasses the #329 create-path 1-char cap
# (still exactly one character per account). Guarded so re-running never errors.
log "seeding ${BOTS} class-rotated character(s) in meridian_characters"
for i in $(seq 1 "${BOTS}"); do
  user="$(bot_user "$i")"
  cls="$(bot_class "$i")"
  acct_id="$(_dbc -N meridian_auth -e \
    "SELECT id FROM account WHERE username='${user}' ORDER BY id LIMIT 1;")"
  [ -n "${acct_id}" ] || die "no account_id for '${user}' (add-users.sh did not create it?)"
  cname="DemoBot$(printf '%0*d' "${BOT_WIDTH}" "$i")"
  # INSERT ... SELECT <literals> WHERE NOT EXISTS(...) — same idempotent shape as the
  # realm seed above. name is UNIQUE (case-insensitive); guard on account_id so a
  # re-run is a no-op. pos_x/y/z are NOT NULL (worldd overrides the spawn anyway).
  _dbc meridian_characters -e \
    "INSERT INTO \`character\` (account_id, name, race, class, level, map_id, pos_x, pos_y, pos_z) \
     SELECT ${acct_id}, '${cname}', 1, ${cls}, 1, ${SEED_MAP_ID}, 64, 64, 0 \
     WHERE NOT EXISTS (SELECT 1 FROM \`character\` WHERE account_id = ${acct_id});"
  ok "  ${user} -> character '${cname}' class ${cls}"
done

# --- 6. Launch N bots walking a square at the spawn (long window). -----------
# Reuse the single-bot launch in a loop; stagger launches slightly and verify each
# bot is still alive before moving on. Every bot is a distinct account, so the
# single-active-session-per-account rule (#326) is satisfied.
log "launching ${BOTS} demo bot(s) (each walks a square at the spawn for ~10 min)"
for i in $(seq 1 "${BOTS}"); do
  user="$(bot_user "$i")"
  cls="$(bot_class "$i")"
  bot_log="${REPO_ROOT}/.dev-run/demo-bot-${i}.log"
  "${BOT_BIN}" \
    --authd "127.0.0.1:${AUTHD_PORT}" \
    --worldd "127.0.0.1:${WORLDD_PORT}" \
    --user "${user}" --password "${BOT_PASS}" \
    --realm "${REALM_ID}" --build "${CLIENT_BUILD}" \
    --path square --duration 600 >"${bot_log}" 2>&1 &
  pid=$!
  BOT_PIDS+=("$pid")
  sleep 2
  kill -0 "${pid}" 2>/dev/null \
    || { err_log="$(tail -5 "${bot_log}" 2>/dev/null)"; die "bot ${i} (${user}) exited early: ${err_log}"; }
  ok "  bot ${i}/${BOTS} (${user}, class ${cls}) walking — pid ${pid}, log ${bot_log}"
done
ok "${BOTS} bot(s) walking"

# --- 7a. Headless verify path (--no-client): prove the classes, no GUI. ------
# Everything above already ran (realm → accounts → characters → bots). Now wait for
# the bots to complete their WORLD_HELLO, grep worldd's log for the per-session
# class_id field (world_dispatch.cpp ~509 logs "WORLD_HELLO accepted -> HandshakeOk
# ... class_id=N"), and print the DISTINCT classes seen. This proves the bots enter
# with varied classes — i.e. the capsules WILL be colored differently — without a
# display.
if [ "${NO_CLIENT}" = "1" ]; then
  WORLDD_LOG="${REPO_ROOT}/.dev-run/worldd.log"
  log "headless verify: waiting for ${BOTS} bot(s) to enter world (WORLD_HELLO)..."
  # Poll the log for BOTS accepted handshakes (up to ~15s) rather than sleep-guess.
  accepted=0
  # shellcheck disable=SC2034  # w is the retry-budget loop counter
  for w in $(seq 1 60); do
    # grep -c prints exactly one count line (0 on no match); `|| true` swallows
    # its exit-1-on-no-match so `set -e` and the arithmetic stay happy.
    accepted="$(grep -c 'WORLD_HELLO accepted' "${WORLDD_LOG}" 2>/dev/null || true)"
    [ -n "${accepted}" ] || accepted=0
    [ "${accepted}" -ge "${BOTS}" ] && break
    sleep 0.25
  done

  echo
  log "worldd WORLD_HELLO accept lines (class_id per session):"
  grep 'WORLD_HELLO accepted' "${WORLDD_LOG}" 2>/dev/null \
    | grep -oE 'class_id=[0-9]+' | sort | uniq -c || true

  echo
  distinct="$(grep 'WORLD_HELLO accepted' "${WORLDD_LOG}" 2>/dev/null \
                | grep -oE 'class_id=[0-9]+' | sort -u | paste -sd',' - || true)"
  n_distinct="$(grep 'WORLD_HELLO accepted' "${WORLDD_LOG}" 2>/dev/null \
                  | grep -oE 'class_id=[0-9]+' | sort -u | grep -c . || true)"
  [ -n "${n_distinct}" ] || n_distinct=0
  ok "accepted handshakes: ${accepted}/${BOTS}"
  ok "distinct class_ids entered: ${n_distinct}  [${distinct}]"

  # For the default 4-bot demo all four roster classes should appear.
  if [ "${BOTS}" -ge 4 ] && [ "${n_distinct}" -lt 4 ]; then
    warn "expected 4 distinct class_ids for --bots ${BOTS}, saw ${n_distinct} — see ${WORLDD_LOG}"
  fi
  echo
  ok "headless verify complete — tearing down (cleanup trap)."
  exit 0
fi

# --- 7b. Open the GUI client for the human. ----------------------------------
CLASS_LEGEND="red=Vanguard, blue=Runcaller, green=Warden, gold=Mender"
cat <<EOF

$(ok "READY — watch ${BOTS} class-colored bot(s) move in the GUI")
  In the client window that opens:
    1. Host      : 127.0.0.1
       Port      : ${AUTHD_PORT}
       Account   : ${TEST_USER}
       Password  : ${TEST_PASS}
    2. Log in → Character Select → Create a character. PICK A CLASS at create —
       your OWN capsule is colored by that class too (${CLASS_LEGEND}).
    3. Enter World. You spawn at the bootstrap point; ${BOTS} bot capsule(s)
       (labelled 'guid …') walk a square nearby, each CLASS-COLORED so you can
       tell them apart — that is the networked world scene rendering remote
       entities live over worldd's #87 AoI relay, colored via #328.
       (Use WASD to move your own capsule; hold RMB to steer the camera.)

  Close the client window (or press Ctrl-C here) to tear everything down.

EOF

log "launching the GUI client into the LOGIN flow (scripts/dev/run-client.sh, windowed Metal)"
# Boot the login screen (the project main_scene), NOT the default camera_demo — the
# login → char-select → Enter World chain is what hands world.tscn its real session
# (grant + worldd addr + session_key) so it connects to worldd and renders the bots.
"${SELF_DIR}/run-client.sh" --scene res://scenes/login/login_screen.tscn || true

ok "client closed — cleaning up"
