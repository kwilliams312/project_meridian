# Discord Development Updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the orchestration lead a one-command way to post delegation-loop milestones (epic/story lifecycle, PR merged into dev) to a Discord channel via an incoming webhook.

**Architecture:** A single self-contained bash script `scripts/dev/post-update.sh` builds a Discord embed payload (JSON encoded with `python3` for correctness) and `curl`s it to a webhook whose URL comes from an env var or a gitignored file. The script no-ops (exit 0) when no webhook is configured. `CLAUDE.md`/`AGENTS.md` wire the script into the loop and reserve posting to the lead.

**Tech Stack:** bash (target macOS bash 3.2 + Linux), python3 (JSON encoding — already a repo dependency), curl, pytest (existing test tooling).

## Global Constraints

- **Source is the orchestration lead only** — not CI/CD, not sub-agents. Sub-agents never post.
- **Delivery is a one-way Discord incoming webhook** — no bot, no gateway.
- **Webhook URL is never committed.** Resolve from `$MERIDIAN_DISCORD_WEBHOOK_URL`, else a gitignored `.discord-webhook` at repo root (override path via `$MERIDIAN_DISCORD_WEBHOOK_FILE`), else skip.
- **A missing webhook must never break the loop** — script exits 0 with a stderr skip notice.
- **Script style:** `#!/usr/bin/env bash`, `# SPDX-License-Identifier: Apache-2.0` second line, header comment block, `set -euo pipefail`. Must run under macOS bash 3.2 (no `mapfile`, no associative arrays; guard empty-array expansions).
- **Events + embed mapping (verbatim):**
  | event | emoji | title prefix | color (decimal) |
  |-------|-------|--------------|-----------------|
  | `epic-open` | 📋 | Epic opened | 3447003 |
  | `story-dispatch` | 🚀 | Story dispatched | 10181046 |
  | `story-close` | ✅ | Story closed | 3066993 |
  | `pr-merged` | 🔀 | PR merged into dev | 5763719 |
  | `note` | 📝 | Note | 9807270 |
- Embed: `username="Meridian Dev"`, `footer.text="Project Meridian"`, `timestamp=date -u +%Y-%m-%dT%H:%M:%SZ`, title = `<emoji> <prefix> — <message>`, `url` present only when the `[url]` arg is given.

## File Structure

- `scripts/dev/post-update.sh` — the posting script (create). Sole responsibility: turn `<event> <message> [url]` into a Discord POST (or dry-run payload).
- `.gitignore` — add `.discord-webhook` (modify).
- `tests/test_post_update.py` — pytest shelling out to the script in `--dry-run` and skip modes (create).
- `CLAUDE.md` — add "Development updates → Discord" subsection + inline loop calls (modify).
- `AGENTS.md` — one line reserving posting to the lead (modify).

---

### Task 1: Posting script — core (arg parsing, event→embed mapping, dry-run payload)

**Files:**
- Create: `scripts/dev/post-update.sh`
- Modify: `.gitignore` (add `.discord-webhook`)
- Test: `tests/test_post_update.py`

**Interfaces:**
- Consumes: nothing (leaf script).
- Produces: CLI `scripts/dev/post-update.sh <event> "<message>" [url]` with flags `--dry-run`, `--help`. In `--dry-run` mode prints the Discord payload JSON to stdout and exits 0. Unknown event or fewer than 2 positional args → exit 2 with stderr message.

- [ ] **Step 1: Write the failing test**

Create `tests/test_post_update.py`:

