#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# deploy/scripts/redeploy.sh — nightly test-realm redeploy + smoke + rollback (#94).
#
# The server-ops "redeploy → smoke → rollback" cycle for the IT-M0 test realm
# (docs/it-m0-runbook.md §3). It pulls the target GHCR images to the test-realm
# host, records the version it is REPLACING (for rollback), rolls the new version
# out via the D-30 compose stack (deploy/docker/docker-compose.yml, #177), runs a
# smoke test, and — if the smoke FAILS — ROLLS BACK to the recorded version and
# exits non-zero so the nightly workflow (.github/workflows/nightly-redeploy.yml)
# pages the team.
#
# Feeds:  the nightly test realm tracks `main` (D-30 §10 rule 5); complements
#         #159 (the host) and #175 (GHCR autopublish of authd/worldd images).
#
# ── The deploy model ─────────────────────────────────────────────────────────
# The daemons run as containers via `docker compose` on the test-realm host. The
# image tag is pinned per deploy with MERIDIAN_TAG (compose default: `latest`).
# We redeploy by pulling a specific tag and `compose up -d` — same stack, new
# tag. "Rollback" = re-deploy the PREVIOUS tag we captured before touching it.
#
# ── The smoke (issue #94: "headless-bot smoke (login→enter→move)") ───────────
# Two layers, cheap → thorough, mirroring scripts/dev/run-local.sh --smoke and
# client/test/run_bot_client_it.sh:
#   L1 (always)  compose reports both daemons up + healthy (the image HEALTHCHECK
#                is the daemon's own --version), and a TLS 1.3 handshake reaches
#                authd:7100 (IF-1) and worldd:7200 (IF-2). Proves "the daemons
#                come up" (issue #94).
#   L2 (if a bot is available)  the headless meridian-bot (#111) drives the FULL
#                path: login (SRP-6a/TLS) → enter-world → move, and we assert its
#                BOT_RESULT line (handshake_ok=1 AND moves_accepted>0). Proves a
#                "basic login/enter-world works" (issue #94). Skipped with a clear
#                notice if no bot binary is configured (BOT_BIN unset / absent),
#                so the L1 smoke is never blocked on the client toolchain.
#
# ── Modes ────────────────────────────────────────────────────────────────────
#   (default)         Real run against $DEPLOY_TARGET (SSH host) — pull, deploy,
#                     smoke, roll back on failure.
#   --dry-run         Print every remote command instead of running it; drive the
#                     deploy→smoke→rollback DECISION PATHS locally with NO host and
#                     NO Docker. Combine with --simulate-smoke=pass|fail to force
#                     the smoke outcome and prove the rollback branch is taken.
#                     This is what CI / `bash -n` / shellcheck exercise.
#   --local           Run against the LOCAL docker compose stack (deploy/docker)
#                     instead of an SSH host — for a real end-to-end rehearsal on
#                     a box that has Docker. Still does pull→deploy→smoke→rollback.
#
# ── Configuration (env) ──────────────────────────────────────────────────────
#   DEPLOY_TARGET     SSH target for the test-realm host, e.g. deploy@realm.example
#                     (#159). Required for a real (non --local / non --dry-run) run.
#   MERIDIAN_TAG      Image tag to deploy (short SHA or `latest`). Default: latest.
#   SSH_OPTS          Extra ssh options (e.g. -i key -p 2222). Default: batch-mode.
#   COMPOSE_DIR       Directory ON THE HOST holding docker-compose.yml + certs +
#                     db-init. Default: /opt/meridian/deploy/docker.
#   COMPOSE_FILE      Compose filename. Default: docker-compose.yml.
#   AUTHD_HOST/PORT   Smoke target for authd. Default: 127.0.0.1 / 7100 (probed
#                     ON the host, over the loopback the ports are published to).
#   WORLDD_HOST/PORT  Smoke target for worldd. Default: 127.0.0.1 / 7200.
#   BOT_BIN           Path (ON THE HOST) to meridian-bot for the L2 smoke. Unset
#                     => L2 skipped with a notice.
#   BOT_USER/BOT_PASS Account creds the bot logs in with (pre-seeded via
#                     meridian-account, runbook §4.1). Required only if BOT_BIN set.
#   SMOKE_RETRIES     TLS-handshake retry budget per daemon. Default: 40 (~10 s).
#
# ── Idempotence ──────────────────────────────────────────────────────────────
# Re-running with the same MERIDIAN_TAG is a no-op deploy (compose pulls the same
# digest, `up -d` sees no change). A failed run always leaves the host on the
# PREVIOUS good tag (rollback), never a half-deployed state.
#
# ── Verify locally (no host, no Docker) ──────────────────────────────────────
#   bash -n   deploy/scripts/redeploy.sh          # (syntax)
#   $ shellcheck deploy/scripts/redeploy.sh       # (lint)
#   deploy/scripts/redeploy.sh --dry-run --simulate-smoke=pass   # deploy path
#   deploy/scripts/redeploy.sh --dry-run --simulate-smoke=fail   # rollback path
set -euo pipefail

