"""Contract tests for the versioned Meshy JSON-lines job protocol (#679).

Every HTTP interaction is backed by ``httpx.MockTransport``.  These tests must
never contact Meshy (or any other external service).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import httpx
import pytest
from jsonschema import Draft202012Validator

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import meshy.__main__ as meshy_main  # noqa: E402
import meshy.client as client_mod  # noqa: E402
import meshy.protocol as protocol  # noqa: E402
from test_meshy import _make_glb, _text_to_3d_handler  # noqa: E402


def _events(stdout: str) -> list[dict]:
    events = [json.loads(line) for line in stdout.splitlines() if line]
    validator = Draft202012Validator(
        json.loads(protocol.SCHEMA_PATH.read_text(encoding="utf-8"))
    )
    assert all(not list(validator.iter_errors(event)) for event in events)
    return events


def _base_args(tmp_path: Path) -> list[str]:
    return [
        "generate",
        "--text",
        "an original small stone marker",
        "--ns",
        "core",
        "--name",
        "protocol_marker",
        "--class",
        "prop",
        "--terms-verified",
        "--model-version",
        "meshy-5",
        "--content-root",
        str(tmp_path / "content"),
        "--poll-interval",
        "0",
        "--poll-timeout",
        "0",
        "--json-events",
    ]


def test_protocol_schema_accepts_every_documented_event_shape():
    schema = json.loads(protocol.SCHEMA_PATH.read_text(encoding="utf-8"))
    validator = Draft202012Validator(schema)
    emitter = protocol.EventEmitter(secret="super-secret")

    for event_name in protocol.EVENT_NAMES:
        event = emitter.build(
            event_name,
            message="safe",
            mode="text",
            model_version="meshy-5",
            task_id="task-1",
            task_ids=["task-1"],
            stage="preview",
            status="IN_PROGRESS",
            progress=50,
            path="content/core/assets/art/example",
            asset_id="core:art.example",
            lod0_tris=100,
            error_code="provider_error",
        )
        assert list(validator.iter_errors(event)) == []


def test_emitter_redacts_api_key_and_bearer_tokens_recursively(capsys):
    emitter = protocol.EventEmitter(secret="super-secret", stream=sys.stdout)
    emitter.emit(
        "error",
        message="provider echoed super-secret and Bearer another-token",
        details={"nested": ["super-secret", "Authorization: Bearer third-token"]},
    )
    raw = capsys.readouterr().out
    assert "super-secret" not in raw
    assert "another-token" not in raw
    assert "third-token" not in raw
    assert raw.count("[REDACTED]") >= 3


def test_json_protocol_happy_path_emits_complete_ordered_job(
    tmp_path, monkeypatch, capsys
):
    glb = _make_glb(tmp_path / "source.glb", triangle_count=2)
    handler, _calls = _text_to_3d_handler(glb_bytes=glb.read_bytes())
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(
        meshy_main,
        "_new_client",
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: client_mod.MeshyClient(
            api_key, model_version=model_version, transport=transport
        ),
    )
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    rc = meshy_main.main(_base_args(tmp_path))

    captured = capsys.readouterr()
    assert rc == 0
    assert captured.err == ""
    events = _events(captured.out)
    validator = Draft202012Validator(
        json.loads(protocol.SCHEMA_PATH.read_text(encoding="utf-8"))
    )
    assert all(not list(validator.iter_errors(event)) for event in events)
    assert [event["sequence"] for event in events] == list(range(1, len(events) + 1))
    names = [event["event"] for event in events]
    assert names == [
        "validation.started",
        "validation.passed",
        "preview.submitted",
        "poll.progress",
        "refine.submitted",
        "poll.progress",
        "download.started",
        "download.completed",
        "budget.started",
        "budget.passed",
        "provenance.started",
        "provenance.written",
        "completed",
    ]
    assert events[-1]["task_ids"] == ["task_preview_1", "task_refine_1"]
    asset_dir = tmp_path / "content/core/assets/art/protocol_marker"
    assert (asset_dir / "sm_protocol_marker.glb").exists()
    assert (asset_dir / "protocol_marker.asset.yaml").exists()
    assert (asset_dir / "protocol_marker.prompts.yaml").exists()


@pytest.mark.parametrize("status_code", [402, 429])
def test_json_protocol_classifies_provider_http_errors_and_redacts_key(
    status_code, tmp_path, monkeypatch, capsys
):
    secret = "provider-secret-value"

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(status_code, text=f"echoed {secret}")

    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(
        meshy_main,
        "_new_client",
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: client_mod.MeshyClient(
            api_key, model_version=model_version, transport=transport
        ),
    )
    monkeypatch.setenv("MESHY_API_KEY", secret)

    rc = meshy_main.main(_base_args(tmp_path))

    captured = capsys.readouterr()
    assert rc == 1
    assert captured.err == ""
    assert secret not in captured.out
    final = _events(captured.out)[-1]
    assert final["event"] == "error"
    assert final["error_code"] == ("payment_required" if status_code == 402 else "rate_limited")
    assert final["http_status"] == status_code


@pytest.mark.parametrize("model_version", ["latest", "LATEST", " latest ", ""])
def test_json_protocol_refuses_unpinned_model_before_http(
    model_version, tmp_path, monkeypatch, capsys
):
    def forbidden(*args, **kwargs):
        raise AssertionError("latest must be refused before client construction")

    monkeypatch.setattr(meshy_main, "_new_client", forbidden)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    args = _base_args(tmp_path)
    args[args.index("meshy-5")] = model_version

    rc = meshy_main.main(args)

    assert rc == 2
    final = _events(capsys.readouterr().out)[-1]
    assert final["event"] == "error"
    assert final["error_code"] == "invalid_request"
    assert "pinned" in final["message"]


def test_json_protocol_wraps_argument_parser_errors(capsys):
    rc = meshy_main.main(["generate", "--json-events"])

    captured = capsys.readouterr()
    assert rc == 2
    assert captured.err == ""
    final = _events(captured.out)[-1]
    assert final["event"] == "error"
    assert final["error_code"] == "invalid_request"
    assert "required" in final["message"]


def test_json_protocol_reports_status_drift(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        if request.method == "POST":
            return httpx.Response(200, json={"result": "preview-1"})
        return httpx.Response(200, json={"status": "NEW_PROVIDER_STATUS", "progress": 7})

    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(
        meshy_main,
        "_new_client",
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: client_mod.MeshyClient(
            api_key, model_version=model_version, transport=transport
        ),
    )
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    assert meshy_main.main(_base_args(tmp_path)) == 1
    events = _events(capsys.readouterr().out)
    assert any(e["event"] == "preview.submitted" and e["task_id"] == "preview-1" for e in events)
    assert events[-1]["error_code"] == "status_drift"
    assert events[-1]["task_ids"] == ["preview-1"]


def test_json_protocol_reports_timeout_with_task_id(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        if request.method == "POST":
            return httpx.Response(200, json={"result": "preview-timeout"})
        return httpx.Response(200, json={"status": "IN_PROGRESS", "progress": 12})

    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(
        meshy_main,
        "_new_client",
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: client_mod.MeshyClient(
            api_key, model_version=model_version, transport=transport
        ),
    )
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    assert meshy_main.main(_base_args(tmp_path)) == 1
    events = _events(capsys.readouterr().out)
    assert any(e["event"] == "poll.progress" and e["progress"] == 12 for e in events)
    assert events[-1]["error_code"] == "timeout"
    assert events[-1]["task_ids"] == ["preview-timeout"]


def test_keyboard_interrupt_emits_cancel_and_removes_partial_output(
    tmp_path, monkeypatch, capsys
):
    class InterruptingClient:
        model_version = "meshy-5"

        def text_to_3d(self, prompt, **kwargs):
            kwargs["on_event"](
                "preview.submitted", task_id="preview-cancel", stage="preview"
            )
            partial.parent.mkdir(parents=True, exist_ok=True)
            partial.write_bytes(b"not shippable")
            raise KeyboardInterrupt

        def close(self):
            pass

    content_root = tmp_path / "content"
    partial = (
        content_root
        / "core/assets/art/protocol_marker/sm_protocol_marker.partial.glb"
    )
    monkeypatch.setattr(meshy_main, "_new_client", lambda *args, **kwargs: InterruptingClient())
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    rc = meshy_main.main(_base_args(tmp_path))

    assert rc == 130
    events = _events(capsys.readouterr().out)
    assert events[-1]["event"] == "cancelled"
    assert events[-1]["task_ids"] == ["preview-cancel"]
    assert not partial.exists()
    assert not (partial.parent / "sm_protocol_marker.glb").exists()
    assert not (partial.parent / "protocol_marker.asset.yaml").exists()
    assert not (partial.parent / "protocol_marker.prompts.yaml").exists()
