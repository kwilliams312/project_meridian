"""Tests for tools/art/generate_face_skin.py (story #568, ⑤/S1).

Covers the three failing-test requirements from the plan: the CLI produces the
7 expected PNGs, each is a structurally valid PNG within the Art PRD §2.3 /
client/import-presets/presets.json `texture_set` size cap (2048px), and two
runs are byte-identical (SHA-256 stable) — the determinism guarantee every
`tools/art/` generator makes (mirrors `generate_pickaxe_blockout.py`).
"""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "art"))

import generate_face_skin as gfs  # noqa: E402

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
# Art PRD §2.3 "Player character body" texture cap, mirrored in
# client/import-presets/presets.json size_caps_px.texture_set and
# tools/validate_content.py ASSET_BUDGETS["texture_set"].
TEXTURE_CAP_PX = 2048

EXPECTED_FILES = sorted(
    [f"face_{i}_bc.png" for i in range(1, 5)]
    + [f"skin_{i}_bc.png" for i in range(1, 4)]
)


def _ihdr_dims(data: bytes) -> tuple[int, int]:
    assert data[:8] == PNG_MAGIC, "missing PNG magic bytes"
    length, tag = struct.unpack(">I4s", data[8:16])
    assert tag == b"IHDR", "first chunk after signature must be IHDR"
    width, height = struct.unpack(">II", data[16:24])
    return width, height


def test_generate_produces_seven_pngs(tmp_path: Path) -> None:
    assert gfs.main(["--out", str(tmp_path)]) == 0
    files = sorted(p.name for p in tmp_path.glob("*.png"))
    assert files == EXPECTED_FILES


def test_generated_pngs_are_valid_and_within_texture_cap(tmp_path: Path) -> None:
    gfs.main(["--out", str(tmp_path)])
    for name in EXPECTED_FILES:
        data = (tmp_path / name).read_bytes()
        width, height = _ihdr_dims(data)
        assert 0 < width <= TEXTURE_CAP_PX, f"{name} width {width} exceeds cap"
        assert 0 < height <= TEXTURE_CAP_PX, f"{name} height {height} exceeds cap"


def test_rerun_is_byte_identical(tmp_path: Path) -> None:
    """Determinism: SHA-256 of every output file matches across two runs."""
    out1, out2 = tmp_path / "run1", tmp_path / "run2"
    gfs.main(["--out", str(out1)])
    gfs.main(["--out", str(out2)])
    for name in EXPECTED_FILES:
        b1 = (out1 / name).read_bytes()
        b2 = (out2 / name).read_bytes()
        assert hashlib.sha256(b1).hexdigest() == hashlib.sha256(b2).hexdigest(), name
        assert b1 == b2


def test_face_variants_are_distinct() -> None:
    """Faces carry subtle per-variant feature variation, not 4 identical copies."""
    hashes = {hashlib.sha256(gfs.build_face_png(v)).hexdigest() for v in range(1, 5)}
    assert len(hashes) == 4


def test_skin_variants_are_distinct() -> None:
    """Skin palettes are distinct tone ramps, not 3 identical copies."""
    hashes = {hashlib.sha256(gfs.build_skin_png(v)).hexdigest() for v in range(1, 4)}
    assert len(hashes) == 3


def test_main_requires_out_argument() -> None:
    with pytest.raises(SystemExit):
        gfs.main([])


def test_face_and_skin_dims_match_module_constants() -> None:
    face = gfs.build_face_png(1)
    skin = gfs.build_skin_png(1)
    assert _ihdr_dims(face) == (gfs.WIDTH, gfs.HEIGHT)
    assert _ihdr_dims(skin) == (gfs.WIDTH, gfs.HEIGHT)
