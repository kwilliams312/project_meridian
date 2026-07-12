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
    # Conforming shells (#589) hug the REAL body surface, so a shell is as wide as
    # the body region it covers: the torso cuirass spans the ~0.51 m shoulder
    # width, a boot the shin+foot length. Caps are the body region extent + a
    # small conform margin, not the old greybox-box sizes.
    _SHELL_CAP = {
        "head": 0.42, "shoulders": 0.32, "chest": 0.60,
        "hands": 0.35, "legs": 0.55, "feet": 0.65,
    }

    @pytest.mark.parametrize("slot", SLOTS)
    def test_each_shell_is_anatomy_scale(self, slot):
        cap = self._SHELL_CAP[slot]
        for i, ext in enumerate(gwk.shell_extents(slot)):
            assert max(ext) <= cap, f"{slot} shell {i} extent {ext} exceeds {cap} m"

    def test_hands_gloves_are_hand_scale(self):
        # Each glove wraps the hand+wrist along the arm — hand-scale (≤ 0.35 m),
        # NOT one bar across the whole ~1.6 m T-pose arm span (#569 regression).
        extents = gwk.shell_extents("hands")
        assert len(extents) == 2
        for ext in extents:
            assert max(ext) <= 0.35, f"glove extent {ext} is not hand-scale"

    def test_hands_are_two_disjoint_gloves_not_a_bar(self):
        # The invisible-kit bug rendered as one bar across the arm span. The two
        # gloves must be far out on the arms with a wide empty gap across the torso —
        # NO geometry anywhere near the centreline (|x| < 0.4 m).
        from tools.art.generate_warden_kit import build_shells

        pos = build_shells(gwk.SLOTS["hands"])[0]
        near_mid = [p for p in pos if abs(p[0]) < 0.4]
        assert near_mid == [], f"hands has {len(near_mid)} verts near centre — a bar, not gloves"
        # And each glove actually sits far out over its hand (the hands geoset runs
        # ~0.6-0.9 m out; the conform margin widens that band slightly).
        assert all(0.5 < abs(p[0]) < 1.0 for p in pos)

    def test_hands_each_glove_skinned_to_its_own_hand_bone(self):
        # Left glove → LeftHand, right glove → RightHand (single influence, no collapse).
        from tools.art.generate_warden_kit import build_shells

        pos, _n, _i, vjoints, used, _uv = build_shells(gwk.SLOTS["hands"])
        assert set(used) == {"LeftHand", "RightHand"}
        for p, bone in zip(pos, vjoints):
            expected = "RightHand" if p[0] > 0 else "LeftHand"
            assert bone == expected, f"vertex {p} bound to {bone}, expected {expected}"


@pytest.mark.unit
class TestPlateConformsToBody:
    """Issue #589: plates conform to the real body so hiding a region leaves NO void.

    The ⑤/S6 render showed blocky box plates that were NARROWER than the body
    region their item ``worn.hides`` erases; the gap between the box edge and the
    (now-hidden) body surface read as black voids. The fix lofts each plate to the
    real body geoset surface (sampled from the committed base .glb) pushed out by a
    small margin, so the plate's bounding volume fully CONTAINS the body region it
    hides — the plate occupies exactly where the body was, no gap. This test is the
    by-construction guarantee: for every plate that hides a body region, the plate
    AABB must contain that region's AABB on all three axes.
    """

    # Each hiding plate -> the body geoset LOD0 mesh(es) its worn.hides erases.
    # (Shoulders hides nothing after #589 — the pauldrons layer on top of the body.)
    _HIDDEN_REGION_MESHES = {
        "head": ["geo_head_lod0"],
        "chest": ["geo_torso_lod0"],
        "legs": ["geo_hips_legs_lod0"],
        "feet": ["geo_lower_legs_lod0", "geo_feet_lod0"],
        "hands": ["geo_hands_lod0"],
    }

    @pytest.mark.parametrize("slot", sorted(_HIDDEN_REGION_MESHES))
    def test_plate_aabb_covers_hidden_body_region(self, slot):
        meshes = self._HIDDEN_REGION_MESHES[slot]
        body = [v for m in meshes for v in gwk._read_body_positions(m)]
        b_lo = [min(v[a] for v in body) for a in range(3)]
        b_hi = [max(v[a] for v in body) for a in range(3)]
        plate = gwk.build_shells(gwk.SLOTS[slot])[0]
        p_lo = [min(v[a] for v in plate) for a in range(3)]
        p_hi = [max(v[a] for v in plate) for a in range(3)]
        for a, name in enumerate("xyz"):
            assert p_lo[a] <= b_lo[a] + 1e-6, (
                f"{slot} plate {name}-min {p_lo[a]:.4f} does not cover body "
                f"{b_lo[a]:.4f} — a void opens on the {name}- side"
            )
            assert p_hi[a] >= b_hi[a] - 1e-6, (
                f"{slot} plate {name}-max {p_hi[a]:.4f} does not cover body "
                f"{b_hi[a]:.4f} — a void opens on the {name}+ side"
            )

    def test_shoulders_conform_but_do_not_hide_torso(self):
        # Pauldrons are deltoid caps sampled from the upper-outer torso; they sit
        # over the shoulder region (~y≥1.33) and never reach the torso centreline.
        for shell in gwk.SLOTS["shoulders"]:
            assert shell.y_min is not None and shell.y_min >= 1.3
        pos = gwk.build_shells(gwk.SLOTS["shoulders"])[0]
        assert min(p[1] for p in pos) >= 1.25, "pauldron dips below the shoulder"


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


