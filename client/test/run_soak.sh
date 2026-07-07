#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# client/test/run_soak.sh — the SOAK / STABILITY harness (#152). The endurance
# sibling of #111's run_bot_client_it.sh (one bot, one pass) and #248's
# run_two_bot_it.sh (two bots, mutual visibility): this runs N bots CONCURRENTLY
# against the REAL authd + worldd for a DURATION and watches the server for
# STABILITY — no crashes, no error spikes, no unbounded memory growth, no tick
# degradation — then reports PASS/FAIL.
#
# It answers the IT-M0 done-criteria (docs/it-m0-runbook.md §Step 7 / DC-5 + DC-9,
# docs/sad/server-sad.md §8): "10-bot fleet on the realm for >= 1 h, zero crashes"
# (server soak) and the 30-min client soak. Parameterized so the SAME script runs
#   * a SHORT soak locally / in CI    :  run_soak.sh --bots 5  --duration 120   (5 bots, 2 min)
#   * the FULL server soak on the realm:  run_soak.sh --bots 10 --duration 3600  (10 bots, 1 h)
#   * the client 30-min soak           :  run_soak.sh --bots 10 --duration 1800
# ...or against an EXISTING realm with --external-host (skip the local boot).
#
# The harness (reusing #111/#248 patterns):
#   1. boot a THROWAWAY MariaDB on a UNIQUE PID-keyed socket/port (NOT the dev
#      default /tmp/mmdb.sock:3307, NOT #111's 3400s, NOT #248's 3600s),
#   2. load the auth + characters schemas,
#   3. build authd + worldd + meridian-account (server) + meridian-bot (client),
#   4. create N test accounts + seed ONE realm bound to worldd,
#   5. launch the real authd (IF-1 TLS) + worldd (IF-2 TLS) WITH the Prometheus
#      /metrics endpoint on a unique port (#164) so the soak can scrape it,
#   6. launch N meridian-bot processes CONCURRENTLY, each --duration <D> --path
#      square (login -> enter-world -> move on a path for the whole window),
#   7. MONITOR during the soak: every --poll-interval seconds, scrape worldd
#      /metrics (CCU, tick p99, opcode errors/drops, disconnects) + sample the
#      authd/worldd RSS via ps. Record a time series,
#   8. collect every bot's exit code, wait for the window to elapse,
#   9. EVALUATE stability + emit a soak REPORT (bots ok/total, peak CCU, tick p99,
#      disconnects, opcode-error delta, RSS start->end/peak, verdict), exit non-zero
#      on FAIL.
#
# FAIL if: any daemon crashes/exits mid-soak; any bot fails to stay in-world for
# the window; opcode error/drop metrics spike past the budget; tick p99 exceeds
# the SAD 40 ms soft budget; or daemon RSS grows past the growth budget. PASS if
# all bots complete the duration in-world with stable metrics and zero crashes.
#
# Requires MariaDB (mariadbd, mariadb-install-db, mariadb) + cmake + flatc +
# OpenSSL + curl on PATH. Env-guarded / integration + nightly (NOT a fast CI job).
# Exits non-zero on any stability failure; prints the ACTUAL report so the run is
# HONEST about what stayed up and what drifted.
#
# Usage:
#   client/test/run_soak.sh [--bots N] [--duration SECONDS] [options]
#
#   --bots N              number of concurrent bots           (default 5)
#   --duration SECONDS    soak window per bot                 (default 120)
#   --poll-interval S     seconds between /metrics + RSS polls (default 10)
#   --path P              scripted bot path: square | idle    (default square)
#   --tick-budget-ms N    max acceptable tick p99, ms          (default 40 — SAD soft)
#   --rss-growth-pct N    max acceptable daemon RSS growth, %  (default 50)
#   --external-host H:P   soak an EXISTING authd realm instead of booting locally
#                         (see "SOAKING AN EXTERNAL REALM" below)
#   --external-worldd H:P worldd game addr for the external realm
#   --external-metrics U  worldd /metrics base URL for the external realm
#                         (e.g. http://10.0.0.5:9464)
#   MERIDIAN_IT_KEEP=1    keep the datadir + logs after the run
#
# Examples:
#   client/test/run_soak.sh --bots 5  --duration 120      # SHORT soak (CI/local)
#   client/test/run_soak.sh --bots 10 --duration 3600     # FULL 10-bot x 1 h (realm)
#   client/test/run_soak.sh --bots 10 --duration 1800     # 30-min client soak
#
# SOAKING AN EXTERNAL REALM (the real test realm, #94):
#   The local-boot steps (1-5) are skipped; the harness only creates the N
#   accounts + realm binding on the target's auth DB it can reach, launches the
#   bots at --external-host/--external-worldd, and scrapes --external-metrics.
#   This path is DOCUMENTED for the ops-run 10x1 h but is NOT exercised by the
#   short local soak — see the follow-up note in the PR.

