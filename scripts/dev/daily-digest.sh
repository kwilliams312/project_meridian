#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/daily-digest.sh — post ONE Discord digest of the previous day's
# delegation-loop work, instead of a message per milestone (see CLAUDE.md).
#
# It reconstructs "yesterday" straight from GitHub (the source of truth), so it
# does not depend on anyone having posted per-event updates during the day:
#   * PRs merged into `dev`  — the work that actually shipped,
#   * issues (stories/tasks/epics) closed,
#   * issues opened          — what got started or planned.
# "Yesterday" is the previous *calendar day in America/Los_Angeles* — this is
# meant to run at ~08:00 Pacific (see .github/workflows/daily-digest.yml) as a
# recap of the day before.
#
# The composed markdown is handed to post-update.sh (event `digest`), which owns
# webhook resolution + the actual POST. A quiet day still posts a short digest so
# the channel confirms the pipeline is alive.
#
# Usage:
#   scripts/dev/daily-digest.sh [--dry-run]
#     --dry-run   print the digest payload (via post-update.sh) instead of POSTing
#     --date YYYY-MM-DD   recap this Pacific day instead of yesterday (for testing)
#
# Requires: gh (authenticated; in CI use GH_TOKEN=${{ github.token }}), python3.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

dry_run=0
target_date=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) dry_run=1; shift ;;
    --date) [ $# -ge 2 ] || { echo "daily-digest: --date needs a value" >&2; exit 2; }; target_date="$2"; shift 2 ;;
    --date=*) target_date="${1#--date=}"; shift ;;
    -h|--help) sed -n '/^# Usage:/,/^set /p' "${BASH_SOURCE[0]}" | sed '/^set /d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "daily-digest: unknown arg: $1" >&2; exit 2 ;;
  esac
done

command -v gh >/dev/null 2>&1 || { echo "daily-digest: gh not found" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "daily-digest: python3 not found" >&2; exit 2; }

# Resolve owner/repo from the origin remote so the script is repo-portable.
repo_slug="$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null || true)"
[ -n "$repo_slug" ] || { echo "daily-digest: could not resolve repo (is gh authenticated?)" >&2; exit 2; }

# Compute the Pacific-day window [start, end) as UTC ISO timestamps, plus a
# search lower-bound date (to bound the gh queries) and a human day label.
# All date math in python3 so it is identical on the macOS dev box (BSD date)
# and the ubuntu CI runner (GNU date).
read -r WIN_START WIN_END WIN_LB DAY_LABEL < <(
  TARGET_DATE="$target_date" python3 - <<'PY'
import os
from datetime import datetime, timedelta, time
from zoneinfo import ZoneInfo

pt = ZoneInfo("America/Los_Angeles")
utc = ZoneInfo("UTC")
target = os.environ.get("TARGET_DATE", "").strip()
if target:
    day = datetime.strptime(target, "%Y-%m-%d").date()
else:
    # "Yesterday" relative to now in Pacific time.
    day = (datetime.now(pt) - timedelta(days=1)).date()

start_pt = datetime.combine(day, time.min, tzinfo=pt)
end_pt = start_pt + timedelta(days=1)
start_utc = start_pt.astimezone(utc).strftime("%Y-%m-%dT%H:%M:%SZ")
end_utc = end_pt.astimezone(utc).strftime("%Y-%m-%dT%H:%M:%SZ")
# Lower bound for gh search date qualifiers (UTC-day granular): the earliest UTC
# date the window can touch. It is only a floor to keep result sets small; exact
# filtering happens client-side against start/end below.
lb = start_pt.astimezone(utc).date().isoformat()
label = day.strftime("%a %b %-d, %Y")
print(start_utc, end_utc, lb, label)
PY
)

# Gather from GitHub. `--search "<qualifier>:>=<lb>"` bounds each query server-side
# so we fetch a day-ish of rows, not the whole repo; python filters to the exact
# window. mergedAt/closedAt/createdAt are full ISO timestamps.
prs_json="$(gh pr list --repo "$repo_slug" --base dev --state merged \
  --search "merged:>=$WIN_LB" --limit 200 \
  --json number,title,url,mergedAt 2>/dev/null || echo '[]')"