# ── Logging (mirrors scripts/dev/_common.sh so ops output looks consistent). ──
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  _C_RESET=$'\033[0m'; _C_BOLD=$'\033[1m'
  _C_BLUE=$'\033[34m'; _C_GREEN=$'\033[32m'; _C_YELLOW=$'\033[33m'; _C_RED=$'\033[31m'
else
  _C_RESET=''; _C_BOLD=''; _C_BLUE=''; _C_GREEN=''; _C_YELLOW=''; _C_RED=''
fi
log()  { printf '%s==>%s %s\n' "${_C_BLUE}${_C_BOLD}" "${_C_RESET}" "$*"; }
ok()   { printf '%s  OK%s %s\n' "${_C_GREEN}" "${_C_RESET}" "$*"; }
warn() { printf '%s  ! %s%s\n' "${_C_YELLOW}" "$*" "${_C_RESET}" >&2; }
die()  { printf '%s  X %s%s\n' "${_C_RED}${_C_BOLD}" "$*" "${_C_RESET}" >&2; exit 1; }

# ── Config with defaults. ────────────────────────────────────────────────────
MODE="remote"                       # remote | local
DRY_RUN=0
SIMULATE_SMOKE=""                   # "", pass, fail (dry-run only)
MERIDIAN_TAG="${MERIDIAN_TAG:-latest}"
DEPLOY_TARGET="${DEPLOY_TARGET:-}"
SSH_OPTS="${SSH_OPTS:--o BatchMode=yes -o StrictHostKeyChecking=accept-new}"
COMPOSE_DIR="${COMPOSE_DIR:-/opt/meridian/deploy/docker}"
COMPOSE_FILE="${COMPOSE_FILE:-docker-compose.yml}"
AUTHD_HOST="${AUTHD_HOST:-127.0.0.1}"; AUTHD_PORT="${AUTHD_PORT:-7100}"
WORLDD_HOST="${WORLDD_HOST:-127.0.0.1}"; WORLDD_PORT="${WORLDD_PORT:-7200}"
BOT_BIN="${BOT_BIN:-}"
BOT_USER="${BOT_USER:-}"; BOT_PASS="${BOT_PASS:-}"
SMOKE_RETRIES="${SMOKE_RETRIES:-40}"

usage() { grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run)            DRY_RUN=1 ;;
    --local)              MODE="local" ;;
    --simulate-smoke=*)   SIMULATE_SMOKE="${1#*=}" ;;
    --tag=*)              MERIDIAN_TAG="${1#*=}" ;;
    --target=*)           DEPLOY_TARGET="${1#*=}" ;;
    -h|--help)            usage; exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
  shift
done

case "${SIMULATE_SMOKE}" in
  ""|pass|fail) ;;
  *) die "--simulate-smoke must be 'pass' or 'fail' (got '${SIMULATE_SMOKE}')" ;;
esac
if [ -n "${SIMULATE_SMOKE}" ] && [ "${DRY_RUN}" -ne 1 ]; then
  die "--simulate-smoke only applies with --dry-run"
fi

# ── Remote-exec abstraction ──────────────────────────────────────────────────
# run_remote runs a command "on the host". In --dry-run it PRINTS the command and
# returns success (except the smoke, which is short-circuited by --simulate-smoke).
# For MODE=remote it wraps in ssh; for MODE=local it runs the command directly.
run_remote() {
  local cmd="$1"
  if [ "${DRY_RUN}" -eq 1 ]; then
    printf '   [dry-run] %s\n' "${cmd}" >&2
    return 0
  fi
  if [ "${MODE}" = "remote" ]; then
    # cmd is already a fully-composed remote command line; we intend it to be
    # sent verbatim and run on the host (client-side quoting is deliberate).
    # shellcheck disable=SC2086,SC2029  # SSH_OPTS word-split; cmd runs host-side
    ssh ${SSH_OPTS} "${DEPLOY_TARGET}" "${cmd}"
  else
    bash -c "${cmd}"
  fi
}