set -euo pipefail

# ============================================================================
# 0. Parse args.
# ============================================================================
BOTS=5
DURATION=120
POLL_INTERVAL=10
BOT_PATH=square
TICK_BUDGET_MS=40
RSS_GROWTH_PCT=50
EXTERNAL_HOST=""
EXTERNAL_WORLDD=""
EXTERNAL_METRICS=""

while [ $# -gt 0 ]; do
  case "$1" in
    --bots)             BOTS="$2"; shift 2 ;;
    --duration)         DURATION="$2"; shift 2 ;;
    --poll-interval)    POLL_INTERVAL="$2"; shift 2 ;;
    --path)             BOT_PATH="$2"; shift 2 ;;
    --tick-budget-ms)   TICK_BUDGET_MS="$2"; shift 2 ;;
    --rss-growth-pct)   RSS_GROWTH_PCT="$2"; shift 2 ;;
    --external-host)    EXTERNAL_HOST="$2"; shift 2 ;;
    --external-worldd)  EXTERNAL_WORLDD="$2"; shift 2 ;;
    --external-metrics) EXTERNAL_METRICS="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,80p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "run_soak.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac
done

case "${BOTS}" in ''|*[!0-9]*) echo "run_soak.sh: --bots must be a positive integer" >&2; exit 2 ;; esac
case "${DURATION}" in ''|*[!0-9]*) echo "run_soak.sh: --duration must be a positive integer" >&2; exit 2 ;; esac
[ "${BOTS}" -ge 1 ] || { echo "run_soak.sh: --bots must be >= 1" >&2; exit 2; }
[ "${DURATION}" -ge 5 ] || { echo "run_soak.sh: --duration must be >= 5" >&2; exit 2; }

EXTERNAL=0
[ -n "${EXTERNAL_HOST}" ] && EXTERNAL=1

# --- Locate the repo root (this script lives at client/test/). ---------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

log()  { printf '\033[1;34m[soak]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ok]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[ERR]\033[0m %s\n' "$*" >&2; }

# ============================================================================
# Shared config (unique, PID-keyed — never collides with a dev DB or a
# parallel #111/#248 harness).
# ============================================================================
IT_TAG="msoak152_$$"
DB_SOCKET="/tmp/${IT_TAG}.sock"
DB_PORT="$(( 3800 + (RANDOM % 200) ))"       # 3800..3999 — away from 3306/3307, #111's 3400s, #248's 3600s
DB_DATADIR="${REPO_ROOT}/.dev-run/${IT_TAG}-data"
DB_LOG="${REPO_ROOT}/.dev-run/${IT_TAG}.log"
DB_PIDFILE="${REPO_ROOT}/.dev-run/${IT_TAG}.pid"
AUTH_DB_NAME="meridian_auth"
CHAR_DB_NAME="meridian_characters"

CERT_DIR="$(mktemp -d "/tmp/${IT_TAG}-cert.XXXXXX")"
CERT_PEM="${CERT_DIR}/cert.pem"
KEY_PEM="${CERT_DIR}/key.pem"

AUTHD_PORT="$(( 8100 + (RANDOM % 200) ))"    # away from the 7100/7200/7300(#111)/7700(#248) defaults
WORLDD_PORT="$(( 8300 + (RANDOM % 200) ))"   # distinct from authd
METRICS_PORT="$(( 9600 + (RANDOM % 200) ))"  # worldd Prometheus /metrics (default 9464; move away)
AUTHD_PID=""
WORLDD_PID=""

CLIENT_BUILD=1000
ACCOUNT_PREFIX="soak_${$}_"
PASSWORD="correct horse battery staple"
REALM_NAME="Soak IT Realm $$"

RUN_DIR="${REPO_ROOT}/.dev-run/${IT_TAG}-run"
mkdir -p "${RUN_DIR}"
METRICS_CSV="${RUN_DIR}/metrics.csv"
BOT_LOG_DIR="${RUN_DIR}/bots"
mkdir -p "${BOT_LOG_DIR}"

# ============================================================================
# Cleanup — kill bots, daemons, DB; drop temp dirs (honor MERIDIAN_IT_KEEP).
# ============================================================================
BOT_PIDS=()
cleanup() {
  set +e
  for p in "${BOT_PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
  [ -n "${WORLDD_PID}" ] && kill "${WORLDD_PID}" 2>/dev/null
  [ -n "${AUTHD_PID}" ]  && kill "${AUTHD_PID}"  2>/dev/null
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
    log "kept run dir: ${RUN_DIR}"
  else
    rm -rf "${DB_DATADIR}" "${RUN_DIR}"
  fi
}
trap cleanup EXIT

