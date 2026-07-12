"""Tests for the tools/art/ deterministic asset generators.

- generate_face_skin.py (story #568, ⑤/S1): 7 procedural face/skin PNGs, valid +
  within the texture cap, byte-identical across runs.
- generate_warden_kit.py (story #569, ⑤/S2): 6 skinned armor plates + RGB dye
  masks — I021 bind-conformance per plate (via the real import validator, no
  Blender), the Art PRD §2.1 budget band (3-8k per piece, ≤40k outfit), mask
  presence/validity, ≤4 normalized influences, and byte-determinism.

Every tools/art/ generator makes the same determinism guarantee (mirrors
generate_pickaxe_blockout.py).
"""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "art"))

import generate_face_skin as gfs  # noqa: E402
from tools.art import generate_warden_kit as gwk  # noqa: E402
from validate_imports import check_gltf_rig, load_skeleton_defs  # noqa: E402

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
# Art PRD §2.3 "Player character body" texture cap, mirrored in
# client/import-presets/presets.json size_caps_px.texture_set and
# tools/validate_content.py ASSET_BUDGETS["texture_set"].
TEXTURE_CAP_PX = 2048

EXPECTED_FILES = sorted(
    [f"face_{i}_bc.png" for i in range(1, 5)]
    + [f"skin_{i}_bc.png" for i in range(1, 4)]
)

SKELETON_DEFS = REPO / "schema" / "content" / "skeleton.defs.yaml"
SLOTS = list(gwk.SLOTS)


# --------------------------------------------------------------------------- #
# S1 — generate_face_skin.py
# --------------------------------------------------------------------------- #
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


# --------------------------------------------------------------------------- #
# S2 — generate_warden_kit.py
# --------------------------------------------------------------------------- #
@pytest.mark.unit
class TestPlateBudgets:
    """Art PRD §2.1: armor set piece 3-8k per slot; full outfit ≤ 40k LOD0."""

    @pytest.mark.parametrize("slot", SLOTS)
    def test_plate_tris_in_band(self, slot):
        tris = gwk.plate_tri_count(slot)
        assert 3_000 <= tris <= 8_000, f"{slot}: {tris} tris outside 3-8k band"

    def test_outfit_lod0_under_40k(self):
        total = sum(gwk.plate_tri_count(s) for s in SLOTS)
        assert total <= 40_000, f"outfit LOD0 total {total} exceeds 40k"

    def test_six_slots(self):
        assert set(SLOTS) == {"head", "shoulders", "chest", "hands", "legs", "feet"}


@pytest.mark.unit
class TestPlateLocality:
    """Each armor VOLUME is anatomy-scale and localized to its bone(s).

    Regression for the invisible/rail hands bug (#569): the hands piece must be two
    compact hand-scale gloves at the hand bones — not one wide bar spanning the whole
    T-pose arm span. A plate's COMBINED bbox can be wide (two hands sit ~1.6 m apart
    in the T-pose), but no single volume may be, and paired plates must be disjoint.
    """

    # Per-anatomy volume cap (metres): the largest allowed extent of ONE shell.
    # Hands/feet are small; a leg guard legitimately runs the length of the leg.
    _SHELL_CAP = {
        "head": 0.30, "shoulders": 0.30, "chest": 0.50,
        "hands": 0.25, "legs": 0.95, "feet": 0.40,
    }

    @pytest.mark.parametrize("slot", SLOTS)
    def test_each_shell_is_anatomy_scale(self, slot):
        cap = self._SHELL_CAP[slot]
        for i, ext in enumerate(gwk.shell_extents(slot)):
            assert max(ext) <= cap, f"{slot} shell {i} extent {ext} exceeds {cap} m"

    def test_hands_gloves_are_hand_scale(self):
        # The explicit #569 acceptance: each glove ≤ 0.25 m (hand-scale, not arm-span).
        extents = gwk.shell_extents("hands")
        assert len(extents) == 2
        for ext in extents:
            assert max(ext) <= 0.25, f"glove extent {ext} is not hand-scale"

    def test_hands_are_two_disjoint_gloves_not_a_bar(self):
        # The invisible-kit bug rendered as one bar across the arm span. The two
        # gloves must be far out on the arms with a wide empty gap across the torso —
        # NO geometry anywhere near the centreline (|x| < 0.4 m).
        from tools.art.generate_warden_kit import build_shells

        pos = build_shells(gwk.SLOTS["hands"])[0]
        near_mid = [p for p in pos if abs(p[0]) < 0.4]
        assert near_mid == [], f"hands has {len(near_mid)} verts near centre — a bar, not gloves"
        # And each glove actually sits over its hand bone (~0.72-0.82 m out).
        assert all(0.6 < abs(p[0]) < 0.9 for p in pos)

    def test_hands_each_glove_skinned_to_its_own_hand_bone(self):
        # Left glove → LeftHand, right glove → RightHand (single influence, no collapse).
        from tools.art.generate_warden_kit import build_shells

        pos, _n, _i, vjoints, used = build_shells(gwk.SLOTS["hands"])
        assert set(used) == {"LeftHand", "RightHand"}
        for p, bone in zip(pos, vjoints):
            expected = "RightHand" if p[0] > 0 else "LeftHand"
            assert bone == expected, f"vertex {p} bound to {bone}, expected {expected}"


