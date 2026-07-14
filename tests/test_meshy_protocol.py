"""Contract tests for the versioned Meshy JSON-lines job protocol (#679).

Every HTTP interaction is backed by ``httpx.MockTransport``.  These tests must
never contact Meshy (or any other external service).
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import threading
from pathlib import Path
from types import SimpleNamespace

import httpx
import pytest
from jsonschema import Draft202012Validator

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import meshy.__main__ as meshy_main  # noqa: E402
import meshy.client as client_mod  # noqa: E402
import meshy.locking as locking_mod  # noqa: E402
import meshy.protocol as protocol  # noqa: E402
from test_meshy import _make_glb, _text_to_3d_handler  # noqa: E402


@pytest.fixture(autouse=True)
def _isolated_lock_root(tmp_path, monkeypatch):
    monkeypatch.setenv("MERIDIAN_MESHY_LOCK_DIR", str(tmp_path / "job-locks"))


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


@pytest.mark.parametrize("platform", ["posix", "nt"])
def test_advisory_lock_uses_platform_backend_without_cross_platform_import(
    platform, tmp_path
):
    calls = []
    if platform == "nt":
        backend = SimpleNamespace(
            LK_NBLCK=1,
            LK_UNLCK=2,
            locking=lambda fd, mode, length: calls.append((fd, mode, length)),
        )
        lock = locking_mod.AdvisoryFileLock(
            tmp_path / "windows.lock", platform="nt", msvcrt_module=backend
        )
    else:
        backend = SimpleNamespace(
            LOCK_EX=1,
            LOCK_NB=2,
            LOCK_UN=4,
            flock=lambda fd, mode: calls.append((fd, mode)),
        )
        lock = locking_mod.AdvisoryFileLock(
            tmp_path / "posix.lock", platform="posix", fcntl_module=backend
        )

    lock.acquire()
    lock.release()

    if platform == "nt":
        assert [call[1:] for call in calls] == [
            (backend.LK_NBLCK, 1),
            (backend.LK_UNLCK, 1),
        ]
    else:
        assert [call[1] for call in calls] == [
            backend.LOCK_EX | backend.LOCK_NB,
            backend.LOCK_UN,
        ]


def test_meshy_and_convert_rig_import_when_fcntl_is_unavailable():
    script = """
import builtins
real_import = builtins.__import__
def without_fcntl(name, *args, **kwargs):
    if name == 'fcntl':
        raise ModuleNotFoundError('simulated Windows: no fcntl')
    return real_import(name, *args, **kwargs)
builtins.__import__ = without_fcntl
import meshy.__main__ as cli
args = cli._build_parser().parse_args(
    ['convert-rig', 'input.glb', '--meshy-version', 'meshy-5', '--out', 'out.glb']
)
assert args.command == 'convert-rig'
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=REPO,
        env={**os.environ, "PYTHONPATH": str(REPO / "tools")},
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr


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
    assert not list(asset_dir.parent.glob(".protocol_marker.*.partial"))
    assert not list(asset_dir.parent.glob(".protocol_marker.*.owner"))

    # A later same-target attempt is refused before client construction and
    # cannot delete or replace the completed asset.
    assert meshy_main.main(args) == 2
    assert final_glb.read_bytes() == landed


@pytest.mark.parametrize(
    ("injected", "expected_rc", "terminal"),
    [
        (RuntimeError("publish boundary failed"), 1, "error"),
        (KeyboardInterrupt(), 130, "cancelled"),
    ],
)
def test_atomic_publish_boundary_failure_rolls_back_and_allows_retry(
    injected, expected_rc, terminal, tmp_path, monkeypatch, capsys
):
    """The one directory-rename boundary is rollback-safe for errors and SIGINT."""

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
    original_publish = meshy_main._TargetReservation.publish

    def publish_then_fail(reservation):
        original_publish(reservation)
        raise injected

    monkeypatch.setattr(meshy_main._TargetReservation, "publish", publish_then_fail)

    assert meshy_main.main(_base_args(tmp_path)) == expected_rc
    first_events = _events(capsys.readouterr().out)
    assert first_events[-1]["event"] == terminal
    assert (
        sum(
            event["event"] in {"completed", "cancelled", "error"}
            for event in first_events
        )
        == 1
    )

    parent = tmp_path / "content/core/assets/art"
    assert not (parent / "protocol_marker").exists()
    assert not list(parent.glob(".protocol_marker.*.partial"))
    assert not list(parent.glob(".protocol_marker.*.owner"))
    assert list(parent.iterdir()) == []

    # The failed publication leaves no poisoned reservation or raw final file.
    monkeypatch.setattr(meshy_main._TargetReservation, "publish", original_publish)
    assert meshy_main.main(_base_args(tmp_path)) == 0
    assert (parent / "protocol_marker/sm_protocol_marker.glb").read_bytes() == (
        glb.read_bytes()
    )