closed_json="$(gh issue list --repo "$repo_slug" --state closed \
  --search "closed:>=$WIN_LB" --limit 200 \
  --json number,title,url,closedAt,labels 2>/dev/null || echo '[]')"
opened_json="$(gh issue list --repo "$repo_slug" --state all \
  --search "created:>=$WIN_LB" --limit 200 \
  --json number,title,url,createdAt,labels 2>/dev/null || echo '[]')"

# Compose the digest body (Discord markdown).
body="$(
  WIN_START="$WIN_START" WIN_END="$WIN_END" REPO_SLUG="$repo_slug" \
  PRS_JSON="$prs_json" CLOSED_JSON="$closed_json" OPENED_JSON="$opened_json" \
  python3 - <<'PY'
import json
import os

start = os.environ["WIN_START"]
end = os.environ["WIN_END"]

def load(name):
    try:
        return json.loads(os.environ.get(name, "[]") or "[]")
    except json.JSONDecodeError:
        return []

def in_window(ts):
    # ISO 'Z' timestamps compare lexicographically within the same format.
    return bool(ts) and start <= ts < end

def labels_of(issue):
    return {l.get("name", "") for l in issue.get("labels", [])}

prs = [p for p in load("PRS_JSON") if in_window(p.get("mergedAt"))]
closed = [i for i in load("CLOSED_JSON") if in_window(i.get("closedAt"))]
opened = [i for i in load("OPENED_JSON") if in_window(i.get("createdAt"))]

# Sort each section by issue/PR number for a stable, readable order.
prs.sort(key=lambda x: x["number"])
closed.sort(key=lambda x: x["number"])
opened.sort(key=lambda x: x["number"])

# Keep the whole body comfortably under the Discord 4096-char embed-description
# limit (post-update.sh also caps at 4000) so it is never truncated mid-line.
# Budget-aware assembly: fill each section greedily and, when the budget runs
# out, close the section cleanly with "…and N more" rather than getting chopped.
MAX_BODY = 3800
MAX_PER_SECTION = 20

def line(item):
    icon = "📋 " if "epic" in labels_of(item) else ""
    num = item["number"]
    url = item["url"]
    title = item.get("title", "").strip()
    return f"• {icon}[#{num}]({url}) {title}"

sections = [
    ("Merged into `dev`", prs),
    ("Stories closed", closed),
    ("Opened", opened),
]

blocks = []
used = 0  # running character total across ALL sections
for heading, items in sections:
    if not items:
        continue
    header = f"**{heading} ({len(items)})**"
    sep = 2 if blocks else 0  # the "\n\n" joining this block to the previous
    if used + sep + len(header) > MAX_BODY:
        break  # no room even for this section header
    lines = [header]
    used += sep + len(header)
    shown = 0
    for it in items[:MAX_PER_SECTION]:
        ln = line(it)
        # Reserve room for a possible "…and N more" trailer (~24 chars).
        if used + 1 + len(ln) + 24 > MAX_BODY:
            break
        lines.append(ln)
        used += 1 + len(ln)
        shown += 1
    remaining = len(items) - shown
    if remaining > 0:
        trailer = f"• …and {remaining} more"
        lines.append(trailer)
        used += 1 + len(trailer)
    blocks.append("\n".join(lines))

if blocks:
    body = "\n\n".join(blocks)
else:
    body = "_Quiet day — no PRs merged into `dev` and no story activity._"

print(body)
PY
)"

# Hand off to post-update.sh, which owns webhook resolution + the POST.
post_args=(digest "$DAY_LABEL" "https://github.com/${repo_slug}/pulls?q=is%3Apr+base%3Adev+is%3Amerged" --body "$body")
[ "$dry_run" -eq 1 ] && post_args+=(--dry-run)

exec "${SCRIPT_DIR}/post-update.sh" "${post_args[@]}"