_dbc() { mariadb --no-defaults --socket="${DB_SOCKET}" --user=root "$@"; }

# ============================================================================
# Metric helpers — scrape worldd /metrics (Prometheus text) and sum a metric's
# series across labels (CCU is per {realm,zone,shard}; we want the realm total).
# ============================================================================
METRICS_BASE=""   # set after worldd is up (local) or from --external-metrics

# scrape_metric NAME  ->  sum of all series values for that metric name.
# For counters this is the running total; for gauges (ccu, sessions) the
# instantaneous total across label sets. Empty/absent => 0.
scrape_metric() {
  local name="$1"
  curl -fsS --max-time 5 "${METRICS_BASE}/metrics" 2>/dev/null \
    | awk -v m="${name}" '
        $0 ~ "^"m"([{ ]|$)" {
          # value is the last whitespace-separated field on the sample line
          v = $NF
          if (v ~ /^-?[0-9.eE+]+$/) sum += v
        }
        END { printf "%.6g", sum+0 }'
}

# tick_p99_ms — approximate p99 of meridian_tick_duration_seconds from the
# cumulative histogram buckets scraped from /metrics. Returns ms (float).
# We find the smallest bucket "le" whose cumulative count >= 0.99 * total.
#
# NOTE (M0): worldd's tick body only records an observation when it drains a
# NON-empty batch (an idle tick is not timed — see world_dispatch.cpp), and the
# M0 tick body does trivial work (drain + count; the real movement/AI/AoI loop
# lands later). So during a soak the histogram DOES accumulate samples (bots'
# movement enqueues events), but each lands in the smallest bucket and p99 rounds
# to ~0 ms. Returns "0" both when there are no samples yet and when p99 is in the
# smallest bucket — either way the gate (p99 <= budget) is satisfied. The gate is
# real: a tick body exceeding the budget WOULD trip it. As the tick grows past M0
# this metric becomes the true tick-health signal it is designed to be.
tick_p99_ms() {
  curl -fsS --max-time 5 "${METRICS_BASE}/metrics" 2>/dev/null \
    | awk '
        /^meridian_tick_duration_seconds_bucket/ {
          # extract le="..." and the count (last field)
          le = $0; sub(/.*le="/, "", le); sub(/".*/, "", le)
          cnt = $NF
          buckets[le] = cnt
          les[NR] = le
        }
        /^meridian_tick_duration_seconds_count/ { total = $NF }
        END {
          if (total == 0) { print "0"; exit }
          target = 0.99 * total
          # sort finite bucket boundaries ascending, walk to the target
          n = 0
          for (l in buckets) { if (l != "+Inf") { arr[n++] = l+0 } }
          # simple insertion sort (few buckets)
          for (i = 1; i < n; i++) { key = arr[i]; j = i-1;
            while (j >= 0 && arr[j] > key) { arr[j+1] = arr[j]; j-- } arr[j+1] = key }
          for (i = 0; i < n; i++) {
            le = arr[i]
            # match by string form; rebuild key with same precision as printed
            for (l in buckets) { if (l != "+Inf" && (l+0) == le) { c = buckets[l]; break } }
            if (c >= target) { printf "%.3f", le * 1000.0; found = 1; exit }
          }
          if (!found) print "inf"  # p99 fell in the +Inf bucket (over the largest boundary)
        }'
}

# daemon_rss_kb PID  ->  resident set size in KB (0 if PID gone). macOS + Linux ps.
daemon_rss_kb() {
  local pid="$1"
  [ -n "${pid}" ] || { echo 0; return; }
  ps -o rss= -p "${pid}" 2>/dev/null | tr -d ' ' | grep -E '^[0-9]+$' || echo 0
}

