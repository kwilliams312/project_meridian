"""Tests for the meridian_export Blender addon.

The pure sidecar module is tested WITHOUT a Blender install: `bpy` is mocked at
import so `import meridian_export` succeeds, and the sidecar-shaping logic is
exercised directly against the real asset.schema.yaml and the repo's own
validate_content.py (#142 provenance + budget lints).
"""

from __future__ import annotations

import sys
import types
from pathlib import Path

import pytest
import yaml
from jsonschema import Draft202012Validator

REPO = Path(__file__).resolve().parent.parent
ADDON_DIR = REPO / "tools" / "blender" / "meridian_export"
SCHEMA_DIR = REPO / "schema" / "content"

# Make the addon package + the CI validator importable.
sys.path.insert(0, str(ADDON_DIR))
sys.path.insert(0, str(REPO / "tools"))

import sidecar  # noqa: E402  (the pure module — no bpy)
import rig_checks  # noqa: E402  (the pure module — no bpy)
from validate_content import (  # noqa: E402
    check_budget as ci_check_budget,
    check_provenance as ci_check_provenance,
)


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


# --- Fixtures for a typical kit piece ------------------------------------------
def _kit_mesh(**over) -> sidecar.MeshInfo:
    defaults = dict(
        name="sm_env_zone01_kit_wall_stone_a",
        tri_count=8_500,
        texture_max_px=2048,
        material_set_count=3,
        scale=(1.0, 1.0, 1.0),
        transform_applied=True,
        has_negative_scale=False,
        pivot_offset_m=(0.0, 0.0, 0.0),
        kit_category="wall",
        aabb_size_m=(2.0, 3.0, 0.5),
    )
    defaults.update(over)
    return sidecar.MeshInfo(**defaults)


def _original_prov(**over) -> sidecar.ProvenanceInput:
    defaults = dict(source_tier="original", authors=["ken"], license="CC-BY-4.0")
    defaults.update(over)
    return sidecar.ProvenanceInput(**defaults)


# --- (a) emitted sidecar VALIDATES against asset.schema.yaml --------------------
def test_build_sidecar_original_kit_validates_against_schema(asset_validator):
    doc = sidecar.build_sidecar(
        asset_id="core:art.env.zone01.kit.wall_stone_a",
        asset_class="kit_piece",
        source="assets/art/env/zone01/kit/wall_stone_a.glb",
        mesh=_kit_mesh(),
        provenance=_original_prov(),
        import_hints={"lod_policy": "authored", "lightmap_uv2": True, "occluder": True},
    )
    errors = sorted(asset_validator.iter_errors(doc), key=lambda e: e.json_path)
    assert errors == [], [e.message for e in errors]


def test_build_sidecar_cc_by_kit_validates_against_schema(asset_validator):
    """cc_by tier: schema allOf requires origin_url + attribution + license_verified_on."""
    doc = sidecar.build_sidecar(
        asset_id="core:art.env.zone01.rock.cliff03",
        asset_class="prop",
        source="assets/art/env/zone01/rock_cliff03.glb",
        mesh=_kit_mesh(
            name="sm_env_zone01_rock_cliff03",
            tri_count=2_400,
            texture_max_px=1024,
            material_set_count=1,
            kit_category=None,
            aabb_size_m=None,
        ),
        provenance=_original_prov(
            source_tier="cc_by",
            license="CC-BY-4.0",
            origin_url="https://ambientcg.com/view?id=Rock030",
            attribution="Rock030 by ambientCG",
            license_verified_on="2026-09-14",
        ),
    )
    errors = sorted(asset_validator.iter_errors(doc), key=lambda e: e.json_path)
    assert errors == [], [e.message for e in errors]


def test_build_sidecar_ai_tier_validates_and_carries_ai_block(asset_validator):
    doc = sidecar.build_sidecar(
        asset_id="core:art.prop.crate.ai01",
        asset_class="prop",
        source="assets/art/prop/crate_ai01.glb",
        mesh=_kit_mesh(
            name="sm_prop_crate_ai01",
            tri_count=1_200,
            texture_max_px=512,
            material_set_count=1,
            kit_category=None,
            aabb_size_m=None,
        ),
        provenance=_original_prov(
            source_tier="ai",
            license="CC-BY-4.0",
            origin_url="https://example.com/gen",
            license_verified_on="2026-09-14",
            ai_tool="somegen v1.2",
            ai_prompts_file="prompts/crate_ai01.txt",
        ),
    )
    errors = sorted(asset_validator.iter_errors(doc), key=lambda e: e.json_path)
    assert errors == [], [e.message for e in errors]
    assert doc["provenance"]["ai"] == {
        "tool": "somegen v1.2",
        "prompts_file": "prompts/crate_ai01.txt",
    }