```python
# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/dev/post-update.sh (Discord development updates)."""
import json
import os
import pathlib
import subprocess

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "dev" / "post-update.sh"


def run(*args, env=None):
    """Invoke the script with a clean env (no inherited webhook)."""
    e = dict(os.environ)
    e.pop("MERIDIAN_DISCORD_WEBHOOK_URL", None)
    if env:
        e.update(env)
    return subprocess.run(
        ["bash", str(SCRIPT), *args],
        capture_output=True,
        text=True,
        env=e,
    )


@pytest.mark.unit
@pytest.mark.parametrize(
    "event,needle,color",
    [
        ("epic-open", "Epic opened", 3447003),
        ("story-dispatch", "Story dispatched", 10181046),
        ("story-close", "Story closed", 3066993),
        ("pr-merged", "PR merged into dev", 5763719),
        ("note", "Note", 9807270),
    ],
)
def test_dry_run_payload_per_event(event, needle, color):
    r = run("--dry-run", event, "hello world")
    assert r.returncode == 0, r.stderr
    payload = json.loads(r.stdout)
    assert payload["username"] == "Meridian Dev"
    embed = payload["embeds"][0]
    assert needle in embed["title"]
    assert "hello world" in embed["title"]
    assert embed["color"] == color
    assert embed["footer"]["text"] == "Project Meridian"
    assert "url" not in embed


@pytest.mark.unit
def test_dry_run_includes_url_when_passed():
    r = run("--dry-run", "pr-merged", "story-1 thing", "https://example.com/pr/1")
    assert r.returncode == 0, r.stderr
    embed = json.loads(r.stdout)["embeds"][0]
    assert embed["url"] == "https://example.com/pr/1"


@pytest.mark.unit
def test_message_with_quotes_is_valid_json():
    r = run("--dry-run", "note", 'has "quotes" and \\ backslash')
    assert r.returncode == 0, r.stderr
    embed = json.loads(r.stdout)["embeds"][0]
    assert 'has "quotes" and \\ backslash' in embed["title"]


@pytest.mark.unit
def test_unknown_event_exits_2():
    r = run("--dry-run", "bogus", "msg")
    assert r.returncode == 2
    assert "unknown event" in r.stderr


@pytest.mark.unit
def test_missing_args_exits_2():
    r = run("--dry-run", "note")
    assert r.returncode == 2
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd <repo> && uv run pytest tests/test_post_update.py -q`
Expected: FAIL — script does not exist yet (non-zero return / FileNotFoundError surfaced as failing asserts).

- [ ] **Step 3: Create the script**

Create `scripts/dev/post-update.sh`:

```bash
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
```

- [ ] **Step 4: Make it executable**

Run: `chmod +x scripts/dev/post-update.sh`

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd <repo> && uv run pytest tests/test_post_update.py -q`
Expected: PASS (all Task 1 tests green).

- [ ] **Step 6: Add the secret file to .gitignore**

Add this line to `.gitignore` (grouped near other local-secret/dev entries):

```
# Discord webhook URL for scripts/dev/post-update.sh (never commit)
.discord-webhook
```

Verify: `git check-ignore .discord-webhook` prints `.discord-webhook`.

- [ ] **Step 7: Commit**

```bash
git add scripts/dev/post-update.sh tests/test_post_update.py .gitignore
git commit -m "feat(dev): post-update.sh — Discord dev-update posting (core + dry-run)"
```

---

### Task 2: Webhook resolution + delivery (skip-when-unset, POST)

The delivery code was written in Task 1 (it is one file). This task adds the regression test proving the non-fatal skip path and runtime-verifies delivery.

**Files:**
- Modify: `scripts/dev/post-update.sh` (only if Step 2 reveals a gap)
- Test: `tests/test_post_update.py`

**Interfaces:**
- Consumes: the script from Task 1.
- Produces: guaranteed behavior — with no `$MERIDIAN_DISCORD_WEBHOOK_URL` and a non-existent `$MERIDIAN_DISCORD_WEBHOOK_FILE`, the script exits 0, prints `skipping Discord post` to stderr, and makes no network call.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_post_update.py`:

```python
@pytest.mark.unit
def test_skips_and_exits_0_when_no_webhook(tmp_path):
    # No env URL, and point the webhook file at a nonexistent path so any
    # developer's real .discord-webhook cannot interfere -> skip, no network.
    r = run(
        "note",
        "no webhook configured here",
        env={"MERIDIAN_DISCORD_WEBHOOK_FILE": str(tmp_path / "nope")},
    )
    assert r.returncode == 0
    assert "skipping Discord post" in r.stderr


@pytest.mark.unit
def test_empty_webhook_file_skips(tmp_path):
    # A file that is only comments/blank lines resolves to no URL -> skip.
    wf = tmp_path / "webhook"
    wf.write_text("# just a comment\n\n")
    r = run(
        "note",
        "still no url",
        env={"MERIDIAN_DISCORD_WEBHOOK_FILE": str(wf)},
    )
    assert r.returncode == 0
    assert "skipping Discord post" in r.stderr
```

- [ ] **Step 2: Run tests to verify pass (implementation already present)**

Run: `cd <repo> && uv run pytest tests/test_post_update.py -q`
Expected: PASS. If either test fails, fix the webhook-resolution block in `scripts/dev/post-update.sh` (the `grep -vE ... | head -n1 | tr -d ... || true` line and the `-z "$webhook"` skip guard) until green.

- [ ] **Step 3: Runtime-verify the dry-run payload by hand**

Run: `scripts/dev/post-update.sh --dry-run pr-merged "story-639 — char-select redesign" https://example.com/pr/645`
Expected stdout: single-line JSON with `"title": "🔀 PR merged into dev — story-639 — char-select redesign"`, `"color": 5763719`, and `"url": "https://example.com/pr/645"`. Confirm it parses: pipe to `python3 -m json.tool`.

- [ ] **Step 4: Runtime-verify the skip path by hand**

Run: `env -u MERIDIAN_DISCORD_WEBHOOK_URL MERIDIAN_DISCORD_WEBHOOK_FILE=/nonexistent scripts/dev/post-update.sh note "hello"; echo "exit=$?"`
Expected: stderr `post-update: skipping Discord post (no webhook configured)` and `exit=0`.

- [ ] **Step 5: (Optional, live) verify a real POST**

Only if a test webhook URL is available (ask the human — do not fabricate one):
`MERIDIAN_DISCORD_WEBHOOK_URL="<url>" scripts/dev/post-update.sh note "post-update.sh live smoke test"`
Expected: stdout `post-update: posted (204) 📝 Note — post-update.sh live smoke test` and the message visible in the channel. (Discord webhooks return HTTP 204 on success.)

- [ ] **Step 6: shellcheck (if available)**

Run: `command -v shellcheck >/dev/null && shellcheck scripts/dev/post-update.sh || echo "shellcheck not installed — skipped"`
Expected: no warnings (or the skip notice).

- [ ] **Step 7: Commit**

```bash
git add tests/test_post_update.py
git commit -m "test(dev): post-update.sh skips non-fatally when no webhook configured"
```

---

### Task 3: Wire the script into the delegation-loop docs

**Files:**
- Modify: `CLAUDE.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: `scripts/dev/post-update.sh` from Task 1.
- Produces: documented lead behavior — the lead posts at epic-open, story-dispatch, pr-merged, story-close; sub-agents are told not to post.

- [ ] **Step 1: Add the Discord subsection to CLAUDE.md**

In `CLAUDE.md`, immediately after the `### Sub-agent isolation` block (which ends with `See `AGENTS.md` for the rules sub-agents must follow.`), insert:

```markdown
### Development updates → Discord

The lead announces delegation-loop milestones to a Discord channel via
`scripts/dev/post-update.sh` (one-way incoming webhook — no bot). **Only the lead
posts; sub-agents never do.** Post at these moments in the loop:

| Moment | Command |
|--------|---------|
| Epic opened (step 1) | `scripts/dev/post-update.sh epic-open "<epic title>" <epic-url>` |
| Story dispatched (step 2) | `scripts/dev/post-update.sh story-dispatch "<story title>" <story-url>` |
| PR merged into dev (step 6) | `scripts/dev/post-update.sh pr-merged "<story> — <summary>" <pr-url>` |
| Story closed on merge (step 7) | `scripts/dev/post-update.sh story-close "<story title>" <story-url>` |

Configure the webhook once via `$MERIDIAN_DISCORD_WEBHOOK_URL` or a gitignored
`.discord-webhook` file at the repo root. If neither is set the script prints a
skip notice and exits 0 — a missing webhook never blocks the loop. Use the `note`
event for any other update worth surfacing.
```

- [ ] **Step 2: Add inline post cues to the loop steps in CLAUDE.md**

Make these four in-place edits to the numbered delegation loop in `CLAUDE.md`:

1. End of step 1 (`...Group stories under an epic and keep its checklist current.`) → append:
   ` Post an `epic-open` update to Discord (see below).`
2. End of step 2 (`...so parallel tasks never corrupt each other's tree.`) → append:
   ` As you dispatch, post a `story-dispatch` update.`
3. Step 6, in the `approve and merge into `dev`` clause → append after it:
   ` (then post a `pr-merged` update)`.
4. End of step 7 (`...ticks the parent epic's checklist.`) → append:
   ` and posts a `story-close` update.`

- [ ] **Step 3: Add the one-line rule to AGENTS.md**

In `AGENTS.md`, under the `## Task & PR rules` list, add a bullet:

```markdown
- **Do not post development updates to Discord.** Announcing delegation-loop
  progress is the orchestration lead's job (`scripts/dev/post-update.sh`);
  sub-agents never post.
```

- [ ] **Step 4: Verify the doc edits**

Run: `grep -n "post-update.sh" CLAUDE.md AGENTS.md`
Expected: the subsection table rows + the AGENTS.md bullet appear; the four inline cues are present in steps 1, 2, 6, 7.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md AGENTS.md
git commit -m "docs: wire post-update.sh into the delegation loop; reserve posting to the lead"
```

---

### Task 4: Full-suite verification

**Files:** none (verification only).

- [ ] **Step 1: Run the full test suite**

Run: `cd <repo> && uv run pytest -q`
Expected: 0 failures (the new `tests/test_post_update.py` included). If any pre-existing failure appears, fix it before considering the work done (repo zero-tolerance rule).

- [ ] **Step 2: Confirm the secret is ignored and absent**

Run: `git status --porcelain | grep -i discord-webhook || echo "no .discord-webhook tracked ✓"` and `git check-ignore .discord-webhook`
Expected: `.discord-webhook` is ignored and never staged.

---

## Self-Review

**Spec coverage:**
- Posting script (`scripts/dev/post-update.sh`) → Task 1. ✓
- Secret handling (env var + gitignored file, override path, skip-when-unset exit 0) → Task 1 (resolution + `.gitignore`) + Task 2 (skip regression tests). ✓
- Message format (embed, per-event emoji/title/color, timestamp, optional url, username/footer) → Task 1 (`Global Constraints` table + script + parametrized tests). ✓
- Trigger points (epic-open, story-dispatch, pr-merged, story-close) → Task 3 doc wiring. ✓ (Source = lead only; CI/CD out of scope — not built.)
- Sub-agents never post → Task 3 AGENTS.md bullet. ✓
- Testing (dry-run structural, url present/absent, unknown event, skip-no-network) → Tasks 1–2. ✓
- Error handling (missing args exit 2, unknown event exit 2, non-2xx exit 1, skip exit 0) → Task 1 script + tests. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code. Task 2 has no new production code (delivery is in the one-file script from Task 1) and says so explicitly rather than leaving a stub.

**Type consistency:** Event names, colors, and title prefixes match across the Global Constraints table, the script `case`, and the parametrized test — verified identical (`epic-open`/3447003, `story-dispatch`/10181046, `story-close`/3066993, `pr-merged`/5763719, `note`/9807270).