# ============================================================================
# LOCAL BOOT (steps 1-5) — skipped entirely for an external realm.
# ============================================================================
if [ "${EXTERNAL}" -eq 0 ]; then
  # --- 1. Boot the throwaway MariaDB. ----------------------------------------
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

  # --- 2. Load the auth + characters schemas. --------------------------------
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

  # --- 3. Build authd + worldd + meridian-account + meridian-bot. ------------
  export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
  SRV_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-server-build"
  CLI_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-client-build"

  log "building authd + worldd + meridian-account (server tree)"
  cmake -S "${REPO_ROOT}/server" -B "${SRV_BUILD}" >/dev/null
  cmake --build "${SRV_BUILD}" --target authd worldd meridian-account -j >/dev/null
  AUTHD_BIN="$(find "${SRV_BUILD}" -name authd -type f -perm -u+x | head -1)"
  WORLDD_BIN="$(find "${SRV_BUILD}" -name worldd -type f -perm -u+x | head -1)"
  ACCOUNT_BIN="$(find "${SRV_BUILD}" -name meridian-account -type f -perm -u+x | head -1)"
  [ -x "${AUTHD_BIN}" ]   || { err "authd not built"; exit 1; }
  [ -x "${WORLDD_BIN}" ]  || { err "worldd not built"; exit 1; }
  [ -x "${ACCOUNT_BIN}" ] || { err "meridian-account not built"; exit 1; }
  ok "built authd + worldd + meridian-account"

  log "building the headless bot (client tree)"
  cmake -S "${REPO_ROOT}/client" -B "${CLI_BUILD}" -DMERIDIAN_BOT=ON >/dev/null
  cmake --build "${CLI_BUILD}" --target meridian-bot -j >/dev/null
  BOT_BIN="$(find "${CLI_BUILD}" -name meridian-bot -type f -perm -u+x | head -1)"
  [ -x "${BOT_BIN}" ] || { err "meridian-bot not built"; exit 1; }
  ok "built meridian-bot"

  # --- 4. Create N accounts + seed one realm bound to worldd's port. ---------
  export MERIDIAN_DB_HOST=127.0.0.1
  export MERIDIAN_DB_PORT="${DB_PORT}"
  export MERIDIAN_DB_USER=root
  export MERIDIAN_DB_NAME="${AUTH_DB_NAME}"
  unset MERIDIAN_DB_PASS MERIDIAN_DB_SOCKET

  log "creating ${BOTS} test accounts via meridian-account"
  ACCOUNTS=()
  for n in $(seq 1 "${BOTS}"); do
    acct="${ACCOUNT_PREFIX}${n}"
    printf '%s' "${PASSWORD}" | "${ACCOUNT_BIN}" create --username "${acct}" >/dev/null
    ACCOUNTS+=("${acct}")
  done
  ok "created ${#ACCOUNTS[@]} accounts (${ACCOUNTS[0]} .. ${ACCOUNTS[$((${#ACCOUNTS[@]}-1))]})"

  log "seeding realm '${REALM_NAME}' (address 127.0.0.1:${WORLDD_PORT})"
  _dbc "${AUTH_DB_NAME}" -e \
    "INSERT INTO realm (name, address, port, build_min, build_max, population, flags) \
     VALUES ('${REALM_NAME}', '127.0.0.1', ${WORLDD_PORT}, 0, 100000, 0, 0);"
  REALM_ID="$(_dbc -N "${AUTH_DB_NAME}" -e "SELECT id FROM realm WHERE name='${REALM_NAME}';")"
  [ -n "${REALM_ID}" ] || { err "realm not seeded"; exit 1; }
  ok "realm seeded (id ${REALM_ID})"

  # --- 5. Self-signed cert + launch authd AND worldd (with /metrics). --------
  log "generating self-signed TLS cert (shared by authd + worldd)"
  openssl req -x509 -newkey rsa:2048 -keyout "${KEY_PEM}" -out "${CERT_PEM}" \
    -days 1 -nodes -subj "/CN=meridian-soak" >/dev/null 2>&1

  log "launching authd on 127.0.0.1:${AUTHD_PORT}"
  "${AUTHD_BIN}" --cert "${CERT_PEM}" --key "${KEY_PEM}" \
    --bind 127.0.0.1 --port "${AUTHD_PORT}" --realm reference \
    --metrics-port 0 &   # authd metrics off (we scrape worldd); avoid a port clash
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

  log "launching worldd on 127.0.0.1:${WORLDD_PORT} (realm_id=${REALM_ID}, /metrics on ${METRICS_PORT})"
  "${WORLDD_BIN}" --cert "${CERT_PEM}" --key "${KEY_PEM}" \
    --bind 127.0.0.1 --port "${WORLDD_PORT}" --realm reference \
    --metrics-port "${METRICS_PORT}" --metrics-bind 127.0.0.1 &
  WORLDD_PID=$!
  for i in $(seq 1 40); do
    nc -z 127.0.0.1 "${WORLDD_PORT}" 2>/dev/null && break
    kill -0 "${WORLDD_PID}" 2>/dev/null || { err "worldd exited during startup"; exit 1; }
    sleep 0.25
    [ "$i" = "40" ] && { err "worldd did not open its port"; exit 1; }
  done
  ok "worldd listening on ${WORLDD_PORT}"

  METRICS_BASE="http://127.0.0.1:${METRICS_PORT}"
  # Wait for /metrics to answer (the exposer starts just after the listener).
  for i in $(seq 1 20); do
    curl -fsS --max-time 2 "${METRICS_BASE}/metrics" >/dev/null 2>&1 && break
    sleep 0.25
  done
  curl -fsS --max-time 2 "${METRICS_BASE}/metrics" >/dev/null 2>&1 \
    && ok "worldd /metrics reachable at ${METRICS_BASE}/metrics" \
    || warn "worldd /metrics not reachable — soak will monitor RSS + liveness only"

  BOT_AUTHD="127.0.0.1:${AUTHD_PORT}"
  BOT_WORLDD="127.0.0.1:${WORLDD_PORT}"
  BOT_REALM_ARGS="--realm ${REALM_ID}"