# The compose invocation prefix, used on the host for every stack operation.
compose_cmd() {
  printf 'cd %q && MERIDIAN_TAG=%q docker compose -f %q' \
    "${COMPOSE_DIR}" "$1" "${COMPOSE_FILE}"
}

# ── Preconditions ────────────────────────────────────────────────────────────
if [ "${DRY_RUN}" -ne 1 ] && [ "${MODE}" = "remote" ] && [ -z "${DEPLOY_TARGET}" ]; then
  die "DEPLOY_TARGET is not set (the test-realm host, #159). Set it, or use --local / --dry-run."
fi

log "Meridian test-realm redeploy (#94)"
log "  mode=${MODE} dry_run=${DRY_RUN} tag=${MERIDIAN_TAG} target=${DEPLOY_TARGET:-<local>}"
log "  compose=${COMPOSE_DIR}/${COMPOSE_FILE}"

# ── 1. Record the currently-running version (for rollback). ──────────────────
# We read the tag off the running authd container's image reference. If nothing
# is running yet (first deploy), there is no rollback target — we note that and
# proceed (a failed first deploy has nothing to roll back TO; it just fails).
PREVIOUS_TAG=""
capture_previous_tag() {
  if [ "${DRY_RUN}" -eq 1 ]; then
    # Simulate an existing deployment so the rollback path is exercisable.
    PREVIOUS_TAG="prev-abc1234"
    log "1. Recorded currently-running version: ${PREVIOUS_TAG} (dry-run simulated)"
    return 0
  fi
  local ref
  # `docker compose ps -q authd` -> container id; inspect its image tag.
  ref="$(run_remote "$(compose_cmd "${MERIDIAN_TAG}") ps -q authd 2>/dev/null | head -n1 | xargs -r docker inspect --format '{{ index .Config.Image }}' 2>/dev/null || true")"
  if [ -n "${ref}" ]; then
    PREVIOUS_TAG="${ref##*:}"
    [ -n "${PREVIOUS_TAG}" ] || PREVIOUS_TAG="latest"
    log "1. Recorded currently-running version: ${PREVIOUS_TAG} (image: ${ref})"
  else
    warn "1. No running authd found — this looks like a FIRST deploy; no rollback target."
  fi
}

# ── 2. Pull + deploy a given tag via compose. ────────────────────────────────
deploy_tag() {
  local tag="$1"
  log "Deploying tag '${tag}' ..."
  run_remote "$(compose_cmd "${tag}") pull authd worldd" \
    || die "image pull failed for tag '${tag}'"
  run_remote "$(compose_cmd "${tag}") up -d" \
    || die "compose up failed for tag '${tag}'"
  ok "compose up -d complete for tag '${tag}'"
}

# ── 3. Smoke: L1 health+reachability, L2 (optional) login→enter→move. ────────
# Returns 0 on pass, non-zero on fail. In --dry-run the outcome is forced by
# --simulate-smoke so both decision paths are locally testable.
smoke_test() {
  if [ "${DRY_RUN}" -eq 1 ]; then
    case "${SIMULATE_SMOKE}" in
      fail) warn "SMOKE (simulated) => FAIL"; return 1 ;;
      *)    ok   "SMOKE (simulated) => PASS"; return 0 ;;
    esac
  fi

  local rc=0

  # L1a: compose reports both daemons running (image HEALTHCHECK = daemon --version).
  log "Smoke L1a: compose service health"
  if ! run_remote "$(compose_cmd "${MERIDIAN_TAG}") ps --status running --services | grep -qx authd" \
     || ! run_remote "$(compose_cmd "${MERIDIAN_TAG}") ps --status running --services | grep -qx worldd"; then
    warn "L1a FAILED: authd and/or worldd not in 'running' state"
    rc=1
  else
    ok "L1a: authd + worldd running"
  fi

  # L1b: TLS 1.3 handshake reaches each daemon (mirrors run-local.sh wait_listen).
  # Probed ON the host over the published loopback ports.
  log "Smoke L1b: TLS 1.3 reachability (authd:${AUTHD_PORT}, worldd:${WORLDD_PORT})"
  tls_reach "authd"  "${AUTHD_HOST}"  "${AUTHD_PORT}"  || rc=1
  tls_reach "worldd" "${WORLDD_HOST}" "${WORLDD_PORT}" || rc=1

  # L2: optional headless-bot login -> enter-world -> move (#111 / issue #94).
  if [ -n "${BOT_BIN}" ]; then
    if [ -z "${BOT_USER}" ] || [ -z "${BOT_PASS}" ]; then
      warn "L2 skipped: BOT_BIN set but BOT_USER/BOT_PASS missing"
    else
      log "Smoke L2: headless-bot login -> enter-world -> move"
      bot_smoke || rc=1
    fi
  else
    warn "L2 skipped: BOT_BIN not configured (L1 health+reachability only)."
  fi

  return "${rc}"
}