@pytest.mark.parametrize("transition", ["commit-state", "sentinel", "terminal"])
def test_interrupt_after_commit_point_resolves_to_one_completed_event(
    transition, tmp_path, monkeypatch, capsys
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

    if transition == "commit-state":
        original_write = meshy_main._TargetReservation._write_state

        def interrupt_after_state(reservation, phase):
            original_write(reservation, phase)
            if phase == "committed":
                raise KeyboardInterrupt

        monkeypatch.setattr(
            meshy_main._TargetReservation, "_write_state", interrupt_after_state
        )
    elif transition == "sentinel":
        original_commit = meshy_main._TargetReservation.commit

        def interrupt_after_sentinel(reservation):
            original_commit(reservation)
            raise KeyboardInterrupt

        monkeypatch.setattr(
            meshy_main._TargetReservation, "commit", interrupt_after_sentinel
        )
    else:
        monkeypatch.setattr(
            meshy_main,
            "_post_commit_hook",
            lambda: (_ for _ in ()).throw(KeyboardInterrupt),
        )

    assert meshy_main.main(_base_args(tmp_path)) == 0
    events = _events(capsys.readouterr().out)
    terminals = [
        event
        for event in events
        if event["event"] in {"completed", "cancelled", "error"}
    ]
    assert [event["event"] for event in terminals] == ["completed"]
    final = tmp_path / "content/core/assets/art/protocol_marker"
    assert (final / "sm_protocol_marker.glb").read_bytes() == glb.read_bytes()
    assert not (final / ".meshy-ownership.json").exists()
    assert {entry.name for entry in final.parent.iterdir()} == {"protocol_marker"}
    assert {entry.name for entry in final.iterdir()} == {
        "sm_protocol_marker.glb",
        "protocol_marker.asset.yaml",
        "protocol_marker.prompts.yaml",
    }


@pytest.mark.parametrize("interruption", ["before-write", "after-write"])
def test_interrupt_during_completed_emission_is_recoverable_and_idempotent(
    interruption, tmp_path, monkeypatch, capsys
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
    original_emit = protocol.EventEmitter.emit
    interrupted = False

    def interrupt_once(emitter, event, **fields):
        nonlocal interrupted
        if event == "completed" and not interrupted:
            interrupted = True
            if interruption == "before-write":
                raise KeyboardInterrupt
            original_emit(emitter, event, **fields)
            raise KeyboardInterrupt
        return original_emit(emitter, event, **fields)

    monkeypatch.setattr(protocol.EventEmitter, "emit", interrupt_once)

    assert meshy_main.main(_base_args(tmp_path)) == 0
    events = _events(capsys.readouterr().out)
    terminals = [
        event
        for event in events
        if event["event"] in {"completed", "cancelled", "error"}
    ]
    assert [event["event"] for event in terminals] == ["completed"]
    assert (tmp_path / "content/core/assets/art/protocol_marker").is_dir()


def test_client_close_failure_emits_one_redacted_error_and_rolls_back(
    tmp_path, monkeypatch, capsys
):
    secret = "close-secret"
    glb = _make_glb(tmp_path / "source.glb", triangle_count=2)
    handler, _calls = _text_to_3d_handler(glb_bytes=glb.read_bytes())
    transport = httpx.MockTransport(handler)

    class CloseFailClient:
        def __init__(self):
            self.inner = client_mod.MeshyClient(
                secret, model_version="meshy-5", transport=transport
            )

        def __getattr__(self, name):
            return getattr(self.inner, name)

        def close(self):
            raise RuntimeError(f"teardown echoed {secret} and Bearer teardown-token")

    monkeypatch.setattr(meshy_main, "_new_client", lambda *_args: CloseFailClient())
    monkeypatch.setenv("MESHY_API_KEY", secret)

    assert meshy_main.main(_base_args(tmp_path)) == 1

    captured = capsys.readouterr()
    assert captured.err == ""
    assert secret not in captured.out
    assert "teardown-token" not in captured.out
    events = _events(captured.out)
    terminals = [
        event
        for event in events
        if event["event"] in {"completed", "cancelled", "error"}
    ]
    assert len(terminals) == 1
    assert terminals[0]["event"] == "error"
    assert terminals[0]["error_code"] == "internal_error"
    assert terminals[0]["message"] == ("teardown echoed [REDACTED] and [REDACTED]")
    parent = tmp_path / "content/core/assets/art"
    assert not (parent / "protocol_marker").exists()
    assert list(parent.iterdir()) == []


@pytest.mark.parametrize("phase", ["staging", "published"])
def test_dead_owner_artifacts_are_recovered_but_live_lock_is_not_stolen(
    phase, tmp_path, monkeypatch
):
    """An OS-released stale lock is recoverable in either side of publication."""

    parent = tmp_path / "content/core/assets/art"
    parent.mkdir(parents=True)
    token = "de" * 32
    staging = parent / f".protocol_marker.{token}.partial"
    final = parent / "protocol_marker"
    stale_dir = staging if phase == "staging" else final
    stale_dir.mkdir()
    (stale_dir / "raw-unpublished.glb").write_bytes(b"must not survive")
    identity = os.path.normcase(str(final.resolve(strict=False)))
    record = {
        "schema": meshy_main._TargetReservation._STATE_SCHEMA,
        "target": identity,
        "token": token,
        "phase": phase,
    }
    (stale_dir / ".meshy-ownership.json").write_text(
        json.dumps(record), encoding="utf-8"
    )
    lock_root = tmp_path / "job-locks"
    lock_root.mkdir()
    key = hashlib.sha256(identity.encode("utf-8")).hexdigest()
    (lock_root / f"{key}.json").write_text(json.dumps(record), encoding="utf-8")

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

    assert meshy_main.main(_base_args(tmp_path)) == 0
    assert not staging.exists()
    assert not (final / "raw-unpublished.glb").exists()
    assert (final / "sm_protocol_marker.glb").read_bytes() == glb.read_bytes()


def test_forged_owner_names_and_sentinel_never_authorize_final_deletion(
    tmp_path, monkeypatch, capsys
):
    parent = tmp_path / "content/core/assets/art"
    final = parent / "protocol_marker"
    final.mkdir(parents=True)
    valuable = final / "sm_protocol_marker.glb"
    valuable.write_bytes(b"legitimate completed asset")
    forged_token = "ab" * 32
    (parent / f".protocol_marker.{forged_token}.owner").write_text(
        forged_token, encoding="ascii"
    )
    (final / ".meshy-ownership.json").write_text(
        json.dumps(
            {
                "schema": meshy_main._TargetReservation._STATE_SCHEMA,
                "target": os.path.normcase(str(final.resolve(strict=False))),
                "token": forged_token,
                "phase": "published",
            }
        ),
        encoding="utf-8",
    )
    identity = os.path.normcase(str(final.resolve(strict=False)))
    key = hashlib.sha256(identity.encode("utf-8")).hexdigest()
    lock_root = tmp_path / "job-locks"
    lock_root.mkdir()
    (lock_root / f"{key}.json").write_text(
        json.dumps(
            {
                "schema": meshy_main._TargetReservation._STATE_SCHEMA,
                "target": identity,
                "token": "cd" * 32,
                "phase": "published",
            }
        ),
        encoding="utf-8",
    )
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    assert meshy_main.main(_base_args(tmp_path)) == 2
    events = _events(capsys.readouterr().out)
    assert events[-1]["event"] == "error"
    assert events[-1]["error_code"] == "invalid_request"
    assert valuable.read_bytes() == b"legitimate completed asset"
    assert (final / ".meshy-ownership.json").exists()
    assert (parent / f".protocol_marker.{forged_token}.owner").exists()


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
    assert not list(asset_dir.parent.glob(".protocol_marker.*.partial"))
    assert not list(asset_dir.parent.glob(".protocol_marker.*.owner"))
    assert list(asset_dir.parent.iterdir()) == []