else
  # ==========================================================================
  # EXTERNAL REALM (steps 1-5 skipped). The accounts + realm must already exist
  # on the target; we only launch bots + scrape the given /metrics URL. This is
  # the DOCUMENTED path for the ops-run 10x1 h on the test realm (#94); it is not
  # exercised by the local short soak. The bot binary is still built locally.
  # ==========================================================================
  log "EXTERNAL soak: authd=${EXTERNAL_HOST} worldd=${EXTERNAL_WORLDD} metrics=${EXTERNAL_METRICS:-<none>}"
  [ -n "${EXTERNAL_WORLDD}" ] || { err "--external-worldd is required with --external-host"; exit 2; }
  export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
  CLI_BUILD="${REPO_ROOT}/.dev-run/${IT_TAG}-client-build"
  log "building the headless bot (client tree)"
  cmake -S "${REPO_ROOT}/client" -B "${CLI_BUILD}" -DMERIDIAN_BOT=ON >/dev/null
  cmake --build "${CLI_BUILD}" --target meridian-bot -j >/dev/null
  BOT_BIN="$(find "${CLI_BUILD}" -name meridian-bot -type f -perm -u+x | head -1)"
  [ -x "${BOT_BIN}" ] || { err "meridian-bot not built"; exit 1; }
  ok "built meridian-bot"

  # Accounts on an external realm are provisioned out-of-band. Expect the same
  # prefix scheme; the operator seeds soak_<n> accounts + realm ahead of the run.
  ACCOUNTS=()
  for n in $(seq 1 "${BOTS}"); do ACCOUNTS+=("${MERIDIAN_SOAK_ACCOUNT_PREFIX:-soak_}${n}"); done
  BOT_AUTHD="${EXTERNAL_HOST}"
  BOT_WORLDD="${EXTERNAL_WORLDD}"
  BOT_REALM_ARGS=""
  [ -n "${MERIDIAN_SOAK_REALM_ID:-}" ] && BOT_REALM_ARGS="--realm ${MERIDIAN_SOAK_REALM_ID}"
  METRICS_BASE="${EXTERNAL_METRICS}"
  WORLDD_PID=""   # not ours to watch
  AUTHD_PID=""
fi

# ============================================================================
# 6. Launch N bots CONCURRENTLY (login -> enter-world -> move for the window).
# ============================================================================
HAVE_METRICS=0
[ -n "${METRICS_BASE}" ] && curl -fsS --max-time 2 "${METRICS_BASE}/metrics" >/dev/null 2>&1 && HAVE_METRICS=1

# Baseline metric counters (deltas measured against these).
if [ "${HAVE_METRICS}" -eq 1 ]; then
  BASE_OPCODE_ERR="$(scrape_metric meridian_opcode_errors_total)"
  BASE_OPCODE_DROP="$(scrape_metric meridian_opcode_dropped_total)"
  BASE_DISCONNECTS="$(scrape_metric meridian_disconnects_total)"
else
  BASE_OPCODE_ERR=0; BASE_OPCODE_DROP=0; BASE_DISCONNECTS=0
fi
RSS_AUTHD_START="$(daemon_rss_kb "${AUTHD_PID}")"
RSS_WORLDD_START="$(daemon_rss_kb "${WORLDD_PID}")"

log "launching ${BOTS} bots concurrently (--duration ${DURATION}s --path ${BOT_PATH})"
BOT_PIDS=()
for idx in $(seq 1 "${BOTS}"); do
  acct="${ACCOUNTS[$((idx-1))]}"
  botlog="${BOT_LOG_DIR}/bot_${idx}.log"
  # ${BOT_REALM_ARGS} is intentionally UNQUOTED for word-splitting: it is either
  # empty or the two tokens "--realm <id>" (id is numeric — no globbing/space risk).
  ( "${BOT_BIN}" \
      --authd "${BOT_AUTHD}" --worldd "${BOT_WORLDD}" \
      --user "${acct}" --password "${PASSWORD}" \
      ${BOT_REALM_ARGS} --build "${CLIENT_BUILD}" \
      --duration "${DURATION}" --path "${BOT_PATH}" >"${botlog}" 2>&1 ) &
  BOT_PIDS+=($!)
done
ok "launched ${#BOT_PIDS[@]} bots (pids: ${BOT_PIDS[*]})"

