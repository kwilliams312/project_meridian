# Discord Development Updates — Design

**Date:** 2026-07-13
**Status:** Approved (design)
**Author:** Orchestration agent + kwillia

## Problem

The orchestration agent (the technical lead driving a session, per `CLAUDE.md`) runs
a delegation loop: decompose work into stories, dispatch sub-agents, QA, review, and
merge into `dev`. There is currently no outward-facing signal of this progress — a
human watching the project has to read GitHub or the session transcript to know what
is happening. We want the lead to post concise development updates to a Discord
channel as it drives the loop.

## Scope

**In scope**

- A single posting script the lead calls to send an update to Discord.
- Updates sourced **only** by the orchestration agent (not CI/CD, not sub-agents).
- One-way delivery via a Discord **incoming webhook**.
- Updates at these delegation-loop moments:
  - epic opened / story dispatched to a sub-agent,
  - PR merged into `dev`,
  - story closed on merge.
- Documentation wiring the script into `CLAUDE.md` and `AGENTS.md`.

**Out of scope (YAGNI)**

- CI/CD-sourced notifications (build pass/fail, deploy, promotion). The script is
  reusable by a workflow later, but no workflow wiring is built now.
- A Discord bot / gateway connection / two-way interaction. A webhook is sufficient
  for one-way notifications.
- Sub-agents posting their own updates (avoids N-way channel noise from parallel
  sub-agents). Only the lead posts.

## Approach

A single bash script, `scripts/dev/post-update.sh`, following the existing
`scripts/dev/` convention. It builds a Discord embed payload and `curl`s it to the
webhook. No hosted process, works locally and (later) from CI unchanged.

Rejected alternatives:

- **Python module + CLI** — more testable but adds import/dependency weight for what
  is fundamentally one `curl`.
- **GitHub Actions webhook steps** — CI/CD was explicitly scoped out as a source.

## Components

### 1. Posting script — `scripts/dev/post-update.sh`

```
scripts/dev/post-update.sh <event> "<message>" [url]

  events:  epic-open | story-dispatch | story-close | pr-merged | note
  flags:   --dry-run   print the JSON payload to stdout instead of POSTing
           --help      usage
```

- `<message>` becomes the embed description.
- `[url]` (optional) makes the embed title a clickable link to the issue/PR.
- `note` is a generic freeform update for anything not covered by the named events.
- Unknown event → usage error, exit 2.

### 2. Secret handling

The webhook URL is resolved in this order:

1. `$MERIDIAN_DISCORD_WEBHOOK_URL`
2. else a gitignored file `.discord-webhook` at the repo root — first non-empty,
   non-`#` line is taken as the URL.

If neither is set, the script prints
`skipping Discord post (no webhook configured)` to stderr and **exits 0**. A missing
webhook must never break the delegation loop or a local run.

`.discord-webhook` is added to `.gitignore`. The URL is never committed.

### 3. Message format

A single Discord embed:

```json
{
  "username": "Meridian Dev",
  "embeds": [{
    "title": "🔀 PR merged into dev — story-639 char-select redesign",
    "url": "https://github.com/.../pull/645",
    "color": 5763719,
    "footer": { "text": "Project Meridian" },
    "timestamp": "2026-07-13T18:20:00Z"
  }]
}
```

- `url` is omitted from the payload when no `[url]` argument is given.
- `timestamp` is `date -u +%FT%TZ` at post time.

Event → emoji / title prefix / color:

| event          | emoji | title prefix          | color (decimal) |
|----------------|-------|-----------------------|-----------------|
| `epic-open`    | 📋    | Epic opened           | 3447003 (blue)  |
| `story-dispatch`| 🚀   | Story dispatched      | 10181046 (purple)|
| `story-close`  | ✅    | Story closed          | 3066993 (green) |
| `pr-merged`    | 🔀    | PR merged into dev    | 5763719 (teal)  |
| `note`         | 📝    | Note                  | 9807270 (grey)  |

Full title = `<emoji> <title prefix> — <message>`.

### 4. Documentation changes

- **`CLAUDE.md`** — a new short "Development updates → Discord" subsection under
  Orchestration describing the script, the `MERIDIAN_DISCORD_WEBHOOK_URL` env var /
  `.discord-webhook` file, and that only the lead posts. Inline `post-update.sh`
  calls are wired into the delegation loop at:
  - step 1–2: epic opened / story dispatched,
  - step 6: PR merged into `dev`,
  - step 7: story closed on merge.
- **`AGENTS.md`** — one line: sub-agents do **not** post development updates; posting
  is the lead's responsibility.

### 5. Testing

`tests/test_post_update.py` (repo already uses pytest), shelling out to the script:

- `--dry-run` for each event emits **valid JSON** with the expected title, color, and
  `username`/`footer`.
- `url` is present in the payload when passed, absent when not.
- With the webhook **unset** and no `--dry-run`, the script **exits 0** and makes no
  network call (assert by unsetting the env var and pointing `HOME`/cwd away from any
  `.discord-webhook`, and confirming no `curl` reaches the network — dry structural
  check, no live POST).
- Unknown event exits 2.

`shellcheck scripts/dev/post-update.sh` if `shellcheck` is available.

## Error handling

- Missing webhook → skip, exit 0 (non-fatal).
- Missing required args (`event`, `message`) → usage error, exit 2.
- Unknown event → usage error, exit 2.
- `curl` non-2xx from Discord → print the HTTP status and Discord response body to
  stderr, exit 1. The lead surfaces this but the loop is not required to abort.

## Data flow

```
lead (CLAUDE.md loop step)
  → scripts/dev/post-update.sh <event> "<msg>" [url]
      → resolve webhook URL (env | .discord-webhook | none→skip)
      → build embed JSON (event → emoji/title/color, timestamp)
      → curl POST to webhook   (or print payload if --dry-run)
  → Discord channel
```

## Repo hygiene

Per `CLAUDE.md`, day-to-day work lands on `dev`. Implementation of this feature will
be done on a `dev`-based branch; the exact branch/commit approach is confirmed with
the human before any git write commands are run (per the repo git rule: explicit
approval before add/commit/push).
