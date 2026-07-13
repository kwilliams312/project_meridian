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