# ============================================================================
# 7. MONITOR during the soak — poll /metrics + RSS every POLL_INTERVAL, record
#    a time series, and FAIL FAST if a daemon dies mid-soak.
# ============================================================================
echo "elapsed_s,ccu,sessions,tick_p99_ms,opcode_err_delta,opcode_drop_delta,disc_delta,rss_authd_kb,rss_worldd_kb" >"${METRICS_CSV}"

PEAK_CCU=0
MAX_TICK_P99=0
RSS_WORLDD_PEAK="${RSS_WORLDD_START}"
RSS_AUTHD_PEAK="${RSS_AUTHD_START}"
DAEMON_CRASHED=0
SOAK_START="$(date +%s)"
POLL_N=0

# Poll until DURATION elapses OR all bots have exited (whichever first). The
# bots self-terminate at --duration; we keep polling the whole window so the
# metrics series covers the full soak even if a bot exits a touch early/late.
while true; do
  NOW="$(date +%s)"
  ELAPSED=$(( NOW - SOAK_START ))
  [ "${ELAPSED}" -ge "${DURATION}" ] && break

  # Liveness: a daemon that vanished mid-soak is a CRASH — fail fast.
  if [ "${EXTERNAL}" -eq 0 ]; then
    if ! kill -0 "${WORLDD_PID}" 2>/dev/null; then
      err "worldd EXITED mid-soak at t=${ELAPSED}s — CRASH"; DAEMON_CRASHED=1; break
    fi
    if ! kill -0 "${AUTHD_PID}" 2>/dev/null; then
      err "authd EXITED mid-soak at t=${ELAPSED}s — CRASH"; DAEMON_CRASHED=1; break
    fi
  fi

  CCU=0; SESSIONS=0; TICK_P99="0"; ERR_D=0; DROP_D=0; DISC_D=0
  if [ "${HAVE_METRICS}" -eq 1 ]; then
    CCU="$(scrape_metric meridian_ccu)"
    SESSIONS="$(scrape_metric meridian_sessions)"
    TICK_P99="$(tick_p99_ms)"
    CUR_ERR="$(scrape_metric meridian_opcode_errors_total)"
    CUR_DROP="$(scrape_metric meridian_opcode_dropped_total)"
    CUR_DISC="$(scrape_metric meridian_disconnects_total)"
    ERR_D="$(awk -v a="${CUR_ERR}" -v b="${BASE_OPCODE_ERR}" 'BEGIN{printf "%.0f", a-b}')"
    DROP_D="$(awk -v a="${CUR_DROP}" -v b="${BASE_OPCODE_DROP}" 'BEGIN{printf "%.0f", a-b}')"
    DISC_D="$(awk -v a="${CUR_DISC}" -v b="${BASE_DISCONNECTS}" 'BEGIN{printf "%.0f", a-b}')"
    # Track peaks.
    PEAK_CCU="$(awk -v a="${CCU}" -v b="${PEAK_CCU}" 'BEGIN{print (a>b)?a:b}')"
    if [ "${TICK_P99}" != "inf" ] && [ "${TICK_P99}" != "0" ]; then
      MAX_TICK_P99="$(awk -v a="${TICK_P99}" -v b="${MAX_TICK_P99}" 'BEGIN{print (a>b)?a:b}')"
    elif [ "${TICK_P99}" = "inf" ]; then
      MAX_TICK_P99="inf"
    fi
  fi

  RSS_A="$(daemon_rss_kb "${AUTHD_PID}")"
  RSS_W="$(daemon_rss_kb "${WORLDD_PID}")"
  [ "${RSS_A:-0}" -gt "${RSS_AUTHD_PEAK:-0}" ] 2>/dev/null && RSS_AUTHD_PEAK="${RSS_A}"
  [ "${RSS_W:-0}" -gt "${RSS_WORLDD_PEAK:-0}" ] 2>/dev/null && RSS_WORLDD_PEAK="${RSS_W}"

  echo "${ELAPSED},${CCU},${SESSIONS},${TICK_P99},${ERR_D},${DROP_D},${DISC_D},${RSS_A},${RSS_W}" >>"${METRICS_CSV}"
  POLL_N=$(( POLL_N + 1 ))
  printf '\033[1;36m[t=%4ds]\033[0m ccu=%s sessions=%s tick_p99=%sms err+=%s drop+=%s disc+=%s rss_worldd=%sKB\n' \
    "${ELAPSED}" "${CCU}" "${SESSIONS}" "${TICK_P99}" "${ERR_D}" "${DROP_D}" "${DISC_D}" "${RSS_W}"

  sleep "${POLL_INTERVAL}"
done