def _decode_mask_pixels(png: bytes) -> list[tuple[int, int, int]]:
    """Decode a build_mask_png RGB PNG (filter 0 rows) into a flat pixel list."""
    import zlib

    w, h, _bits, ctype = struct.unpack_from(">IIBB", png, 16)
    assert ctype == 2  # RGB
    # Concatenate IDAT chunk payloads, then inflate.
    idat = b""
    off = 8
    while off < len(png):
        length = struct.unpack_from(">I", png, off)[0]
        tag = png[off + 4:off + 8]
        if tag == b"IDAT":
            idat += png[off + 8:off + 8 + length]
        off += 12 + length
    raw = zlib.decompress(idat)
    stride = 1 + w * 3
    pixels: list[tuple[int, int, int]] = []
    for y in range(h):
        base = y * stride + 1  # skip the per-row filter byte (0)
        for x in range(w):
            p = base + x * 3
            pixels.append((raw[p], raw[p + 1], raw[p + 2]))
    return pixels


@pytest.mark.unit
class TestPlateAppearanceS6:
    """⑤/S6: plates read as dyed armor, not grey boxes.

    The dye shader multiplies albedo by the dye (S3), so (a) the base albedo must
    be LIGHT for a dye to read as its true hue, (b) metallic must be low so the
    albedo drives the look, (c) the mesh must carry UVs so the mask maps across the
    surface, and (d) the mask must cover the FULL dyeable surface (no neutral gaps).
    """

    def test_base_albedo_is_light_steel(self):
        # grey (~0.4) × dye = mud; light steel (≥ 0.6) × dye = the dye's true hue.
        r, g, b, a = gwk.BASE_COLOR
        assert min(r, g, b) >= 0.6, f"base albedo {gwk.BASE_COLOR} too dark for dyes to read"
        assert a == 1.0

    @pytest.mark.parametrize("slot", SLOTS)
    def test_material_metallic_is_low(self, slot):
        from pygltflib import GLTF2

        gltf = GLTF2().load_from_bytes(gwk.build_plate_glb(slot))
        pbr = gltf.materials[0].pbrMetallicRoughness
        assert pbr.metallicFactor <= 0.2, f"{slot} metallic {pbr.metallicFactor} swallows the dyed albedo"

    @pytest.mark.parametrize("slot", SLOTS)
    def test_plate_carries_uvs_for_mask(self, slot):
        # Without TEXCOORD_0 the dye mask samples one texel → a dye tints a stripe.
        from pygltflib import GLTF2

        gltf = GLTF2().load_from_bytes(gwk.build_plate_glb(slot))
        prim = gltf.meshes[0].primitives[0]
        assert prim.attributes.TEXCOORD_0 is not None, f"{slot} plate has no UVs"
        acc = gltf.accessors[prim.attributes.TEXCOORD_0]
        assert acc.type == "VEC2"
        assert acc.count == gltf.accessors[prim.attributes.POSITION].count

    @pytest.mark.parametrize("slot", SLOTS)
    def test_mask_covers_full_surface(self, slot):
        # Every texel is dyed by exactly one channel — no neutral/black gaps that
        # would leave undyed stripes on the plate.
        pixels = _decode_mask_pixels(gwk.build_mask_png(slot))
        assert all(max(px) > 0 for px in pixels), f"{slot} mask has undyed (black) texels"
        # Primary (R) is the dominant channel so a single primary dye reads across
        # the whole plate body, not a thin band.
        primary = sum(1 for px in pixels if px[0] > 0)
        assert primary > len(pixels) * 0.5, f"{slot} primary region is not dominant"


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
