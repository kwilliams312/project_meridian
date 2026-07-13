#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/post-update.sh — post a Meridian development update to Discord.
#
# The orchestration lead (see CLAUDE.md) calls this to announce delegation-loop
# milestones (epic/story lifecycle, PR merged into dev) to a Discord channel via
# an incoming webhook. One-way only; no bot, no gateway. Sub-agents never post.
#
# Usage:
#   scripts/dev/post-update.sh <event> "<message>" [url]
#     events:  epic-open | story-dispatch | story-close | pr-merged | note
#     --dry-run   print the JSON payload to stdout instead of POSTing
#     --help      show this help
#
# Webhook URL resolution (first hit wins):
#   1. $MERIDIAN_DISCORD_WEBHOOK_URL
#   2. file at ${MERIDIAN_DISCORD_WEBHOOK_FILE:-<repo>/.discord-webhook}
#      (first non-empty, non-'#' line)
#   3. none -> print a skip notice to stderr and exit 0 (never breaks the loop)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WEBHOOK_FILE="${MERIDIAN_DISCORD_WEBHOOK_FILE:-${REPO_ROOT}/.discord-webhook}"

usage() {
  cat <<'EOF'
scripts/dev/post-update.sh <event> "<message>" [url]
  events:  epic-open | story-dispatch | story-close | pr-merged | note
  flags:   --dry-run   print the JSON payload instead of POSTing
           --help      show this help
EOF
}

die() { printf 'post-update: %s\n' "$*" >&2; usage >&2; exit 2; }

dry_run=0
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) dry_run=1; shift ;;
    --help|-h) usage; exit 0 ;;
    --) shift; while [ $# -gt 0 ]; do args+=("$1"); shift; done ;;
    -*) die "unknown flag: $1" ;;
    *) args+=("$1"); shift ;;
  esac
done

[ "${#args[@]}" -ge 2 ] || die "usage: post-update.sh <event> \"<message>\" [url]"
event="${args[0]}"
message="${args[1]}"
url="${args[2]:-}"

case "$event" in
  epic-open)      emoji="📋"; prefix="Epic opened";        color=3447003 ;;
  story-dispatch) emoji="🚀"; prefix="Story dispatched";   color=10181046 ;;
  story-close)    emoji="✅"; prefix="Story closed";       color=3066993 ;;
  pr-merged)      emoji="🔀"; prefix="PR merged into dev"; color=5763719 ;;
  note)           emoji="📝"; prefix="Note";               color=9807270 ;;
  *) die "unknown event: $event (want: epic-open|story-dispatch|story-close|pr-merged|note)" ;;
esac

title="${emoji} ${prefix} — ${message}"
timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

payload="$(
  EMBED_TITLE="$title" EMBED_URL="$url" EMBED_COLOR="$color" EMBED_TS="$timestamp" \
  python3 - <<'PY'
import json
import os

embed = {
    "title": os.environ["EMBED_TITLE"],
    "color": int(os.environ["EMBED_COLOR"]),
    "footer": {"text": "Project Meridian"},
    "timestamp": os.environ["EMBED_TS"],
}
url = os.environ.get("EMBED_URL", "")
if url:
    embed["url"] = url
print(json.dumps({"username": "Meridian Dev", "embeds": [embed]}))
PY
)"

if [ "$dry_run" -eq 1 ]; then
  printf '%s\n' "$payload"
  exit 0
fi

# Resolve the webhook URL.
webhook="${MERIDIAN_DISCORD_WEBHOOK_URL:-}"
if [ -z "$webhook" ] && [ -f "$WEBHOOK_FILE" ]; then
  webhook="$(grep -vE '^[[:space:]]*(#|$)' "$WEBHOOK_FILE" | head -n1 | tr -d '[:space:]' || true)"
fi
if [ -z "$webhook" ]; then
  printf 'post-update: skipping Discord post (no webhook configured)\n' >&2
  exit 0
fi

# POST to Discord; capture the HTTP status.
http_code="$(curl -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/json' \
  -X POST -d "$payload" "$webhook" || true)"

case "$http_code" in
  2*) printf 'post-update: posted (%s) %s\n' "$http_code" "$title" ;;
  *)  printf 'post-update: Discord POST failed (HTTP %s)\n' "${http_code:-none}" >&2; exit 1 ;;
esac
