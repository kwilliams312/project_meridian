"""Contract tests for the versioned Meshy JSON-lines job protocol (#679).

Every HTTP interaction is backed by ``httpx.MockTransport``.  These tests must
never contact Meshy (or any other external service).
"""

from __future__ import annotations

import json
import sys
import threading
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
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: (
            client_mod.MeshyClient(
                api_key, model_version=model_version, transport=transport
            )
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
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: (
            client_mod.MeshyClient(
                api_key, model_version=model_version, transport=transport
            )
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
    assert final["error_code"] == (
        "payment_required" if status_code == 402 else "rate_limited"
    )
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
        return httpx.Response(
            200, json={"status": "NEW_PROVIDER_STATUS", "progress": 7}
        )

    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(
        meshy_main,
        "_new_client",
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: (
            client_mod.MeshyClient(
                api_key, model_version=model_version, transport=transport
            )
        ),
    )
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    assert meshy_main.main(_base_args(tmp_path)) == 1
    events = _events(capsys.readouterr().out)
    assert any(
        e["event"] == "preview.submitted" and e["task_id"] == "preview-1"
        for e in events
    )
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
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: (
            client_mod.MeshyClient(
                api_key, model_version=model_version, transport=transport
            )
        ),
    )
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    assert meshy_main.main(_base_args(tmp_path)) == 1
    events = _events(capsys.readouterr().out)
    assert any(e["event"] == "poll.progress" and e["progress"] == 12 for e in events)
    assert events[-1]["error_code"] == "timeout"
    assert events[-1]["task_ids"] == ["preview-timeout"]


def test_json_protocol_reports_client_tls_setup_failure_and_redacts_secret(
    tmp_path, monkeypatch, capsys
):
    secret = "tls-setup-secret"
    missing_bundle = tmp_path / f"missing-{secret}.pem"
    monkeypatch.setenv("MESHY_API_KEY", secret)
    monkeypatch.setenv("SSL_CERT_FILE", str(missing_bundle))

    def construct_with_diagnostic(api_key, model_version):
        try:
            return client_mod.MeshyClient(api_key, model_version=model_version)
        except OSError as exc:
            # Model a setup layer adding environment diagnostics.  The protocol
            # must redact both the key and the key-bearing certificate path.
            raise OSError(f"{exc}; cert={missing_bundle}; key={api_key}") from exc

    monkeypatch.setattr(meshy_main, "_new_client", construct_with_diagnostic)

    rc = meshy_main.main(_base_args(tmp_path))

    captured = capsys.readouterr()
    assert rc == 1
    assert captured.err == ""
    assert secret not in captured.out
    final = _events(captured.out)[-1]
    assert final["event"] == "error"
    assert final["error_code"] == "internal_error"
    assert "missing-[REDACTED].pem" in final["message"]
    assert "key=[REDACTED]" in final["message"]
    assert not (tmp_path / "content/core/assets/art/protocol_marker").exists()


def test_same_target_concurrent_jobs_reserve_atomically_and_preserve_winner(
    tmp_path, monkeypatch
):
    """The losing job cannot enter setup or clean the winner's final output."""

    glb = _make_glb(tmp_path / "source.glb", triangle_count=2)
    handler, _calls = _text_to_3d_handler(glb_bytes=glb.read_bytes())
    transport = httpx.MockTransport(handler)
    owner_reserved = threading.Event()
    release_owner = threading.Event()
    client_constructions = 0
    construction_lock = threading.Lock()

    def blocking_client(api_key, model_version=client_mod.DEFAULT_MODEL_VERSION):
        nonlocal client_constructions
        with construction_lock:
            client_constructions += 1
        owner_reserved.set()
        assert release_owner.wait(timeout=5)
        return client_mod.MeshyClient(
            api_key, model_version=model_version, transport=transport
        )

    monkeypatch.setattr(meshy_main, "_new_client", blocking_client)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    args = _base_args(tmp_path)
    args.remove("--json-events")
    results: dict[str, int] = {}

    owner = threading.Thread(
        target=lambda: results.setdefault("owner", meshy_main.main(args)),
        name="meshy-owner",
    )
    owner.start()
    assert owner_reserved.wait(timeout=5)

    loser = threading.Thread(
        target=lambda: results.setdefault("loser", meshy_main.main(args)),
        name="meshy-loser",
    )
    loser.start()
    loser.join(timeout=5)
    assert not loser.is_alive()
    assert results["loser"] == 2

    release_owner.set()
    owner.join(timeout=5)
    assert not owner.is_alive()
    assert results["owner"] == 0
    assert client_constructions == 1

    asset_dir = tmp_path / "content/core/assets/art/protocol_marker"
    final_glb = asset_dir / "sm_protocol_marker.glb"
    landed = final_glb.read_bytes()
    assert landed == glb.read_bytes()
    assert (asset_dir / "protocol_marker.asset.yaml").exists()
    assert (asset_dir / "protocol_marker.prompts.yaml").exists()
    assert not list(asset_dir.glob("*.partial*"))

    # A later failed/cancelled same-target attempt is refused before client
    # construction and cannot delete or replace the completed asset.
    assert meshy_main.main(args) == 2
    assert final_glb.read_bytes() == landed


def test_keyboard_interrupt_emits_cancel_and_removes_partial_output(
    tmp_path, monkeypatch, capsys
):
    glb = _make_glb(tmp_path / "source.glb", triangle_count=2)
    handler, _calls = _text_to_3d_handler(glb_bytes=glb.read_bytes())
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(
        meshy_main,
        "_new_client",
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION: (
            client_mod.MeshyClient(
                api_key, model_version=model_version, transport=transport
            )
        ),
    )
    monkeypatch.setattr(
        meshy_main.intake,
        "count_glb_triangles",
        lambda _path: (_ for _ in ()).throw(KeyboardInterrupt),
    )
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    rc = meshy_main.main(_base_args(tmp_path))

    assert rc == 130
    events = _events(capsys.readouterr().out)
    assert events[-1]["event"] == "cancelled"
    assert events[-1]["task_ids"] == ["task_preview_1", "task_refine_1"]
    asset_dir = tmp_path / "content/core/assets/art/protocol_marker"
    assert not asset_dir.exists()