# TLS 1.3 handshake to host:port on the deploy host, retrying up to SMOKE_RETRIES.
tls_reach() {
  local name="$1" host="$2" port="$3"
  local probe="for i in \$(seq 1 ${SMOKE_RETRIES}); do if echo | openssl s_client -connect ${host}:${port} -tls1_3 >/dev/null 2>&1; then exit 0; fi; sleep 0.25; done; exit 1"
  if run_remote "${probe}"; then
    ok "L1b: ${name} TLS 1.3 reachable on ${host}:${port}"
    return 0
  fi
  warn "L1b FAILED: ${name} not reachable (TLS 1.3) on ${host}:${port}"
  return 1
}

# L2: run meridian-bot --path square and assert handshake_ok=1 + moves_accepted>0.
# Mirrors the BOT_RESULT assertion in client/test/run_bot_client_it.sh.
bot_smoke() {
  local out
  out="$(run_remote "${BOT_BIN} --authd ${AUTHD_HOST}:${AUTHD_PORT} --worldd ${WORLDD_HOST}:${WORLDD_PORT} --user ${BOT_USER} --password ${BOT_PASS} --path square --duration 5 2>&1" || true)"
  printf '%s\n' "${out}" | sed 's/^/     bot| /' >&2
  local result; result="$(printf '%s\n' "${out}" | grep -E '^BOT_RESULT' | tail -n1 || true)"
  if printf '%s' "${result}" | grep -q 'handshake_ok=1' \
     && printf '%s' "${result}" | grep -qE 'moves_accepted=[1-9][0-9]*'; then
    ok "L2: login -> enter-world -> move OK (${result})"
    return 0
  fi
  warn "L2 FAILED: bot did not complete login->enter->move (${result:-no BOT_RESULT line})"
  return 1
}

# ── Orchestration: capture -> deploy -> smoke -> (rollback on fail). ─────────
main() {
  capture_previous_tag

  log "2. Deploy target tag"
  deploy_tag "${MERIDIAN_TAG}"

  log "3. Smoke test"
  if smoke_test; then
    ok "SMOKE PASSED — test realm is on tag '${MERIDIAN_TAG}'."
    log "Redeploy complete."
    return 0
  fi

  # ── Smoke failed: ROLL BACK. ───────────────────────────────────────────────
  warn "SMOKE FAILED on tag '${MERIDIAN_TAG}'."
  if [ -z "${PREVIOUS_TAG}" ]; then
    die "ROLLBACK IMPOSSIBLE: no previous version recorded (first deploy). Test realm left on FAILED tag '${MERIDIAN_TAG}' — manual intervention required."
  fi
  warn "4. ROLLING BACK to previous version '${PREVIOUS_TAG}' ..."
  if deploy_tag "${PREVIOUS_TAG}"; then
    warn "Rolled back to '${PREVIOUS_TAG}'. Re-checking smoke on the rolled-back version ..."
    if smoke_test; then
      warn "Rollback smoke PASSED — realm restored to '${PREVIOUS_TAG}'."
    else
      warn "Rollback smoke ALSO FAILED — realm may be degraded on '${PREVIOUS_TAG}'."
    fi
  else
    die "ROLLBACK DEPLOY FAILED — test realm is in a BAD state. Manual intervention required."
  fi
  # Fail loudly so the nightly workflow marks the job red and pages the team.
  die "Redeploy FAILED: smoke on '${MERIDIAN_TAG}' failed; rolled back to '${PREVIOUS_TAG}'."
}

main