@pytest.mark.unit
class TestPlateRig:
    """Each plate is skinned and binds ONLY canonical bones (import validator I021)."""

    @pytest.mark.parametrize("slot", SLOTS)
    def test_i021_binds_only_canonical_bones(self, slot, tmp_path):
        glb = tmp_path / f"warden_{slot}.glb"
        glb.write_bytes(gwk.build_plate_glb(slot))
        defs = load_skeleton_defs(SKELETON_DEFS)
        doc = {"class": "armor_model", "import_hints": {"lod_policy": "single"}}
        errors = check_gltf_rig(doc, Path(f"warden_{slot}.glb"), glb, defs)
        assert errors == [], errors

    @pytest.mark.parametrize("slot", SLOTS)
    def test_plate_is_skinned(self, slot):
        from pygltflib import GLTF2

        gltf = GLTF2().load_from_bytes(gwk.build_plate_glb(slot))
        assert gltf.skins, f"{slot} plate has no skin"
        # Every skin joint node carries a canonical bone name.
        canon = set(load_skeleton_defs(SKELETON_DEFS)["bones"])
        for skin in gltf.skins:
            for j in skin.joints:
                assert gltf.nodes[j].name in canon


@pytest.mark.unit
class TestPlateSkinWeights:
    """Skin influences ≤ 4, normalized (Art PRD §2.1 / Global Constraints)."""

    @pytest.mark.parametrize("slot", SLOTS)
    def test_weights_normalized_single_influence(self, slot):
        from pygltflib import GLTF2

        data = gwk.build_plate_glb(slot)
        gltf = GLTF2().load_from_bytes(data)
        blob = gltf.binary_blob()
        prim = gltf.meshes[0].primitives[0]
        acc = gltf.accessors[prim.attributes.WEIGHTS_0]
        bv = gltf.bufferViews[acc.bufferView]
        off = bv.byteOffset or 0
        for i in range(acc.count):
            w = struct.unpack_from("<4f", blob, off + i * 16)
            assert abs(sum(w) - 1.0) < 1e-6, f"{slot} vertex {i} weights {w}"
            assert sum(1 for x in w if x > 0) <= 4


@pytest.mark.unit
class TestDyeMasks:
    """Each dyeable piece ships one RGB dye mask (contract ① §6)."""

    @pytest.mark.parametrize("slot", SLOTS)
    def test_mask_is_valid_png(self, slot):
        png = gwk.build_mask_png(slot)
        assert png[:8] == PNG_MAGIC, f"{slot} mask not a PNG"
        # IHDR declares an RGB (color type 2) image within the 2048² armor cap.
        w, h, _bits, color_type = struct.unpack_from(">IIBB", png, 16)
        assert color_type == 2, f"{slot} mask not RGB"
        assert 0 < w <= 2048 and 0 < h <= 2048

    def test_one_mask_per_slot(self):
        masks = {s: gwk.build_mask_png(s) for s in SLOTS}
        assert len(masks) == 6
        # Masks are per-slot distinct (not a single shared texture).
        assert len(set(masks.values())) == 6


@pytest.mark.unit
class TestDeterminism:
    """Byte-identical regeneration (scripted-original determinism, spec §S2)."""

    @pytest.mark.parametrize("slot", SLOTS)
    def test_plate_glb_stable(self, slot):
        a = hashlib.sha256(gwk.build_plate_glb(slot)).hexdigest()
        b = hashlib.sha256(gwk.build_plate_glb(slot)).hexdigest()
        assert a == b

    @pytest.mark.parametrize("slot", SLOTS)
    def test_mask_png_stable(self, slot):
        a = hashlib.sha256(gwk.build_mask_png(slot)).hexdigest()
        b = hashlib.sha256(gwk.build_mask_png(slot)).hexdigest()
        assert a == b


@pytest.mark.unit
class TestGeneratorMain:
    """The CLI writes all twelve artifacts to the target dir."""

    def test_main_writes_all_artifacts(self, tmp_path):
        rc = gwk.main(["generate_warden_kit.py", str(tmp_path)])
        assert rc == 0
        for slot in SLOTS:
            assert (tmp_path / f"warden_{slot}.glb").is_file()
            assert (tmp_path / f"warden_{slot}_mask.png").is_file()