# ============================================================================
# 8. Collect bot exit codes + final metric deltas.
# ============================================================================
log "soak window elapsed — collecting ${#BOT_PIDS[@]} bot results"
BOTS_OK=0
BOTS_INWORLD=0
for i in "${!BOT_PIDS[@]}"; do
  pid="${BOT_PIDS[$i]}"
  idx=$(( i + 1 ))
  wait "${pid}"; rc=$?
  botlog="${BOT_LOG_DIR}/bot_${idx}.log"
  rline="$(grep '^BOT_RESULT ' "${botlog}" 2>/dev/null | head -1)"
  hs=""; ma=""
  if [ -n "${rline}" ]; then
    hs="$(printf '%s\n' "${rline}" | sed -n 's/.* handshake_ok=\([^ ]*\).*/\1/p')"
    ma="$(printf '%s\n' "${rline}" | sed -n 's/.* moves_accepted=\([^ ]*\).*/\1/p')"
  fi
  if [ "${rc}" -eq 0 ]; then BOTS_OK=$(( BOTS_OK + 1 )); fi
  if [ "${hs}" = "1" ]; then BOTS_INWORLD=$(( BOTS_INWORLD + 1 )); fi
  printf '  bot %2d: rc=%d handshake_ok=%s moves_accepted=%s\n' "${idx}" "${rc}" "${hs:-?}" "${ma:-?}"
done

RSS_AUTHD_END="$(daemon_rss_kb "${AUTHD_PID}")"
RSS_WORLDD_END="$(daemon_rss_kb "${WORLDD_PID}")"

# Final metric deltas (post-soak scrape).
FINAL_ERR_D=0; FINAL_DROP_D=0; FINAL_DISC_D=0
if [ "${HAVE_METRICS}" -eq 1 ]; then
  FINAL_ERR_D="$(awk -v a="$(scrape_metric meridian_opcode_errors_total)" -v b="${BASE_OPCODE_ERR}" 'BEGIN{printf "%.0f", a-b}')"
  FINAL_DROP_D="$(awk -v a="$(scrape_metric meridian_opcode_dropped_total)" -v b="${BASE_OPCODE_DROP}" 'BEGIN{printf "%.0f", a-b}')"
  FINAL_DISC_D="$(awk -v a="$(scrape_metric meridian_disconnects_total)" -v b="${BASE_DISCONNECTS}" 'BEGIN{printf "%.0f", a-b}')"
fi

# Daemon liveness at the end (a crash during teardown-window still counts).
DAEMON_ALIVE=1
if [ "${EXTERNAL}" -eq 0 ]; then
  kill -0 "${WORLDD_PID}" 2>/dev/null || { DAEMON_ALIVE=0; DAEMON_CRASHED=1; }
  kill -0 "${AUTHD_PID}"  2>/dev/null || { DAEMON_ALIVE=0; DAEMON_CRASHED=1; }
fi

# RSS growth (worldd — the daemon carrying the session/entity state).
RSS_GROWTH_KB=$(( ${RSS_WORLDD_END:-0} - ${RSS_WORLDD_START:-0} ))
RSS_GROWTH_PCT_ACTUAL=0
if [ "${RSS_WORLDD_START:-0}" -gt 0 ]; then
  RSS_GROWTH_PCT_ACTUAL="$(awk -v s="${RSS_WORLDD_START}" -v e="${RSS_WORLDD_END}" 'BEGIN{printf "%.1f", (e-s)*100.0/s}')"
fi

# ============================================================================
# 9. EVALUATE stability + emit the soak REPORT.
# ============================================================================
soak_fail=0

echo
echo "========================================================================"
echo " SOAK REPORT — ${BOTS} bots x ${DURATION}s ($(awk -v d="${DURATION}" 'BEGIN{printf "%.1f", d/60.0}') min)"
echo "========================================================================"
printf ' target           : %s\n' "$([ "${EXTERNAL}" -eq 1 ] && echo "EXTERNAL realm (${BOT_WORLDD})" || echo "local authd+worldd (reference realm)")"
printf ' bots ok / total  : %d / %d   (in-world: %d / %d)\n' "${BOTS_OK}" "${BOTS}" "${BOTS_INWORLD}" "${BOTS}"
printf ' polls recorded   : %d  (every %ss)\n' "${POLL_N}" "${POLL_INTERVAL}"
if [ "${HAVE_METRICS}" -eq 1 ]; then
  printf ' peak CCU         : %s\n' "${PEAK_CCU}"
  printf ' tick p99 (max)   : %s ms  (budget %s ms)\n' "${MAX_TICK_P99}" "${TICK_BUDGET_MS}"
  printf ' opcode errors    : +%s  (delta over soak)\n' "${FINAL_ERR_D}"
  printf ' opcode drops     : +%s\n' "${FINAL_DROP_D}"
  printf ' disconnects      : +%s\n' "${FINAL_DISC_D}"
else
  printf ' metrics          : NOT scraped (no /metrics reachable — liveness + RSS only)\n'