# --- (b) emitted sidecar passes the #142 provenance + budget lints -------------
def test_emitted_sidecar_passes_ci_provenance_lint():
    doc = sidecar.build_sidecar(
        asset_id="core:art.env.zone01.kit.wall_stone_a",
        asset_class="kit_piece",
        source="assets/art/env/zone01/kit/wall_stone_a.glb",
        mesh=_kit_mesh(),
        provenance=_original_prov(),
    )
    errors = ci_check_provenance(doc, Path("wall_stone_a.asset.yaml"))
    assert errors == [], errors


def test_emitted_sidecar_passes_ci_budget_lint_within_caps():
    doc = sidecar.build_sidecar(
        asset_id="core:art.env.zone01.kit.wall_stone_a",
        asset_class="kit_piece",
        source="assets/art/env/zone01/kit/wall_stone_a.glb",
        mesh=_kit_mesh(tri_count=8_500, texture_max_px=2048, material_set_count=3),
        provenance=_original_prov(),
    )
    errors = ci_check_budget(doc, Path("wall_stone_a.asset.yaml"))
    assert errors == [], errors


def test_over_budget_kit_is_flagged_by_ci_budget_lint():
    """A kit piece over the 20k tri ceiling must fail the CI L070 lint."""
    doc = sidecar.build_sidecar(
        asset_id="core:art.env.zone01.kit.wall_stone_a",
        asset_class="kit_piece",
        source="assets/art/env/zone01/kit/wall_stone_a.glb",
        mesh=_kit_mesh(tri_count=25_000),
        provenance=_original_prov(),
    )
    errors = ci_check_budget(doc, Path("wall_stone_a.asset.yaml"))
    assert any("L070" in e and "lod0_tris" in e for e in errors), errors


def test_ai_tier_missing_ai_block_fails_ci_provenance_lint():
    """AI tier without the ai block must fail L021 (prompt hygiene)."""
    doc = sidecar.build_sidecar(
        asset_id="core:art.prop.crate.ai01",
        asset_class="prop",
        source="assets/art/prop/crate_ai01.glb",
        mesh=_kit_mesh(name="sm_prop_crate_ai01", kit_category=None, aabb_size_m=None),
        provenance=_original_prov(
            source_tier="ai", origin_url="https://example.com/gen"
        ),
    )
    errors = ci_check_provenance(doc, Path("crate_ai01.asset.yaml"))
    assert any("L021" in e and "ai." in e for e in errors), errors


def test_disallowed_license_fails_ci_lint():
    doc = sidecar.build_sidecar(
        asset_id="core:art.prop.crate.a",
        asset_class="prop",
        source="assets/art/prop/crate_a.glb",
        mesh=_kit_mesh(name="sm_prop_crate_a", kit_category=None, aabb_size_m=None),
        provenance=_original_prov(license="GPL-3.0"),
    )
    errors = ci_check_provenance(doc, Path("crate_a.asset.yaml"))
    assert any("L022" in e for e in errors), errors


# --- budget computed from the mesh ---------------------------------------------
def test_compute_budget_reads_tris_and_texture_from_mesh():
    budgets = sidecar.load_budgets()
    block = sidecar.compute_budget(
        _kit_mesh(tri_count=7_777, texture_max_px=1024), "kit_piece", budgets
    )
    assert block["lod0_tris"] == 7_777
    assert block["texture_max_px"] == 1024
    assert block["material_sets"] == 3


def test_compute_budget_omits_texture_when_undeterminable():
    budgets = sidecar.load_budgets()
    block = sidecar.compute_budget(
        _kit_mesh(texture_max_px=None, material_set_count=None), "kit_piece", budgets
    )
    assert "texture_max_px" not in block
    assert "material_sets" not in block
    assert block["lod0_tris"] > 0


# --- (c) naming / convention violations are flagged ----------------------------
def test_bad_object_name_flagged():
    budgets = sidecar.load_budgets()
    warnings = sidecar.check_naming(_kit_mesh(name="WallStoneA"), "kit_piece", budgets)
    assert any(w.code == "NAME" for w in warnings)


def test_wrong_prefix_for_class_flagged():
    budgets = sidecar.load_budgets()
    # character_model expects sk_, a sm_ name should warn.
    warnings = sidecar.check_naming(
        _kit_mesh(name="sm_char_human_male_base"), "character_model", budgets
    )
    assert any(w.code == "NAME" for w in warnings)


def test_unapplied_transform_flagged():
    warnings = sidecar.check_transform(_kit_mesh(transform_applied=False))
    assert any(w.code == "SCALE" for w in warnings)


def test_non_unit_scale_flagged():
    warnings = sidecar.check_transform(_kit_mesh(scale=(2.0, 1.0, 1.0)))
    assert any(w.code == "SCALE" for w in warnings)


def test_negative_scale_flagged():
    warnings = sidecar.check_transform(_kit_mesh(has_negative_scale=True))
    assert any(w.code == "SCALE" for w in warnings)


def test_pivot_offset_beyond_tolerance_flagged():
    warnings = sidecar.check_pivot(_kit_mesh(pivot_offset_m=(0.05, 0.0, 0.0)))
    assert any(w.code == "PIVOT" for w in warnings)


