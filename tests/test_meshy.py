"""Tests for the Meshy.ai intake CLI (tools/meshy) — spec 4 section 7.1/7.2, TD-09.

httpx is FULLY mocked via `httpx.MockTransport` — zero live network calls, ever
(repo testing rules: every external API is mocked in unit tests; CI never calls
the real Meshy API). CLI invocations call `meshy.__main__.main()` in-process,
matching the direct-import style the other tool test modules use
(tests/test_meridian_export.py, tests/test_validate_imports.py).
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import httpx
import pytest
import yaml
from jsonschema import Draft202012Validator

REPO = Path(__file__).resolve().parent.parent
SCHEMA_DIR = REPO / "schema" / "content"

sys.path.insert(0, str(REPO / "tools"))

import meshy.__main__ as meshy_main  # noqa: E402
import meshy.client as client_mod  # noqa: E402
import meshy.intake as intake  # noqa: E402
import meshy.convert_rig as convert_rig  # noqa: E402
import meshy.mapping as mapping  # noqa: E402
import validate_content  # noqa: E402
import validate_imports  # noqa: E402
from validate_content import check_provenance as ci_check_provenance  # noqa: E402

# bones.py (canonical bone table) is pure-Python and importable without Blender.
sys.path.insert(0, str(REPO / "tools" / "blender" / "meridian_rig"))
import bones  # noqa: E402


# --- Schema validator, with the shared $defs merged in (README contract). ------
@pytest.fixture(scope="module")
def asset_validator() -> Draft202012Validator:
    common = yaml.safe_load(
        (SCHEMA_DIR / "common.defs.yaml").read_text(encoding="utf-8")
    )
    schema = yaml.safe_load(
        (SCHEMA_DIR / "asset.schema.yaml").read_text(encoding="utf-8")
    )
    schema.setdefault("$defs", {}).update(common["$defs"])
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema)


# --- Minimal, valid .glb fixture builder ----------------------------------
# Builds a real (loadable) glTF-binary with exactly `triangle_count` triangles
# in a single mesh primitive, using pygltflib the same way tests/test_meridian_rig.py
# (task 4) is expected to. Vertex data is throwaway; only accessor .count metadata
# matters for the triangle-counting logic under test.
def _make_glb(path: Path, triangle_count: int, vertex_count: int = 4) -> Path:
    from pygltflib import (
        ARRAY_BUFFER,
        ELEMENT_ARRAY_BUFFER,
        FLOAT,
        SCALAR,
        UNSIGNED_SHORT,
        VEC3,
        Accessor,
        Attributes,
        Buffer,
        BufferView,
        GLTF2,
        Mesh,
        Node,
        Primitive,
        Scene,
    )

    indices_count = triangle_count * 3
    indices = [i % vertex_count for i in range(indices_count)]
    indices_bytes = struct.pack(f"<{indices_count}H", *indices)
    pad = (-len(indices_bytes)) % 4
    indices_bytes_padded = indices_bytes + b"\x00" * pad
    positions: list[float] = []
    for i in range(vertex_count):
        positions += [float(i), 0.0, 0.0]
    positions_bytes = struct.pack(f"<{len(positions)}f", *positions)
    blob = indices_bytes_padded + positions_bytes

    gltf = GLTF2(
        scene=0,
        scenes=[Scene(nodes=[0])],
        nodes=[Node(mesh=0)],
        meshes=[
            Mesh(
                primitives=[
                    Primitive(attributes=Attributes(POSITION=1), indices=0, mode=4)
                ]
            )
        ],
        accessors=[
            Accessor(
                bufferView=0,
                componentType=UNSIGNED_SHORT,
                count=indices_count,
                type=SCALAR,
                byteOffset=0,
            ),
            Accessor(
                bufferView=1,
                componentType=FLOAT,
                count=vertex_count,
                type=VEC3,
                byteOffset=0,
                max=[float(vertex_count - 1), 0.0, 0.0],
                min=[0.0, 0.0, 0.0],
            ),
        ],
        bufferViews=[
            BufferView(
                buffer=0,
                byteOffset=0,
                byteLength=len(indices_bytes),
                target=ELEMENT_ARRAY_BUFFER,
            ),
            BufferView(
                buffer=0,
                byteOffset=len(indices_bytes_padded),
                byteLength=len(positions_bytes),
                target=ARRAY_BUFFER,
            ),
        ],
        buffers=[Buffer(byteLength=len(blob))],
    )
    gltf.set_binary_blob(blob)
    gltf.save(str(path))
    return path


GLB_ASSET_URL = "https://assets.meshy.ai/model.glb"


def _text_to_3d_handler(
    *, glb_bytes: bytes, preview_status="SUCCEEDED", refine_status="SUCCEEDED"
):
    """A MockTransport handler for the full preview->refine->download flow."""
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        calls.append(request)
        path = request.url.path
        if request.method == "POST" and path == client_mod.TEXT_TO_3D_PATH:
            body = json.loads(request.content)
            if body["mode"] == "preview":
                return httpx.Response(200, json={"result": "task_preview_1"})
            if body["mode"] == "refine":
                return httpx.Response(200, json={"result": "task_refine_1"})
            raise AssertionError(f"unexpected mode: {body}")
        if (
            request.method == "GET"
            and path == f"{client_mod.TEXT_TO_3D_PATH}/task_preview_1"
        ):
            return httpx.Response(200, json={"status": preview_status, "progress": 100})
        if (
            request.method == "GET"
            and path == f"{client_mod.TEXT_TO_3D_PATH}/task_refine_1"
        ):
            return httpx.Response(
                200,
                json={
                    "status": refine_status,
                    "progress": 100,
                    "model_urls": {"glb": GLB_ASSET_URL},
                },
            )
        if request.method == "GET" and str(request.url) == GLB_ASSET_URL:
            return httpx.Response(200, content=glb_bytes)
        raise AssertionError(f"unexpected request: {request.method} {request.url}")

    return handler, calls


def _preview_payload(calls: list[httpx.Request]) -> dict:
    """Extract the text-to-3D preview POST body from a recorded call list."""
    for c in calls:
        if c.method == "POST" and c.url.path == client_mod.TEXT_TO_3D_PATH:
            body = json.loads(c.content)
            if body.get("mode") == "preview":
                return body
    raise AssertionError("no text-to-3D preview POST was captured")


# ============================================================================
# client.py — MeshyClient unit tests (httpx.MockTransport, no live HTTP)
# ============================================================================


def test_text_to_3d_happy_path_returns_refine_task_handle(tmp_path):
    glb = _make_glb(tmp_path / "src.glb", triangle_count=2)
    handler, calls = _text_to_3d_handler(glb_bytes=glb.read_bytes())
    transport = httpx.MockTransport(handler)
    client = client_mod.MeshyClient("fake-key", transport=transport)

    handle = client.text_to_3d("a small barrel", poll_interval_s=0, poll_timeout_s=5)

    assert handle.task_id == "task_refine_1"
    assert handle.preview_task_id == "task_preview_1"
    assert handle.request_payload["mode"] == "preview"
    assert handle.request_payload["prompt"] == "a small barrel"
    assert any(c.method == "POST" for c in calls)


def test_client_default_timeout_is_generous_read():
    # Issue #735: the old hardwired 30s scalar killed slow-but-healthy Meshy
    # create/poll/download calls. The new default keeps a short connect but a
    # generous read/write/pool so a slow *response* is waited out, not aborted.
    client = client_mod.MeshyClient("fake-key")
    try:
        assert client.timeout == client_mod.DEFAULT_HTTP_TIMEOUT
        assert client.timeout.read == 120.0
        assert client.timeout.connect == 10.0
        assert client.timeout.write == 120.0
        assert client.timeout.pool == 120.0
    finally:
        client.close()


def test_client_scalar_timeout_override_sets_read_keeps_short_connect():
    # A scalar override raises read/write/pool but must keep the short connect
    # cap — the failure mode is a slow response, never a slow TCP connect.
    client = client_mod.MeshyClient("fake-key", timeout=200)
    try:
        assert client.timeout.read == 200.0
        assert client.timeout.write == 200.0
        assert client.timeout.pool == 200.0
        assert client.timeout.connect == 10.0
    finally:
        client.close()


def test_client_passes_timeout_into_httpx_call():
    # The configured timeout must actually reach the wire: httpx stamps it onto
    # every request's extensions, which is what the transport ultimately honors.
    seen: list[dict] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request.extensions.get("timeout"))
        return httpx.Response(200, json={"status": "SUCCEEDED", "progress": 100})

    client = client_mod.MeshyClient(
        "fake-key", transport=httpx.MockTransport(handler), timeout=200
    )
    client.poll("task_1", interval_s=0, timeout_s=5)
    assert seen and seen[0]["read"] == 200.0
    assert seen[0]["connect"] == 10.0


def test_poll_returns_terminal_status():
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json={"status": "SUCCEEDED", "progress": 100})

    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    status = client.poll("task_1", interval_s=0, timeout_s=5)
    assert status.succeeded
    assert status.status == "SUCCEEDED"


def test_poll_times_out_without_real_sleep():
    """Simulates a task stuck IN_PROGRESS forever; timeout_s=0 means the first
    fetch already misses the deadline, so no real sleep ever executes."""

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json={"status": "IN_PROGRESS", "progress": 40})

    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    sleep_calls: list[float] = []

    with pytest.raises(client_mod.MeshyPollTimeoutError):
        client.poll(
            "task_1",
            interval_s=0,
            timeout_s=0,
            sleep=lambda s: sleep_calls.append(s),
        )
    assert sleep_calls == []  # never slept — the deadline was already hit


def test_text_to_3d_raises_on_api_error():
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, text="internal error")

    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    with pytest.raises(client_mod.MeshyAPIError):
        client.text_to_3d("anything", poll_interval_s=0, poll_timeout_s=5)


def test_text_to_3d_raises_when_preview_fails():
    def handler(request: httpx.Request) -> httpx.Response:
        if request.method == "POST":
            return httpx.Response(200, json={"result": "task_preview_1"})
        return httpx.Response(200, json={"status": "FAILED", "progress": 0})

    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    with pytest.raises(client_mod.MeshyAPIError):
        client.text_to_3d("anything", poll_interval_s=0, poll_timeout_s=5)


def test_download_writes_glb_bytes(tmp_path):
    glb = _make_glb(tmp_path / "src.glb", triangle_count=2)
    payload = glb.read_bytes()

    def handler(request: httpx.Request) -> httpx.Response:
        if str(request.url) == GLB_ASSET_URL:
            return httpx.Response(200, content=payload)
        return httpx.Response(
            200,
            json={"status": "SUCCEEDED", "model_urls": {"glb": GLB_ASSET_URL}},
        )

    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    dest = tmp_path / "out" / "model.glb"
    result = client.download("task_1", dest)
    assert result == dest
    assert dest.read_bytes() == payload


# ============================================================================
# intake.py — pure sidecar/prompts shaping tests
# ============================================================================


def test_count_glb_triangles_matches_fixture(tmp_path):
    glb = _make_glb(tmp_path / "t.glb", triangle_count=17)
    assert intake.count_glb_triangles(glb) == 17


def test_build_sidecar_is_ai_tier_pending(asset_validator):
    doc = intake.build_sidecar(
        asset_id="core:art.orc_warrior",
        asset_class="character_model",
        source="assets/art/orc_warrior/sk_orc_warrior.glb",
        model_version="meshy-5",
        prompts_file="orc_warrior.prompts.yaml",
        origin_url="https://api.meshy.ai/openapi/v2/text-to-3d/task_refine_1",
        authors=["meridian-contributors"],
        lod0_tris=12_000,
    )
    asset_validator.validate(doc)  # raises on schema violation
    assert doc["provenance"]["source_tier"] == "ai"
    assert doc["restyle_status"] == "pending"
    assert doc["provenance"]["ai"]["tool"] == "meshy@meshy-5"
    assert doc["provenance"]["ai"]["prompts_file"] == "orc_warrior.prompts.yaml"
    errors = ci_check_provenance(
        doc, Path("core/assets/art/orc_warrior/orc_warrior.asset.yaml")
    )
    assert errors == []


def test_build_prompts_doc_carries_exact_request_and_task_id():
    request_payload = {
        "mode": "preview",
        "prompt": "an orc warrior",
        "ai_model": "meshy-5",
    }
    doc = intake.build_prompts_doc(
        task_id="task_refine_1",
        preview_task_id="task_preview_1",
        request_payload=request_payload,
        model_version="meshy-5",
    )
    assert doc["task_id"] == "task_refine_1"
    assert doc["preview_task_id"] == "task_preview_1"
    assert doc["request"] == request_payload


def test_validate_name_rejects_non_snake_case():
    with pytest.raises(intake.IntakeError):
        intake.validate_name("Orc-Warrior")


def test_validate_namespace_rejects_bad_ns():
    with pytest.raises(intake.IntakeError):
        intake.validate_namespace("Core!")


def test_landing_paths_shape(tmp_path):
    budgets = intake.load_budgets()
    paths = intake.landing_paths(
        content_root=tmp_path,
        ns="core",
        name="orc_warrior",
        asset_class="character_model",
        budgets=budgets,
    )
    assert paths.glb_path == tmp_path / "core/assets/art/orc_warrior/sk_orc_warrior.glb"
    assert (
        paths.sidecar_path
        == tmp_path / "core/assets/art/orc_warrior/orc_warrior.asset.yaml"
    )
    assert (
        paths.prompts_path
        == tmp_path / "core/assets/art/orc_warrior/orc_warrior.prompts.yaml"
    )
    assert paths.source == "assets/art/orc_warrior/sk_orc_warrior.glb"


# ============================================================================
# target_polycount derivation (issue #627) — pure intake logic
# ============================================================================


def test_derive_target_polycount_defaults_below_kit_piece_ceiling():
    # kit_piece caps lod0_tris at 20000; the default must request headroom
    # under that ceiling (Meshy tracks the request closely, so a ceiling-equal
    # request routinely lands over budget). ~80% -> 16000.
    budgets = intake.load_budgets()
    got = intake.derive_target_polycount("kit_piece", budgets)
    ceiling = budgets["kit_piece"]["lod0_tris"]
    assert got == 16000
    assert got <= ceiling


def test_derive_target_polycount_honors_explicit_override():
    budgets = intake.load_budgets()
    got = intake.derive_target_polycount("kit_piece", budgets, override=12000)
    assert got == 12000


def test_derive_target_polycount_rejects_override_above_ceiling():
    budgets = intake.load_budgets()
    with pytest.raises(intake.IntakeError) as exc:
        intake.derive_target_polycount("kit_piece", budgets, override=25000)
    message = str(exc.value)
    assert "25000" in message
    assert "20000" in message  # the ceiling is named


def test_derive_target_polycount_rejects_nonpositive_override():
    budgets = intake.load_budgets()
    with pytest.raises(intake.IntakeError):
        intake.derive_target_polycount("kit_piece", budgets, override=0)


def test_derive_target_polycount_rejects_unknown_class():
    budgets = intake.load_budgets()
    with pytest.raises(intake.IntakeError, match="no lod0_tris budget"):
        intake.derive_target_polycount("not_a_real_class", budgets)


# ============================================================================
# __main__.py — CLI end-to-end (in-process; MeshyClient construction is
# monkeypatched to inject the mock transport — see meshy_main._new_client)
# ============================================================================


def _patch_client(monkeypatch, handler):
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(
        meshy_main,
        "_new_client",
        lambda api_key, model_version=client_mod.DEFAULT_MODEL_VERSION, *, timeout=None: (
            client_mod.MeshyClient(
                api_key,
                model_version=model_version,
                transport=transport,
                timeout=timeout,
            )
        ),
    )


def test_generate_happy_path_lands_sidecar_and_prompts(
    tmp_path, monkeypatch, asset_validator
):
    glb_bytes = _make_glb(tmp_path / "src.glb", triangle_count=2).read_bytes()
    handler, _calls = _text_to_3d_handler(glb_bytes=glb_bytes)
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    content_root = tmp_path / "content"
    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(content_root),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 0

    asset_dir = content_root / "core" / "assets" / "art" / "test_barrel"
    sidecar_path = asset_dir / "test_barrel.asset.yaml"
    prompts_path = asset_dir / "test_barrel.prompts.yaml"
    glb_path = asset_dir / "sm_test_barrel.glb"

    assert glb_path.exists()
    assert sidecar_path.exists()
    assert prompts_path.exists()

    doc = yaml.safe_load(sidecar_path.read_text())
    asset_validator.validate(doc)
    assert doc["provenance"]["source_tier"] == "ai"
    assert doc["restyle_status"] == "pending"
    assert ci_check_provenance(doc, sidecar_path) == []

    prompts_doc = yaml.safe_load(prompts_path.read_text())
    assert prompts_doc["task_id"] == "task_refine_1"
    assert prompts_doc["preview_task_id"] == "task_preview_1"
    assert prompts_doc["request"]["prompt"] == "a small barrel"


def _run_generate_recording_timeouts(tmp_path, monkeypatch, extra_args=()):
    """Drive `generate` (prop, happy path) recording every request's httpx
    timeout, so timeout resolution can be asserted end-to-end into the wire."""
    glb_bytes = _make_glb(tmp_path / "src.glb", triangle_count=2).read_bytes()
    handler, _calls = _text_to_3d_handler(glb_bytes=glb_bytes)
    seen: list[dict] = []

    def recording(request: httpx.Request) -> httpx.Response:
        seen.append(request.extensions.get("timeout"))
        return handler(request)

    _patch_client(monkeypatch, recording)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
            *extra_args,
        ]
    )
    return rc, seen


def test_generate_uses_default_http_timeout_on_every_call(tmp_path, monkeypatch):
    rc, seen = _run_generate_recording_timeouts(tmp_path, monkeypatch)
    assert rc == 0
    assert seen  # at least one request went out
    for timeout in seen:
        assert timeout["read"] == client_mod.DEFAULT_HTTP_TIMEOUT_S
        assert timeout["connect"] == client_mod.DEFAULT_HTTP_CONNECT_TIMEOUT_S


def test_generate_timeout_flag_overrides_default(tmp_path, monkeypatch):
    rc, seen = _run_generate_recording_timeouts(
        tmp_path, monkeypatch, extra_args=["--timeout", "200"]
    )
    assert rc == 0
    assert seen
    for timeout in seen:
        assert timeout["read"] == 200.0


def test_generate_honors_meshy_timeout_env(tmp_path, monkeypatch):
    monkeypatch.setenv("MESHY_TIMEOUT", "200")
    rc, seen = _run_generate_recording_timeouts(tmp_path, monkeypatch)
    assert rc == 0
    assert seen
    for timeout in seen:
        assert timeout["read"] == 200.0


def test_generate_timeout_flag_beats_env(tmp_path, monkeypatch):
    monkeypatch.setenv("MESHY_TIMEOUT", "200")
    rc, seen = _run_generate_recording_timeouts(
        tmp_path, monkeypatch, extra_args=["--timeout", "45"]
    )
    assert rc == 0
    assert seen
    for timeout in seen:
        assert timeout["read"] == 45.0


def test_generate_refuses_malformed_meshy_timeout_env(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call should happen for a malformed timeout")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    monkeypatch.setenv("MESHY_TIMEOUT", "not-a-number")
    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
        ]
    )
    assert rc == 2
    assert "MESHY_TIMEOUT" in capsys.readouterr().err


def test_generate_refuses_nonpositive_timeout(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call should happen for a nonpositive timeout")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--terms-verified",
            "--timeout",
            "0",
            "--content-root",
            str(tmp_path / "content"),
        ]
    )
    assert rc == 2
    assert "--timeout" in capsys.readouterr().err


def _run_kit_piece_generate(tmp_path, monkeypatch, extra_args=()):
    """Drive `generate` for a kit_piece (ceiling 20000) and return (rc, calls)."""
    glb_bytes = _make_glb(tmp_path / "src.glb", triangle_count=2).read_bytes()
    handler, calls = _text_to_3d_handler(glb_bytes=glb_bytes)
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a stone archway kit piece",
            "--ns",
            "core",
            "--name",
            "test_kit",
            "--class",
            "kit_piece",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
            *extra_args,
        ]
    )
    return rc, calls


def test_generate_defaults_target_polycount_under_kit_piece_ceiling(
    tmp_path, monkeypatch
):
    # The core #627 regression: with no override, a kit_piece intake must
    # request a polycount at/under its 20000 ceiling (was hardwired to 30000,
    # guaranteeing an over-budget refusal that blocked #141).
    rc, calls = _run_kit_piece_generate(tmp_path, monkeypatch)
    assert rc == 0
    body = _preview_payload(calls)
    ceiling = intake.load_budgets()["kit_piece"]["lod0_tris"]
    assert body["target_polycount"] <= ceiling
    assert body["target_polycount"] == 16000


def test_generate_honors_explicit_target_polycount_override(tmp_path, monkeypatch):
    rc, calls = _run_kit_piece_generate(
        tmp_path, monkeypatch, extra_args=["--target-polycount", "12000"]
    )
    assert rc == 0
    body = _preview_payload(calls)
    assert body["target_polycount"] == 12000


def test_generate_refuses_target_polycount_over_class_ceiling(
    tmp_path, monkeypatch, capsys
):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call should happen for an over-ceiling request")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a stone archway kit piece",
            "--ns",
            "core",
            "--name",
            "test_kit",
            "--class",
            "kit_piece",
            "--target-polycount",
            "25000",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
        ]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert err.startswith("refused:")
    assert "25000" in err
    assert "20000" in err  # the ceiling is named


def test_generate_refuses_target_polycount_with_image_mode(
    tmp_path, monkeypatch, capsys
):
    # --target-polycount is a text-to-3D control; silently ignoring it on an
    # image intake would be a provenance lie, so it is refused up front.
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call should happen for a refused combination")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "generate",
            "--image",
            "https://example.com/ref.png",
            "--ns",
            "core",
            "--name",
            "test_img",
            "--class",
            "kit_piece",
            "--target-polycount",
            "12000",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
        ]
    )
    assert rc == 2
    assert "--target-polycount" in capsys.readouterr().err


def test_generate_refuses_without_terms_verified(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call should happen without --terms-verified")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--content-root",
            str(tmp_path / "content"),
        ]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert "TD-09" in err


def test_generate_refuses_without_api_key(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call should happen without MESHY_API_KEY")

    _patch_client(monkeypatch, handler)
    monkeypatch.delenv("MESHY_API_KEY", raising=False)

    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
        ]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert "MESHY_API_KEY" in err


def test_generate_nonzero_exit_on_api_error(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(500, text="server exploded")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "1",
        ]
    )
    assert rc == 1
    content_root = tmp_path / "content"
    assert not (content_root / "core" / "assets" / "art" / "test_barrel").exists()


def test_generate_nonzero_exit_on_poll_timeout(tmp_path, monkeypatch):
    handler, _calls = _text_to_3d_handler(
        glb_bytes=b"unused", preview_status="IN_PROGRESS"
    )
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "0",
        ]
    )
    assert rc == 1


def test_generate_budget_precheck_failure_does_not_write_sidecar(tmp_path, monkeypatch):
    # class "prop" caps lod0_tris at 3000 (Art PRD §2.1) — 4000 triangles is over.
    over_budget_glb = _make_glb(tmp_path / "over.glb", triangle_count=4000).read_bytes()
    handler, _calls = _text_to_3d_handler(glb_bytes=over_budget_glb)
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    content_root = tmp_path / "content"
    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a huge barrel",
            "--ns",
            "core",
            "--name",
            "test_bigbarrel",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(content_root),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 1
    asset_dir = content_root / "core" / "assets" / "art" / "test_bigbarrel"
    assert not (asset_dir / "test_bigbarrel.asset.yaml").exists()
    assert not (asset_dir / "test_bigbarrel.prompts.yaml").exists()
    # the oversized glb is cleaned up too — no orphaned artifact left behind.
    assert not (asset_dir / "sm_test_bigbarrel.glb").exists()


def test_generate_image_mode_happy_path(tmp_path, monkeypatch, asset_validator):
    glb_bytes = _make_glb(tmp_path / "src.glb", triangle_count=2).read_bytes()

    def handler(request: httpx.Request) -> httpx.Response:
        path = request.url.path
        if request.method == "POST" and path == client_mod.IMAGE_TO_3D_PATH:
            return httpx.Response(200, json={"result": "task_image_1"})
        if (
            request.method == "GET"
            and path == f"{client_mod.IMAGE_TO_3D_PATH}/task_image_1"
        ):
            return httpx.Response(
                200,
                json={"status": "SUCCEEDED", "model_urls": {"glb": GLB_ASSET_URL}},
            )
        if request.method == "GET" and str(request.url) == GLB_ASSET_URL:
            return httpx.Response(200, content=glb_bytes)
        raise AssertionError(f"unexpected request: {request.method} {request.url}")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    content_root = tmp_path / "content"
    rc = meshy_main.main(
        [
            "generate",
            "--image",
            "https://example.com/ref.png",
            "--ns",
            "core",
            "--name",
            "test_imgprop",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(content_root),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 0
    sidecar_path = (
        content_root
        / "core"
        / "assets"
        / "art"
        / "test_imgprop"
        / "test_imgprop.asset.yaml"
    )
    doc = yaml.safe_load(sidecar_path.read_text())
    asset_validator.validate(doc)
    assert doc["provenance"]["source_tier"] == "ai"


# ============================================================================
# poll() API-shape-drift guard — unrecognized status fails loudly (review fix)
# ============================================================================


def test_poll_unrecognized_status_raises_immediately_naming_value():
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json={"status": "EXPLODED", "progress": 0})

    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    with pytest.raises(client_mod.MeshyAPIError) as exc_info:
        client.poll("task_1", interval_s=0, timeout_s=60)
    message = str(exc_info.value)
    assert "EXPLODED" in message
    for known in sorted(client_mod.KNOWN_STATUSES):
        assert known in message


def test_poll_missing_status_field_raises_immediately():
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json={"progress": 0})

    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    with pytest.raises(client_mod.MeshyAPIError, match="None"):
        client.poll("task_1", interval_s=0, timeout_s=60)


# ============================================================================
# --image local-file support (data-URI encoding; review fix)
# ============================================================================

# 1x1 transparent PNG (real, decodable bytes for the fixture file).
_TINY_PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000101030000002562d1"
    "4f0000000467414d410000b18f0bfc6105000000206348524d00007a26000080"
    "840000fa00000080e8000075300000ea6000003a98000017709cba513c000000"
    "06504c5445000000ffffffa5d99fdd0000000174524e530040e6d8660000000a"
    "4944415408d76360000000020001e221bc330000000049454e44ae426082"
)


def test_image_ref_to_url_passes_urls_through():
    assert (
        intake.image_ref_to_url("https://example.com/a.png")
        == "https://example.com/a.png"
    )
    assert intake.image_ref_to_url("data:image/png;base64,AAAA").startswith("data:")


def test_image_ref_to_url_encodes_local_png_as_data_uri(tmp_path):
    import base64

    img = tmp_path / "ref.png"
    img.write_bytes(_TINY_PNG)
    url = intake.image_ref_to_url(str(img))
    assert url.startswith("data:image/png;base64,")
    assert base64.b64decode(url.split(",", 1)[1]) == _TINY_PNG


def test_image_ref_to_url_rejects_missing_file_and_bad_extension(tmp_path):
    with pytest.raises(intake.IntakeError, match="neither a URL nor an existing"):
        intake.image_ref_to_url(str(tmp_path / "nope.png"))
    bad = tmp_path / "ref.gif"
    bad.write_bytes(b"GIF89a")
    with pytest.raises(intake.IntakeError, match="unsupported extension"):
        intake.image_ref_to_url(str(bad))


def test_build_prompts_doc_replaces_data_uri_with_digest():
    data_uri = "data:image/png;base64," + "A" * 4096
    doc = intake.build_prompts_doc(
        task_id="task_image_1",
        preview_task_id=None,
        request_payload={"image_url": data_uri, "ai_model": "meshy-5"},
        model_version="meshy-5",
    )
    recorded = doc["request"]["image_url"]
    assert "data-uri omitted" in recorded
    assert "sha256:" in recorded
    assert len(recorded) < 200  # never megabytes of base64 in the companion


def test_generate_image_mode_local_file_sends_data_uri(tmp_path, monkeypatch):
    img = tmp_path / "ref.png"
    img.write_bytes(_TINY_PNG)
    glb_bytes = _make_glb(tmp_path / "src.glb", triangle_count=2).read_bytes()
    submitted_image_urls: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        path = request.url.path
        if request.method == "POST" and path == client_mod.IMAGE_TO_3D_PATH:
            submitted_image_urls.append(json.loads(request.content)["image_url"])
            return httpx.Response(200, json={"result": "task_image_1"})
        if (
            request.method == "GET"
            and path == f"{client_mod.IMAGE_TO_3D_PATH}/task_image_1"
        ):
            return httpx.Response(
                200,
                json={"status": "SUCCEEDED", "model_urls": {"glb": GLB_ASSET_URL}},
            )
        if request.method == "GET" and str(request.url) == GLB_ASSET_URL:
            return httpx.Response(200, content=glb_bytes)
        raise AssertionError(f"unexpected request: {request.method} {request.url}")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    content_root = tmp_path / "content"
    rc = meshy_main.main(
        [
            "generate",
            "--image",
            str(img),
            "--ns",
            "core",
            "--name",
            "test_localimg",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(content_root),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 0
    assert submitted_image_urls == [intake.image_ref_to_url(str(img))]
    assert submitted_image_urls[0].startswith("data:image/png;base64,")
    prompts_doc = yaml.safe_load(
        (
            content_root / "core/assets/art/test_localimg/test_localimg.prompts.yaml"
        ).read_text()
    )
    assert "data-uri omitted" in prompts_doc["request"]["image_url"]


def test_generate_refuses_missing_local_image_before_http(
    tmp_path, monkeypatch, capsys
):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call should happen for an invalid --image")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    rc = meshy_main.main(
        [
            "generate",
            "--image",
            str(tmp_path / "missing.png"),
            "--ns",
            "core",
            "--name",
            "test_x",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
        ]
    )
    assert rc == 2
    assert "neither a URL nor an existing" in capsys.readouterr().err


# ============================================================================
# CLI-level refusal for invalid --name / --ns (review fix)
# ============================================================================


@pytest.mark.parametrize(
    ("ns", "name", "expected_fragment"),
    [
        ("core", "Bad-Name", "--name"),
        ("Bad!NS", "good_name", "--ns"),
    ],
)
def test_generate_refuses_invalid_ns_or_name_before_http(
    tmp_path, monkeypatch, capsys, ns, name, expected_fragment
):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call should happen for invalid --ns/--name")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a barrel",
            "--ns",
            ns,
            "--name",
            name,
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(tmp_path / "content"),
        ]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert err.startswith("refused:")
    assert expected_fragment in err


# ============================================================================
# Integration: the landed asset dir (glb + sidecar + prompts file TOGETHER)
# passes the real validate_content gate end-to-end (review fix — this is the
# blind spot that hid the *.prompts.yaml L001 bug: earlier tests validated
# the sidecar dict in isolation, never the on-disk tree the CLI creates).
#
# Issue #525: this test used to assert `res.errors == []` outright — which was
# itself the evidence cited in #525 that the restyle-quarantine lint (spec ④
# §7.2 / tools/meshy/README.md's documented "raw Meshy output never ships")
# didn't actually exist: a freshly generated, still-`pending` ai-tier sidecar
# validated clean. Now that L024 is implemented, the correct end-to-end
# assertion is the opposite — every OTHER layer (schema, L021 provenance,
# budget, import presets) stays clean, and L024 is the one error a raw,
# not-yet-restyled Meshy drop is expected to trip.
# ============================================================================


def test_generate_output_tree_passes_validate_content_end_to_end(tmp_path, monkeypatch):
    glb_bytes = _make_glb(tmp_path / "src.glb", triangle_count=2).read_bytes()
    handler, _calls = _text_to_3d_handler(glb_bytes=glb_bytes)
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")

    # A minimal but real pack: pack.yaml + the CLI-landed asset dir.
    content_root = tmp_path / "content"
    pack_dir = content_root / "core"
    pack_dir.mkdir(parents=True)
    (pack_dir / "pack.yaml").write_text(
        yaml.safe_dump(
            {
                "schema": "meridian/pack@1",
                "namespace": "core",
                "name": "Test Pack",
                "version": "0.1.0",
                "content_schema_version": 1,
                "engine": {"godot": "4.6"},
                "license": "Apache-2.0 (data) / CC-BY-4.0 (referenced original assets)",
            },
            sort_keys=False,
        ),
        encoding="utf-8",
    )

    rc = meshy_main.main(
        [
            "generate",
            "--text",
            "a small barrel",
            "--ns",
            "core",
            "--name",
            "test_barrel",
            "--class",
            "prop",
            "--terms-verified",
            "--content-root",
            str(content_root),
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 0
    asset_dir = content_root / "core" / "assets" / "art" / "test_barrel"
    # All three artifacts land together — the tree CI would actually see.
    assert (asset_dir / "sm_test_barrel.glb").exists()
    assert (asset_dir / "test_barrel.asset.yaml").exists()
    assert (asset_dir / "test_barrel.prompts.yaml").exists()

    # The full validate_content gate (schemas + lints + import presets) over
    # the real on-disk tree — exactly what CI runs. Raw Meshy output lands with
    # restyle_status: pending (tools/meshy/intake.py; README "Provenance
    # shape") and issue #525's L024 restyle-quarantine lint now blocks it from
    # merging as-is: every other layer must stay clean, but L024 MUST fire —
    # that is the entire point of the lint this test exercises end-to-end.
    res = validate_content.validate(
        content_root,
        SCHEMA_DIR,
        assets_mode="error",
        imports_mode="error",
        presets_path=REPO / "client" / "import-presets" / "presets.json",
    )
    other_errors = [e for e in res.errors if not e.startswith("L024")]
    assert other_errors == [], other_errors
    assert any(e.startswith("L024") for e in res.errors), (
        "expected L024 (restyle quarantine) to fire on a freshly-generated, "
        "still-pending ai-tier sidecar — see issue #525"
    )


# ============================================================================
# mapping.py — PURE bone-map load + ConversionPlan resolution (spec ④ §7.3)
# No Blender, no network, no verified live map required: the plan logic is
# exercised against a HYPOTHETICAL map version written to tmp_path, and the
# committed seed map is checked only for structural validity (targets ∈ bones).
# ============================================================================

_HYPOTHETICAL_MAP = {
    "versions": {
        "test-rig-v1": {
            "verified": True,
            "bones": {
                "Hips": "Hips",
                "Spine": "Spine",
                "Spine1": "Chest",
                "LeftArm": "LeftUpperArm",
                "LeftForeArm": "LeftLowerArm",
                "LeftHand": "LeftHand",
            },
        },
    },
}


def _write_map(tmp_path: Path, doc: dict) -> Path:
    p = tmp_path / "bone_map.yaml"
    p.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
    return p


def test_plan_resolves_renames_for_known_version(tmp_path):
    map_path = _write_map(tmp_path, _HYPOTHETICAL_MAP)
    result = mapping.plan(
        ["Hips", "Spine", "Spine1", "LeftArm", "LeftForeArm", "LeftHand"],
        "test-rig-v1",
        path=map_path,
    )
    assert result.renames == {
        "Hips": "Hips",
        "Spine": "Spine",
        "Spine1": "Chest",
        "LeftArm": "LeftUpperArm",
        "LeftForeArm": "LeftLowerArm",
        "LeftHand": "LeftHand",
    }
    assert result.merges == {}
    assert result.unmapped == []


def test_plan_merges_twist_helper_into_nearest_mapped_ancestor(tmp_path):
    map_path = _write_map(tmp_path, _HYPOTHETICAL_MAP)
    # LeftForeArmTwist is a helper/twist bone absent from the map but named as a
    # CamelCase descendant of the mapped LeftForeArm → its weights merge into
    # that bone's canonical target (LeftLowerArm), never a rename.
    result = mapping.plan(
        ["LeftForeArm", "LeftForeArmTwist", "LeftHand"], "test-rig-v1", path=map_path
    )
    assert result.renames == {
        "LeftForeArm": "LeftLowerArm",
        "LeftHand": "LeftHand",
    }
    assert result.merges == {"LeftForeArmTwist": "LeftLowerArm"}
    assert result.unmapped == []


def test_plan_lists_truly_unknown_bones_as_unmapped(tmp_path):
    map_path = _write_map(tmp_path, _HYPOTHETICAL_MAP)
    result = mapping.plan(["Hips", "Tail_01", "Wing_L"], "test-rig-v1", path=map_path)
    assert result.renames == {"Hips": "Hips"}
    assert result.merges == {}
    assert sorted(result.unmapped) == ["Tail_01", "Wing_L"]


def test_plan_digit_suffix_is_unmapped_not_silently_merged(tmp_path):
    # A numbered REAL chain bone the map doesn't know (Spine3 under a map with
    # Spine/Spine1) must surface as unmapped — folding it into Spine's target
    # silently would mis-convert a whole spine segment (PR #523 review, item 3).
    # Only CamelCase ('...Twist') and separator ('..._twist') suffixes are
    # helper-shaped; digit-adjacent matches are rejected.
    map_path = _write_map(tmp_path, _HYPOTHETICAL_MAP)
    result = mapping.plan(["Spine", "Spine3"], "test-rig-v1", path=map_path)
    assert result.renames == {"Spine": "Spine"}
    assert result.merges == {}
    assert result.unmapped == ["Spine3"]


def test_plan_separator_suffix_still_merges(tmp_path):
    map_path = _write_map(tmp_path, _HYPOTHETICAL_MAP)
    result = mapping.plan(
        ["LeftForeArm", "LeftForeArm_twist"], "test-rig-v1", path=map_path
    )
    assert result.merges == {"LeftForeArm_twist": "LeftLowerArm"}
    assert result.unmapped == []


def test_plan_unknown_version_raises_naming_known_versions(tmp_path):
    map_path = _write_map(tmp_path, _HYPOTHETICAL_MAP)
    with pytest.raises(mapping.UnknownVersionError) as exc:
        mapping.plan(["Hips"], "meshy-999", path=map_path)
    message = str(exc.value)
    assert "meshy-999" in message
    assert "test-rig-v1" in message  # known versions are named


def test_load_map_reports_verified_flag(tmp_path):
    map_path = _write_map(tmp_path, _HYPOTHETICAL_MAP)
    bmap = mapping.load_map("test-rig-v1", path=map_path)
    assert bmap.verified is True
    assert bmap.version == "test-rig-v1"
    assert bmap.bones["Spine1"] == "Chest"


def test_load_map_rejects_non_boolean_verified(tmp_path):
    # The verified flag is the gate's spine: a quoted YAML string "false" is
    # truthy under bool() and would UNLOCK the gate. Strict-boolean parsing must
    # refuse the map instead (PR #523 review, item 4).
    doc = {
        "versions": {
            "test-rig-v1": {"verified": "false", "bones": {"Hips": "Hips"}},
        },
    }
    map_path = _write_map(tmp_path, doc)
    with pytest.raises(mapping.MappingError, match="verified"):
        mapping.load_map("test-rig-v1", path=map_path)


# --- Committed seed map (tools/meshy/bone_map.yaml) structural validity -------


def test_seed_map_every_target_is_a_canonical_bone():
    canonical = set(bones.bone_names())
    for version in mapping.known_versions():
        bmap = mapping.load_map(version)
        for meshy_bone, target in bmap.bones.items():
            assert target in canonical, (
                f"{version}: '{meshy_bone}' → '{target}' is not a canonical bone"
            )


def test_seed_map_meshy5_is_unverified():
    # The seed map for the live Meshy rig naming is UNVERIFIED (no live sample
    # was obtainable) — convert-rig must refuse it without --allow-unverified-map.
    assert mapping.load_map("meshy-5").verified is False


# ============================================================================
# convert-rig CLI — refusal gates run in pure Python BEFORE any Blender spawn
# (Blender never runs in CI). The Blender conversion itself is a monkeypatched
# seam; only the gate logic is exercised here.
# ============================================================================


def _make_skinned_glb(path: Path, joint_names: list[str]) -> Path:
    """A minimal but loadable skinned .glb whose skin binds `joint_names`."""
    from pygltflib import (
        ARRAY_BUFFER,
        ELEMENT_ARRAY_BUFFER,
        FLOAT,
        SCALAR,
        UNSIGNED_SHORT,
        VEC3,
        VEC4,
        Accessor,
        Attributes,
        Buffer,
        BufferView,
        GLTF2,
        Mesh,
        Node,
        Primitive,
        Scene,
        Skin,
    )

    vcount = 3
    indices = [0, 1, 2]
    indices_bytes = struct.pack(f"<{len(indices)}H", *indices)
    indices_bytes += b"\x00" * ((-len(indices_bytes)) % 4)
    positions = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
    positions_bytes = struct.pack(f"<{len(positions)}f", *positions)
    joints0 = [0, 0, 0, 0] * vcount
    joints0_bytes = struct.pack(f"<{len(joints0)}H", *joints0)
    joints0_bytes += b"\x00" * ((-len(joints0_bytes)) % 4)
    weights0 = [1.0, 0.0, 0.0, 0.0] * vcount
    weights0_bytes = struct.pack(f"<{len(weights0)}f", *weights0)
    blob = indices_bytes + positions_bytes + joints0_bytes + weights0_bytes

    off_idx = 0
    off_pos = len(indices_bytes)
    off_j = off_pos + len(positions_bytes)
    off_w = off_j + len(joints0_bytes)

    # Joint nodes (named) followed by the skinned mesh node.
    joint_nodes = [Node(name=n) for n in joint_names]
    mesh_node = Node(mesh=0, skin=0)
    nodes = joint_nodes + [mesh_node]

    gltf = GLTF2(
        scene=0,
        scenes=[Scene(nodes=list(range(len(nodes))))],
        nodes=nodes,
        meshes=[
            Mesh(
                primitives=[
                    Primitive(
                        attributes=Attributes(POSITION=1, JOINTS_0=2, WEIGHTS_0=3),
                        indices=0,
                        mode=4,
                    )
                ]
            )
        ],
        skins=[Skin(joints=list(range(len(joint_names))))],
        accessors=[
            Accessor(
                bufferView=0,
                componentType=UNSIGNED_SHORT,
                count=len(indices),
                type=SCALAR,
                byteOffset=0,
            ),
            Accessor(
                bufferView=1,
                componentType=FLOAT,
                count=vcount,
                type=VEC3,
                byteOffset=0,
                min=[0.0, 0.0, 0.0],
                max=[1.0, 1.0, 0.0],
            ),
            Accessor(
                bufferView=2,
                componentType=UNSIGNED_SHORT,
                count=vcount,
                type=VEC4,
                byteOffset=0,
            ),
            Accessor(
                bufferView=3, componentType=FLOAT, count=vcount, type=VEC4, byteOffset=0
            ),
        ],
        bufferViews=[
            BufferView(
                buffer=0,
                byteOffset=off_idx,
                byteLength=len(indices_bytes),
                target=ELEMENT_ARRAY_BUFFER,
            ),
            BufferView(
                buffer=0,
                byteOffset=off_pos,
                byteLength=len(positions_bytes),
                target=ARRAY_BUFFER,
            ),
            BufferView(
                buffer=0,
                byteOffset=off_j,
                byteLength=len(joints0_bytes),
                target=ARRAY_BUFFER,
            ),
            BufferView(
                buffer=0,
                byteOffset=off_w,
                byteLength=len(weights0_bytes),
                target=ARRAY_BUFFER,
            ),
        ],
        buffers=[Buffer(byteLength=len(blob))],
    )
    gltf.set_binary_blob(blob)
    gltf.save(str(path))
    return path


def test_joint_names_from_glb_reads_skin_joints(tmp_path):
    glb = _make_skinned_glb(tmp_path / "rig.glb", ["Hips", "Spine", "LeftArm"])
    assert mapping.joint_names_from_glb(glb) == ["Hips", "Spine", "LeftArm"]


def test_convert_rig_refuses_unknown_version(tmp_path, capsys):
    glb = _make_skinned_glb(tmp_path / "rig.glb", ["Hips"])
    rc = meshy_main.main(
        [
            "convert-rig",
            str(glb),
            "--meshy-version",
            "bogus-9",
            "--out",
            str(tmp_path / "out.glb"),
        ]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert "bogus-9" in err
    assert "meshy-5" in err  # known versions named


def test_convert_rig_hard_errors_on_unmapped_bones(tmp_path, capsys, monkeypatch):
    # A joint with no canonical mapping and no mapped-ancestor prefix.
    glb = _make_skinned_glb(tmp_path / "rig.glb", ["Hips", "Spine", "Tentacle_03"])
    called = []
    monkeypatch.setattr(
        meshy_main,
        "_run_blender_conversion",
        lambda *a, **k: called.append((a, k)) or 0,
    )
    rc = meshy_main.main(
        [
            "convert-rig",
            str(glb),
            "--meshy-version",
            "meshy-5",
            "--out",
            str(tmp_path / "out.glb"),
            "--allow-unverified-map",
        ]
    )
    assert rc == 1
    err = capsys.readouterr().err
    assert "Tentacle_03" in err
    assert called == []  # never reached Blender


def test_convert_rig_refuses_unverified_map_without_flag(tmp_path, capsys, monkeypatch):
    glb = _make_skinned_glb(tmp_path / "rig.glb", ["Hips", "Spine"])
    called = []
    monkeypatch.setattr(
        meshy_main,
        "_run_blender_conversion",
        lambda *a, **k: called.append((a, k)) or 0,
    )
    rc = meshy_main.main(
        [
            "convert-rig",
            str(glb),
            "--meshy-version",
            "meshy-5",
            "--out",
            str(tmp_path / "out.glb"),
        ]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert "--allow-unverified-map" in err
    assert called == []


def test_convert_rig_allow_unverified_invokes_blender_seam(tmp_path, monkeypatch):
    glb = _make_skinned_glb(tmp_path / "rig.glb", ["Hips", "Spine"])
    out = tmp_path / "out.glb"
    captured = {}

    def fake_conv(in_glb, out_glb, version, plan_obj, **kwargs):
        captured["in"] = Path(in_glb)
        captured["out"] = Path(out_glb)
        captured["version"] = version
        captured["plan"] = plan_obj
        return 0

    monkeypatch.setattr(meshy_main, "_run_blender_conversion", fake_conv)
    rc = meshy_main.main(
        [
            "convert-rig",
            str(glb),
            "--meshy-version",
            "meshy-5",
            "--out",
            str(out),
            "--allow-unverified-map",
        ]
    )
    assert rc == 0
    assert captured["in"] == glb
    assert captured["out"] == out
    assert captured["version"] == "meshy-5"
    assert captured["plan"].renames == {"Hips": "Hips", "Spine": "Spine"}


def test_convert_rig_echoes_each_resolved_merge_pair(tmp_path, capsys, monkeypatch):
    # A silent merge leaves no audit trace (PR #523 review, item 3): every
    # resolved helper → target pair must be echoed BEFORE the Blender pass.
    glb = _make_skinned_glb(
        tmp_path / "rig.glb", ["Hips", "LeftForeArm", "LeftForeArmTwist"]
    )
    monkeypatch.setattr(meshy_main, "_run_blender_conversion", lambda *a, **k: 0)
    rc = meshy_main.main(
        [
            "convert-rig",
            str(glb),
            "--meshy-version",
            "meshy-5",
            "--out",
            str(tmp_path / "out.glb"),
            "--allow-unverified-map",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "LeftForeArmTwist -> LeftLowerArm" in out


def test_convert_rig_load_plan_reads_renames_and_merges(tmp_path):
    plan_json = tmp_path / "plan.json"
    plan_json.write_text(
        json.dumps(
            {
                "renames": {"LeftArm": "LeftUpperArm"},
                "merges": {"LeftArmTwist": "LeftUpperArm"},
            }
        ),
        encoding="utf-8",
    )
    renames, merges = convert_rig.load_plan(plan_json)
    assert renames == {"LeftArm": "LeftUpperArm"}
    assert merges == {"LeftArmTwist": "LeftUpperArm"}


def test_convert_rig_parse_args_requires_in_out_plan():
    args = convert_rig.parse_args(
        ["--in", "a.glb", "--out", "b.glb", "--plan-json", "p.json"]
    )
    assert args.input == "a.glb"
    assert args.out == "b.glb"
    assert args.plan_json == "p.json"


def test_convert_rig_argv_after_ddash_splits_on_separator():
    assert convert_rig.argv_after_ddash(["blender", "--", "--in", "x"]) == ["--in", "x"]
    assert convert_rig.argv_after_ddash(["blender", "--background"]) == []


def test_convert_rig_refuses_missing_input_glb(tmp_path, capsys):
    rc = meshy_main.main(
        [
            "convert-rig",
            str(tmp_path / "nope.glb"),
            "--meshy-version",
            "meshy-5",
            "--out",
            str(tmp_path / "out.glb"),
        ]
    )
    assert rc == 2
    assert "nope.glb" in capsys.readouterr().err


# ============================================================================
# Fixture gate — the committed CONVERTED fixture .glb binds only canonical
# bones (I021), invoking the Task-4 checker directly. Blender produced the
# fixture locally (BLOCKED without it); CI validates the artifact structurally.
# On LFS-pointer checkouts (no smudge) the test skips, matching the rig tests.
# ============================================================================

FIXTURE_DIR = REPO / "tests" / "fixtures" / "meshy"
CONVERTED_FIXTURE = FIXTURE_DIR / "canonical_rig_output.glb"


def _is_real_glb(path: Path) -> bool:
    """True for a smudged binary .glb; False for a Git-LFS pointer stub."""
    if not path.is_file():
        return False
    with path.open("rb") as fh:
        head = fh.read(8)
    return head[:4] == b"glTF"


@pytest.mark.skipif(
    not _is_real_glb(CONVERTED_FIXTURE),
    reason="converted fixture .glb absent or an unsmudged LFS pointer",
)
def test_converted_fixture_binds_only_canonical_bones_I021():
    skeleton_defs = validate_imports.load_skeleton_defs(
        REPO / "schema" / "content" / "skeleton.defs.yaml"
    )
    # The converted fixture is an armor_model gear piece: I021 requires its skin
    # bind only canonical bones (a subset is fine). lod_policy 'single' silences
    # the I023 LOD-chain rule for this single-mesh fixture.
    doc = {
        "class": "armor_model",
        "source": "assets/art/char/canonical_rig_output.glb",
        "import_hints": {"lod_policy": "single"},
    }
    errors = validate_imports.check_gltf_rig(
        doc,
        Path("fixtures/meshy/canonical_rig_output.asset.yaml"),
        CONVERTED_FIXTURE,
        skeleton_defs,
    )
    assert errors == [], errors

    # And directly: every bound joint is a canonical bone name.
    from pygltflib import GLTF2

    gltf = GLTF2().load(str(CONVERTED_FIXTURE))
    joints = validate_imports._joint_names(gltf)
    assert joints  # the fixture really is skinned
    assert joints <= set(bones.bone_names())


# ============================================================================
# Weight-mass conservation gate (PR #523 review, items 1+2): the input fixture
# carries a MULTI-influence twist vertex set (0.6 LeftForeArmTwist + 0.4
# LeftForeArm); after conversion every one of those vertices must hold its full
# 1.0 on LeftLowerArm — the two-pass numeric merge conserved the mass, no bpy
# iterator hazard, no ADD-mode clamp distortion.
# ============================================================================

INPUT_FIXTURE = FIXTURE_DIR / "meshy_rig_input.glb"


def _decode_skin_vertex_weights(glb_path: Path) -> list[list[tuple[str, float]]]:
    """Per-vertex [(joint_name, weight), ...] decoded from JOINTS_0/WEIGHTS_0."""
    from pygltflib import GLTF2

    gltf = GLTF2().load(str(glb_path))
    blob = gltf.binary_blob()
    prim = gltf.meshes[0].primitives[0]
    assert prim.attributes.JOINTS_0 is not None
    assert getattr(prim.attributes, "JOINTS_1", None) is None  # ≤4 influences
    joint_node_names = [gltf.nodes[j].name for j in gltf.skins[0].joints]

    def _read(accessor_index: int, fmt: str, comps: int):
        acc = gltf.accessors[accessor_index]
        bv = gltf.bufferViews[acc.bufferView]
        start = (bv.byteOffset or 0) + (acc.byteOffset or 0)
        n = acc.count * comps
        return struct.unpack_from(f"<{n}{fmt}", blob, start), acc.count

    jacc = gltf.accessors[prim.attributes.JOINTS_0]
    jfmt = {5121: "B", 5123: "H", 5125: "I"}[jacc.componentType]
    joints_flat, vcount = _read(prim.attributes.JOINTS_0, jfmt, 4)
    wacc = gltf.accessors[prim.attributes.WEIGHTS_0]
    assert wacc.componentType == 5126  # float weights (Blender export)
    weights_flat, _ = _read(prim.attributes.WEIGHTS_0, "f", 4)

    verts: list[list[tuple[str, float]]] = []
    for i in range(vcount):
        entries = [
            (joint_node_names[joints_flat[i * 4 + k]], weights_flat[i * 4 + k])
            for k in range(4)
            if weights_flat[i * 4 + k] > 0.0
        ]
        verts.append(entries)
    return verts


@pytest.mark.skipif(
    not _is_real_glb(INPUT_FIXTURE),
    reason="input fixture .glb absent or an unsmudged LFS pointer",
)
def test_input_fixture_has_multi_influence_twist_vertices():
    verts = _decode_skin_vertex_weights(INPUT_FIXTURE)
    twist_verts = [
        dict(v) for v in verts if any(name == "LeftForeArmTwist" for name, _ in v)
    ]
    assert twist_verts, "fixture lost its twist-weighted vertices"
    for v in twist_verts:
        # The multi-influence split that exercises the numeric two-pass merge.
        assert v["LeftForeArmTwist"] == pytest.approx(0.6, abs=1e-3)
        assert v["LeftForeArm"] == pytest.approx(0.4, abs=1e-3)


@pytest.mark.skipif(
    not (_is_real_glb(INPUT_FIXTURE) and _is_real_glb(CONVERTED_FIXTURE)),
    reason="fixture .glbs absent or unsmudged LFS pointers",
)
def test_converted_fixture_conserves_weight_mass():
    in_verts = _decode_skin_vertex_weights(INPUT_FIXTURE)
    out_verts = _decode_skin_vertex_weights(CONVERTED_FIXTURE)

    # Every vertex still sums to 1.0 (≤4 influences asserted in the decoder).
    for v in out_verts:
        assert sum(w for _, w in v) == pytest.approx(1.0, abs=1e-3)

    # No trace of the helper; only the expected canonical bones carry weight.
    weighted = {name for v in out_verts for name, _ in v}
    assert weighted == {
        "Hips",
        "Spine",
        "Chest",
        "LeftUpperArm",
        "LeftLowerArm",
        "LeftHand",
    }

    # Mass conservation per bone: the twist verts' 0.6 + their 0.4 LeftForeArm
    # share and the forearm box's own 1.0s all land on LeftLowerArm. Compare
    # per-joint total mass input→output under the plan's rename/merge targets.
    plan_obj = mapping.plan(mapping.joint_names_from_glb(INPUT_FIXTURE), "meshy-5")
    target_of = {**plan_obj.renames, **plan_obj.merges}
    in_mass: dict[str, float] = {}
    for v in in_verts:
        for name, w in v:
            in_mass[target_of[name]] = in_mass.get(target_of[name], 0.0) + w
    out_mass: dict[str, float] = {}
    for v in out_verts:
        for name, w in v:
            out_mass[name] = out_mass.get(name, 0.0) + w
    # Blender splits vertices per flat-shaded face on export (same factor for
    # both fixtures — identical geometry), so compare RELATIVE mass shares.
    in_total = sum(in_mass.values())
    out_total = sum(out_mass.values())
    for bone_name, mass in in_mass.items():
        assert out_mass[bone_name] / out_total == pytest.approx(
            mass / in_total, abs=1e-3
        ), f"weight mass shifted for {bone_name}"


# ============================================================================
# rig() / animate() client + `meshy rig` / `meshy animate` CLI (story #916).
# HTTP is fully mocked. Response shapes mirror docs.meshy.ai/en/api/rigging and
# /animation (fetched 2026-07-17): SUCCEEDED bodies nest the finished glb URLs
# under a `result` object (result.rigged_character_glb_url /
# result.animation_glb_url / result.basic_animations.<clip>_glb_url), NOT the
# top-level `model_urls["glb"]` slot text/image-to-3d use.
# ============================================================================

RIG_GLB_URL = "https://assets.meshy.ai/rigged.glb"
WALK_GLB_URL = "https://assets.meshy.ai/walk.glb"
RUN_GLB_URL = "https://assets.meshy.ai/run.glb"
ANIM_GLB_URL = "https://assets.meshy.ai/anim.glb"


def _rig_handler(*, glb_bytes: bytes, status="SUCCEEDED"):
    """MockTransport handler for the rigging create -> poll -> download flow."""
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        calls.append(request)
        path = request.url.path
        if request.method == "POST" and path == client_mod.RIGGING_PATH:
            return httpx.Response(200, json={"result": "task_rig_1"})
        if request.method == "GET" and path == f"{client_mod.RIGGING_PATH}/task_rig_1":
            return httpx.Response(
                200,
                json={
                    "id": "task_rig_1",
                    "type": "rig",
                    "status": status,
                    "progress": 100,
                    "result": {
                        "rigged_character_glb_url": RIG_GLB_URL,
                        "rigged_character_fbx_url": "https://assets.meshy.ai/rig.fbx",
                        "basic_animations": {
                            "walking_glb_url": WALK_GLB_URL,
                            "running_glb_url": RUN_GLB_URL,
                        },
                    },
                },
            )
        if request.method == "GET" and str(request.url) in (
            RIG_GLB_URL,
            WALK_GLB_URL,
            RUN_GLB_URL,
        ):
            return httpx.Response(200, content=glb_bytes)
        raise AssertionError(f"unexpected request: {request.method} {request.url}")

    return handler, calls


def _animate_handler(*, glb_bytes: bytes, status="SUCCEEDED"):
    """MockTransport handler for the animation create -> poll -> download flow."""
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        calls.append(request)
        path = request.url.path
        if request.method == "POST" and path == client_mod.ANIMATION_PATH:
            return httpx.Response(200, json={"result": "task_anim_1"})
        if (
            request.method == "GET"
            and path == f"{client_mod.ANIMATION_PATH}/task_anim_1"
        ):
            return httpx.Response(
                200,
                json={
                    "id": "task_anim_1",
                    "type": "animate",
                    "status": status,
                    "progress": 100,
                    "result": {
                        "animation_glb_url": ANIM_GLB_URL,
                        "animation_fbx_url": "https://assets.meshy.ai/anim.fbx",
                    },
                },
            )
        if request.method == "GET" and str(request.url) == ANIM_GLB_URL:
            return httpx.Response(200, content=glb_bytes)
        raise AssertionError(f"unexpected request: {request.method} {request.url}")

    return handler, calls


# --- client.py: rig() / animate() / download_url() -------------------------


def test_rig_submits_model_url_and_returns_rig_handle():
    handler, calls = _rig_handler(glb_bytes=b"glb")
    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    handle = client.rig(model_url="https://example.com/mesh.glb", height_meters=1.8)
    assert handle.task_id == "task_rig_1"
    assert handle.mode == "rig"
    post = next(c for c in calls if c.method == "POST")
    body = json.loads(post.content)
    assert body["model_url"] == "https://example.com/mesh.glb"
    assert body["height_meters"] == 1.8
    assert "input_task_id" not in body


def test_rig_submits_input_task_id():
    handler, calls = _rig_handler(glb_bytes=b"glb")
    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    client.rig(input_task_id="gen_task_9")
    body = json.loads(next(c for c in calls if c.method == "POST").content)
    assert body["input_task_id"] == "gen_task_9"
    assert "model_url" not in body


def test_rig_requires_exactly_one_source():
    client = client_mod.MeshyClient(
        "fake-key", transport=httpx.MockTransport(lambda r: httpx.Response(200))
    )
    with pytest.raises(ValueError, match="exactly one"):
        client.rig()
    with pytest.raises(ValueError, match="exactly one"):
        client.rig(input_task_id="x", model_url="y")


def test_rig_poll_exposes_result_urls_not_model_urls():
    handler, _calls = _rig_handler(glb_bytes=b"glb")
    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    status = client.poll(
        "task_rig_1", endpoint=client_mod.RIGGING_PATH, interval_s=0, timeout_s=5
    )
    assert status.succeeded
    assert status.model_urls == {}  # rigging carries NO top-level model_urls
    assert client.rigged_glb_url(status) == RIG_GLB_URL
    assert client.basic_animation_urls(status) == {
        "walking_glb_url": WALK_GLB_URL,
        "running_glb_url": RUN_GLB_URL,
    }


def test_animate_submits_rig_task_and_action_id():
    handler, calls = _animate_handler(glb_bytes=b"glb")
    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    handle = client.animate("task_rig_1", 466, post_process={"fps": 30})
    assert handle.task_id == "task_anim_1"
    assert handle.mode == "animate"
    body = json.loads(next(c for c in calls if c.method == "POST").content)
    assert body["rig_task_id"] == "task_rig_1"
    assert body["action_id"] == 466
    assert body["post_process"] == {"fps": 30}


def test_animate_poll_exposes_animation_glb_url():
    handler, _calls = _animate_handler(glb_bytes=b"glb")
    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    status = client.poll(
        "task_anim_1", endpoint=client_mod.ANIMATION_PATH, interval_s=0, timeout_s=5
    )
    assert client.animation_glb_url(status) == ANIM_GLB_URL


def test_download_url_writes_bytes_from_arbitrary_url(tmp_path):
    payload = b"rigged-glb-bytes"

    def handler(request: httpx.Request) -> httpx.Response:
        assert str(request.url) == RIG_GLB_URL
        return httpx.Response(200, content=payload)

    client = client_mod.MeshyClient("fake-key", transport=httpx.MockTransport(handler))
    dest = tmp_path / "nested" / "out.glb"
    result = client.download_url(RIG_GLB_URL, dest)
    assert result == dest
    assert dest.read_bytes() == payload


def test_download_url_rejects_empty_url(tmp_path):
    client = client_mod.MeshyClient(
        "fake-key", transport=httpx.MockTransport(lambda r: httpx.Response(200))
    )
    with pytest.raises(client_mod.MeshyAPIError):
        client.download_url("", tmp_path / "x.glb")


# --- CLI: `meshy rig` ------------------------------------------------------


def test_rig_cli_happy_path_downloads_glb_and_prints_summary(
    tmp_path, monkeypatch, capsys
):
    handler, _calls = _rig_handler(glb_bytes=b"rigged")
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    out = tmp_path / "quarantine" / "rigged.glb"
    rc = meshy_main.main(
        [
            "rig",
            "--model-url",
            "https://example.com/mesh.glb",
            "--out",
            str(out),
            "--terms-verified",
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 0
    assert out.read_bytes() == b"rigged"
    summary = json.loads(capsys.readouterr().out)
    assert summary["task_id"] == "task_rig_1"
    assert summary["status"] == "SUCCEEDED"
    assert "rigged_character_glb_url" in summary["result_keys"]


def test_rig_cli_downloads_basic_animations(tmp_path, monkeypatch):
    handler, _calls = _rig_handler(glb_bytes=b"rigged")
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    anim_dir = tmp_path / "clips"
    rc = meshy_main.main(
        [
            "rig",
            "--model-url",
            "https://example.com/mesh.glb",
            "--out",
            str(tmp_path / "rigged.glb"),
            "--basic-animations-dir",
            str(anim_dir),
            "--terms-verified",
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 0
    assert (anim_dir / "walking.glb").read_bytes() == b"rigged"
    assert (anim_dir / "running.glb").read_bytes() == b"rigged"


def test_rig_cli_refuses_without_terms_verified(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call without --terms-verified")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "rig",
            "--model-url",
            "https://example.com/mesh.glb",
            "--out",
            str(tmp_path / "out.glb"),
        ]
    )
    assert rc == 2
    assert "TD-09" in capsys.readouterr().err


def test_rig_cli_refuses_without_api_key(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call without MESHY_API_KEY")

    _patch_client(monkeypatch, handler)
    monkeypatch.delenv("MESHY_API_KEY", raising=False)
    rc = meshy_main.main(
        [
            "rig",
            "--model-url",
            "https://example.com/mesh.glb",
            "--out",
            str(tmp_path / "out.glb"),
            "--terms-verified",
        ]
    )
    assert rc == 2
    assert "MESHY_API_KEY" in capsys.readouterr().err


def test_rig_cli_refuses_latest_model_version(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call for a 'latest' pin")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "rig",
            "--model-url",
            "https://example.com/mesh.glb",
            "--out",
            str(tmp_path / "out.glb"),
            "--terms-verified",
            "--model-version",
            "latest",
        ]
    )
    assert rc == 2
    assert "latest" in capsys.readouterr().err


def test_rig_cli_nonzero_exit_when_task_fails(tmp_path, monkeypatch):
    handler, _calls = _rig_handler(glb_bytes=b"x", status="FAILED")
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "rig",
            "--model-url",
            "https://example.com/mesh.glb",
            "--out",
            str(tmp_path / "out.glb"),
            "--terms-verified",
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 1


# --- CLI: `meshy animate` --------------------------------------------------


def test_animate_cli_happy_path_downloads_glb(tmp_path, monkeypatch, capsys):
    handler, _calls = _animate_handler(glb_bytes=b"anim")
    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    out = tmp_path / "quarantine" / "idle.glb"
    rc = meshy_main.main(
        [
            "animate",
            "--rig-task-id",
            "task_rig_1",
            "--action-id",
            "0",
            "--out",
            str(out),
            "--terms-verified",
            "--poll-interval",
            "0",
            "--poll-timeout",
            "5",
        ]
    )
    assert rc == 0
    assert out.read_bytes() == b"anim"
    summary = json.loads(capsys.readouterr().out)
    assert summary["task_id"] == "task_anim_1"
    assert summary["action_id"] == 0
    assert "animation_glb_url" in summary["result_keys"]


def test_animate_cli_refuses_without_terms_verified(tmp_path, monkeypatch, capsys):
    def handler(request: httpx.Request) -> httpx.Response:
        raise AssertionError("no HTTP call without --terms-verified")

    _patch_client(monkeypatch, handler)
    monkeypatch.setenv("MESHY_API_KEY", "fake-key")
    rc = meshy_main.main(
        [
            "animate",
            "--rig-task-id",
            "task_rig_1",
            "--action-id",
            "0",
            "--out",
            str(tmp_path / "out.glb"),
        ]
    )
    assert rc == 2
    assert "TD-09" in capsys.readouterr().err