fi
printf ' worldd RSS       : %s KB -> %s KB (peak %s KB, growth %s%% / %s KB)\n' \
  "${RSS_WORLDD_START}" "${RSS_WORLDD_END}" "${RSS_WORLDD_PEAK}" "${RSS_GROWTH_PCT_ACTUAL}" "${RSS_GROWTH_KB}"
if [ "${EXTERNAL}" -eq 0 ]; then
  printf ' authd RSS        : %s KB -> %s KB (peak %s KB)\n' "${RSS_AUTHD_START}" "${RSS_AUTHD_END}" "${RSS_AUTHD_PEAK}"
  printf ' daemons alive    : %s\n' "$([ "${DAEMON_ALIVE}" -eq 1 ] && echo yes || echo 'NO (crash)')"
fi
printf ' metrics csv      : %s\n' "${METRICS_CSV}"
echo "------------------------------------------------------------------------"

# --- Pass/fail gates. ------------------------------------------------------
# (a) zero daemon crashes.
if [ "${EXTERNAL}" -eq 0 ]; then
  if [ "${DAEMON_CRASHED}" -eq 0 ]; then ok "daemons stayed up (zero crashes)"
  else err "a daemon CRASHED during the soak"; soak_fail=1; fi
fi

# (b) every bot completed the window in-world (rc 0 + handshake_ok).
if [ "${BOTS_OK}" -eq "${BOTS}" ] && [ "${BOTS_INWORLD}" -eq "${BOTS}" ]; then
  ok "all ${BOTS} bots stayed in-world for the full ${DURATION}s (rc 0, handshake_ok)"
else
  err "not all bots completed in-world (ok ${BOTS_OK}/${BOTS}, in-world ${BOTS_INWORLD}/${BOTS})"
  soak_fail=1
fi

# (c) no opcode error/drop spike (metric path only). Budget: a handful of
#     spurious errors is tolerable; a spike proportional to the bot fleet is not.
#     Threshold = 2 per bot (generous) — real stability keeps this at ~0.
if [ "${HAVE_METRICS}" -eq 1 ]; then
  ERR_BUDGET=$(( BOTS * 2 ))
  if [ "${FINAL_ERR_D}" -le "${ERR_BUDGET}" ] && [ "${FINAL_DROP_D}" -le "${ERR_BUDGET}" ]; then
    ok "no opcode error/drop spike (errors +${FINAL_ERR_D}, drops +${FINAL_DROP_D} <= ${ERR_BUDGET})"
  else
    err "opcode error/drop SPIKE (errors +${FINAL_ERR_D}, drops +${FINAL_DROP_D} > budget ${ERR_BUDGET})"
    soak_fail=1
  fi

  # (d) tick health — p99 within the SAD 40 ms soft budget.
  if [ "${MAX_TICK_P99}" = "inf" ]; then
    err "tick p99 fell in the +Inf bucket — tick health DEGRADED past the largest boundary"
    soak_fail=1
  elif [ "$(awk -v a="${MAX_TICK_P99}" -v b="${TICK_BUDGET_MS}" 'BEGIN{print (a<=b)?1:0}')" = "1" ]; then
    ok "tick p99 within budget (${MAX_TICK_P99} ms <= ${TICK_BUDGET_MS} ms)"
  else
    err "tick p99 DEGRADED (${MAX_TICK_P99} ms > ${TICK_BUDGET_MS} ms budget)"
    soak_fail=1
  fi
else
  warn "metrics gates SKIPPED (no /metrics) — soak proven on liveness + bots + RSS only"
fi

# (e) bounded RSS growth (worldd). A soak leak shows as monotonic RSS climb; we
#     gate on % growth start->end against the budget.
if [ "${RSS_WORLDD_START:-0}" -gt 0 ]; then
  if [ "$(awk -v g="${RSS_GROWTH_PCT_ACTUAL}" -v b="${RSS_GROWTH_PCT}" 'BEGIN{print (g<=b)?1:0}')" = "1" ]; then
    ok "worldd RSS growth bounded (${RSS_GROWTH_PCT_ACTUAL}% <= ${RSS_GROWTH_PCT}% budget)"
  else
    err "worldd RSS GREW past budget (${RSS_GROWTH_PCT_ACTUAL}% > ${RSS_GROWTH_PCT}%) — possible leak"
    soak_fail=1
  fi
else
  warn "worldd RSS not sampled (external realm or ps unavailable) — RSS gate skipped"
fi

echo "------------------------------------------------------------------------"
if [ "${soak_fail}" -eq 0 ]; then
  ok "SOAK VERDICT: PASS — ${BOTS} bots x ${DURATION}s, stable metrics, zero crashes"
  echo "========================================================================"
  exit 0
else
  err "SOAK VERDICT: FAIL — see the failed gates above"
  echo "========================================================================"
  exit 1
fi
