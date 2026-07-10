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
import validate_content  # noqa: E402
from validate_content import check_provenance as ci_check_provenance  # noqa: E402


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
# __main__.py — CLI end-to-end (in-process; MeshyClient construction is
# monkeypatched to inject the mock transport — see meshy_main._new_client)
# ============================================================================


def _patch_client(monkeypatch, handler):
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(
        meshy_main,
        "_new_client",
        lambda api_key: client_mod.MeshyClient(api_key, transport=transport),
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
    # the real on-disk tree — exactly what CI runs — must be error-free.
    res = validate_content.validate(
        content_root,
        SCHEMA_DIR,
        assets_mode="error",
        imports_mode="error",
        presets_path=REPO / "client" / "import-presets" / "presets.json",
    )
    assert res.errors == [], res.errors