def test_off_grid_bounds_flagged():
    warnings = sidecar.check_pivot(_kit_mesh(aabb_size_m=(2.3, 3.0, 0.5)))
    assert any(w.code == "GRID" for w in warnings)


def test_clean_kit_has_no_warnings():
    budgets = sidecar.load_budgets()
    budget = sidecar.compute_budget(_kit_mesh(), "kit_piece", budgets)
    warnings = sidecar.collect_warnings(_kit_mesh(), "kit_piece", budget, budgets)
    assert warnings == [], [(w.code, w.message) for w in warnings]


def test_validate_id_and_source():
    assert sidecar.validate_id("core:art.env.zone01.kit.wall_stone_a") is None
    assert sidecar.validate_id("art.env.wall") is not None  # no namespace
    assert sidecar.validate_id("core:npc.dummy") is not None  # not art
    assert sidecar.validate_source("assets/art/env/wall.glb") is None
    assert sidecar.validate_source("/abs/art/wall.glb") is not None


# --- addon imports without Blender (bpy mocked) --------------------------------
def test_addon_imports_with_bpy_mocked(monkeypatch):
    """The addon package must import when bpy is absent (mocked)."""
    # Provide a stub bpy so `from bpy.props import ...` succeeds.
    fake_bpy = types.ModuleType("bpy")
    fake_props = types.ModuleType("bpy.props")
    fake_types = types.ModuleType("bpy.types")
    for prop in ("BoolProperty", "EnumProperty", "StringProperty"):
        setattr(fake_props, prop, lambda **kw: None)
    fake_types.Operator = object
    fake_types.Panel = object
    fake_bpy.props = fake_props
    fake_bpy.types = fake_types
    monkeypatch.setitem(sys.modules, "bpy", fake_bpy)
    monkeypatch.setitem(sys.modules, "bpy.props", fake_props)
    monkeypatch.setitem(sys.modules, "bpy.types", fake_types)
    # Import the package fresh.
    monkeypatch.syspath_prepend(str(ADDON_DIR.parent))
    sys.modules.pop("meridian_export", None)
    import importlib

    mod = importlib.import_module("meridian_export")
    assert mod.bl_info["name"] == "meridian_export"
    assert hasattr(mod, "register") and hasattr(mod, "unregister")
    assert mod.MERIDIAN_OT_export_asset.bl_idname == "meridian.export_asset"


# --- rig_checks: E-rule band (rig/geoset conformance, spec ④ §3/§4) ------------


def _rig_data(**over) -> rig_checks.RigData:
    """A conforming character_model RigData — one geoset mesh per region at lod0."""
    defaults = dict(
        asset_class="character_model",
        bone_names=list(rig_checks.CANONICAL_BONE_NAMES),
        socket_names=list(rig_checks.SOCKET_NAMES),
        mesh_names=[f"geo_{region}_lod0" for region in rig_checks.GEOSET_REGIONS],
        max_influences=4,
        weights_normalized=True,
    )
    defaults.update(over)
    return rig_checks.RigData(**defaults)


def test_conforming_rig_has_no_errors():
    assert rig_checks.check_rig(_rig_data()) == []


def test_unknown_bone_name_flagged_e100():
    data = _rig_data(bone_names=list(rig_checks.CANONICAL_BONE_NAMES) + ["Tail01"])
    errors = rig_checks.check_rig(data)
    assert any(e.startswith("E100") and "Tail01" in e for e in errors), errors


def test_missing_socket_back_flagged_e101():
    data = _rig_data(
        socket_names=[n for n in rig_checks.SOCKET_NAMES if n != "socket_back"]
    )
    errors = rig_checks.check_rig(data)
    assert any(e.startswith("E101") and "socket_back" in e for e in errors), errors


def test_body_missing_geo_waist_lod0_flagged_e102():
    data = _rig_data(
        mesh_names=[
            f"geo_{region}_lod0"
            for region in rig_checks.GEOSET_REGIONS
            if region != "waist"
        ]
    )
    errors = rig_checks.check_rig(data)
    assert any(e.startswith("E102") and "waist" in e for e in errors), errors


def test_five_influences_flagged_e103():
    data = _rig_data(max_influences=5)
    errors = rig_checks.check_rig(data)
    assert any(e.startswith("E103") for e in errors), errors


def test_unnormalized_weights_flagged_e103():
    data = _rig_data(weights_normalized=False)
    errors = rig_checks.check_rig(data)
    assert any(e.startswith("E103") for e in errors), errors


def test_unknown_geoset_region_flagged_e104():
    data = _rig_data(mesh_names=[*_rig_data().mesh_names, "geo_tail_lod0"])
    errors = rig_checks.check_rig(data)
    assert any(e.startswith("E104") and "geo_tail_lod0" in e for e in errors), errors


def test_armor_mesh_with_geoset_name_flagged_e104():
    data = _rig_data(
        asset_class="armor_model",
        mesh_names=["geo_torso_lod0"],
    )
    errors = rig_checks.check_rig(data)
    assert any(e.startswith("E104") and "geo_torso_lod0" in e for e in errors), errors
