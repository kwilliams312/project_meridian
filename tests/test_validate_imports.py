"""Tests for tools/validate_imports.py — Godot import-preset validator (#138).

Inputs are constructed in-memory (no Godot, no Blender). One positive fixture per
class preset, one negative fixture per I-rule, a preset-well-formedness check, and a
sample real content/core sidecar. Mirrors the structure of test_validate_content.py.
"""

import json
import sys
import textwrap
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import validate_imports  # noqa: E402
from validate_imports import (  # noqa: E402
    build_class_index,
    load_presets,
    load_skeleton_defs,
    parse_import_params,
    validate,
)

PRESETS = REPO / "client" / "import-presets" / "presets.json"
SKELETON_DEFS = REPO / "schema" / "content" / "skeleton.defs.yaml"

_DEFS_DOC = yaml.safe_load(SKELETON_DEFS.read_text(encoding="utf-8"))["$defs"]
CANON_BONES: list[str] = _DEFS_DOC["boneName"]["enum"]
GEOSET_REGIONS: list[str] = _DEFS_DOC["geosetRegion"]["enum"]

PACK = """\
schema: meridian/pack@1
namespace: tp
name: Test Pack
version: 0.1.0
content_schema_version: 1
engine:
  godot: "4.6"
license: Apache-2.0
"""

# A conforming env-kit sidecar; individual tests mutate one field.
KIT = """\
schema: meridian/asset@1
id: tp:art.env.zone01.kit.wall_a
class: kit_piece
source: assets/art/env/zone01/kit/wall_a.glb
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
import_hints:
  lod_policy: authored
  lightmap_uv2: true
  occluder: true
"""

CHAR = """\
schema: meridian/asset@1
id: tp:art.char.hero
class: character_model
source: assets/art/char/hero.glb
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
import_hints:
  lod_policy: authored
"""

TEXTURE = """\
schema: meridian/asset@1
id: tp:art.tex.wall_bc
class: texture_set
source: assets/art/tex/wall_bc.png
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
"""

ICON = """\
schema: meridian/asset@1
id: tp:art.icon.item.potion
class: icon
source: assets/art/icon/item/potion.png
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
"""


def build(tmp_path: Path, files: dict[str, str]) -> Path:
    content = tmp_path / "content"
    for relpath, text in {"tp/pack.yaml": PACK, **files}.items():
        p = content / relpath
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(text), encoding="utf-8")
    return content


def run(tmp_path, files, presets=PRESETS, imports_mode="error"):
    return validate(build(tmp_path, files), presets, imports_mode)


def codes(messages):
    return {m.split(" ", 1)[0] for m in messages}


@pytest.mark.unit
class TestConforming:
    """(a) an asset conforming to its class preset passes with no errors."""

    def test_kit_piece_conforms(self, tmp_path):
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": KIT})
        assert res.errors == [], res.errors

    def test_character_conforms(self, tmp_path):
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": CHAR})
        assert res.errors == [], res.errors

    def test_texture_conforms(self, tmp_path):
        res = run(tmp_path, {"tp/assets/art/wall_bc.asset.yaml": TEXTURE})
        assert res.errors == [], res.errors

    def test_icon_conforms(self, tmp_path):
        res = run(tmp_path, {"tp/assets/art/potion.asset.yaml": ICON})
        assert res.errors == [], res.errors


@pytest.mark.unit
class TestOffPreset:
    """(b) an off-preset asset errors with a clear rule id."""

    def test_i001_unknown_class_has_no_preset(self, tmp_path):
        # Force a governed class out of the preset set by validating against a
        # cut-down preset file that omits it.
        data = json.loads(PRESETS.read_text())
        del data["presets"]["character"]  # drops character_model coverage
        cut = tmp_path / "presets.json"
        cut.write_text(json.dumps(data))
        res = run(
            tmp_path,
            {"tp/assets/art/hero.asset.yaml": CHAR},
            presets=cut,
        )
        assert "I001" in codes(res.errors)
        assert "IPRESET" in codes(res.errors)  # coverage gap also reported

    def test_i002_disallowed_source_extension(self, tmp_path):
        # A kit piece pointing at a .png instead of a mesh.
        bad = KIT.replace(
            "source: assets/art/env/zone01/kit/wall_a.glb",
            "source: assets/art/env/zone01/kit/wall_a.png",
        )
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": bad})
        assert "I002" in codes(res.errors)

    def test_i003_over_import_size_cap(self, tmp_path):
        # prop texture import cap is 1024²; declare 4096.
        prop = (
            CHAR.replace("class: character_model", "class: prop")
            .replace("id: tp:art.char.hero", "id: tp:art.prop.rock")
            .replace(
                "source: assets/art/char/hero.glb", "source: assets/art/prop/rock.glb"
            )
        )
        prop += "budget:\n  texture_max_px: 4096\n"
        res = run(tmp_path, {"tp/assets/art/rock.asset.yaml": prop})
        assert "I003" in codes(res.errors)

    def test_i004_disallowed_lod_policy(self, tmp_path):
        # env-kit permits only 'authored'; declare importer.
        bad = KIT.replace("lod_policy: authored", "lod_policy: importer")
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": bad})
        assert "I004" in codes(res.errors)

    def test_i005_lightmap_uv2_forbidden_on_character(self, tmp_path):
        bad = CHAR.replace(
            "  lod_policy: authored\n",
            "  lod_policy: authored\n  lightmap_uv2: true\n",
        )
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": bad})
        assert "I005" in codes(res.errors)

    def test_i005_lightmap_uv2_required_on_kit(self, tmp_path):
        bad = KIT.replace("lightmap_uv2: true", "lightmap_uv2: false")
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": bad})
        assert "I005" in codes(res.errors)

    def test_i006_kit_missing_occluder(self, tmp_path):
        bad = KIT.replace("occluder: true", "occluder: false")
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": bad})
        assert "I006" in codes(res.errors)

    def test_i007_texture_without_channel_suffix(self, tmp_path):
        # texture_set source with no recognized colorspace suffix.
        bad = TEXTURE.replace(
            "source: assets/art/tex/wall_bc.png",
            "source: assets/art/tex/wall_plain.png",
        )
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": bad})
        assert "I007" in codes(res.errors)

    def test_i007_linear_data_map_suffix_passes(self, tmp_path):
        # _n (normal) is a recognized linear-colorspace suffix.
        good = TEXTURE.replace(
            "source: assets/art/tex/wall_bc.png",
            "source: assets/art/tex/wall_n.png",
        )
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": good})
        assert res.errors == []

    def test_off_preset_is_warning_in_warn_mode(self, tmp_path):
        bad = KIT.replace("occluder: true", "occluder: false")
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": bad}, imports_mode="warn")
        assert res.errors == []
        assert "I006" in codes(res.warnings)


@pytest.mark.unit
class TestImportFileDrift:
    """I010 — a committed .import diverges from the class template."""

    def test_i010_generate_lods_drift(self, tmp_path):
        content = build(tmp_path, {"tp/assets/art/wall.asset.yaml": KIT})
        # Place a .import next to the source that flips generate_lods on for a kit
        # (the env-kit template pins it false).
        src = content / "tp" / "assets/art/env/zone01/kit/wall_a.glb"
        src.parent.mkdir(parents=True, exist_ok=True)
        src.write_bytes(b"")
        src.with_name("wall_a.glb.import").write_text(
            "[params]\nmeshes/generate_lods=true\n"
        )
        res = validate(content, PRESETS, "error")
        assert "I010" in codes(res.errors)

    def test_i010_matching_import_passes(self, tmp_path):
        content = build(tmp_path, {"tp/assets/art/wall.asset.yaml": KIT})
        src = content / "tp" / "assets/art/env/zone01/kit/wall_a.glb"
        src.parent.mkdir(parents=True, exist_ok=True)
        src.write_bytes(b"")
        src.with_name("wall_a.glb.import").write_text(
            "[params]\nmeshes/generate_lods=false\n"
        )
        res = validate(content, PRESETS, "error")
        assert res.errors == []


@pytest.mark.unit
class TestPresetTemplates:
    """(c) the preset templates are well-formed and cover the asset classes."""

    def test_presets_load_and_are_well_formed(self):
        data = load_presets(PRESETS)
        assert data["presets"]
        for name, preset in data["presets"].items():
            assert preset["classes"], name
            assert preset["source_ext"], name
            assert preset["cite"], name

    def test_no_class_claimed_by_two_presets(self):
        # build_class_index raises if a class is double-claimed.
        index = build_class_index(load_presets(PRESETS))
        assert "kit_piece" in index
        assert "character_model" in index

    def test_presets_cover_every_governed_class(self, tmp_path):
        # IPRESET coverage: validating an empty tree must yield no coverage gaps.
        res = validate(tmp_path / "empty" / "content", PRESETS, "error")
        assert "IPRESET" not in codes(res.errors)

    def test_malformed_preset_set_reports_ipreset(self, tmp_path):
        broken = tmp_path / "broken.json"
        broken.write_text('{"presets": {}}')
        res = validate(tmp_path / "content", broken, "error")
        assert "IPRESET" in codes(res.errors)

    def test_every_preset_has_a_template_file(self):
        # Each named preset ships a matching .import.tmpl for documentation/drift.
        data = load_presets(PRESETS)
        presets_dir = PRESETS.parent
        for name in data["presets"]:
            assert (presets_dir / f"{name}.import.tmpl").exists(), name

    def test_template_params_parse(self):
        params = parse_import_params(
            (PRESETS.parent / "env-kit.import.tmpl").read_text()
        )
        assert params["meshes/generate_lods"] == "false"


# --- I020-I023 glb rig/geoset/LOD rules (spec ④ §5, story #503) -------------

SKELETON_SIDECAR = """\
schema: meridian/asset@1
id: tp:art.char.test.skeleton
class: character_model
source: assets/art/char/skeleton.glb
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
import_hints:
  lod_policy: authored
"""

BODY_SIDECAR = """\
schema: meridian/asset@1
id: tp:art.char.test.base
class: character_model
source: assets/art/char/body.glb
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
import_hints:
  lod_policy: single
"""

ARMOR_SIDECAR = """\
schema: meridian/asset@1
id: tp:art.armor.chest.iron
class: armor_model
source: assets/art/armor/iron_chest.glb
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
import_hints:
  lod_policy: authored
"""

GEO_LOD0 = [f"geo_{r}_lod0" for r in GEOSET_REGIONS]


def _make_glb(path: Path, *, joints=(), meshes=(), skin=False) -> Path:
    """Write a minimal structural .glb: named joint nodes, optional named meshes
    (each on its own mesh-carrying node), optional skin over all joint nodes.
    Structure only — no buffers/accessors; the validator reads names, not geometry.
    """
    from pygltflib import GLTF2, Mesh, Node, Scene, Skin

    nodes = [Node(name=j) for j in joints]
    gltf_meshes = []
    for i, mesh_name in enumerate(meshes):
        gltf_meshes.append(Mesh(name=mesh_name, primitives=[]))
        nodes.append(Node(name=f"{mesh_name}_obj", mesh=i))
    skins = []
    if skin and joints:
        skins.append(Skin(name="skin", joints=list(range(len(joints)))))
    gltf = GLTF2(
        nodes=nodes,
        meshes=gltf_meshes,
        skins=skins,
        scenes=[Scene(nodes=list(range(len(nodes))))],
        scene=0,
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    gltf.save(str(path))
    return path


def run_rig(
    tmp_path,
    sidecar_text,
    glb_rel,
    *,
    joints=(),
    meshes=(),
    skin=False,
    imports_mode="error",
    make_glb=True,
):
    content = build(tmp_path, {"tp/assets/art/asset.asset.yaml": sidecar_text})
    if make_glb:
        _make_glb(content / "tp" / glb_rel, joints=joints, meshes=meshes, skin=skin)
    return validate(
        content, PRESETS, imports_mode, skeleton_defs_path=SKELETON_DEFS
    )


@pytest.mark.unit
class TestGltfRigRules:
    """I020-I023 — glb rig/geoset/LOD conformance (spec ④ §5)."""

    def test_passing_skeleton_glb(self, tmp_path):
        # Skeleton-only asset (no meshes): exactly the canonical 63 joints.
        res = run_rig(
            tmp_path, SKELETON_SIDECAR, "assets/art/char/skeleton.glb",
            joints=CANON_BONES,
        )
        assert res.errors == [], res.errors

    def test_passing_body_glb(self, tmp_path):
        # Body asset: canonical joints + all 8 geoset regions at lod0,
        # lod_policy 'single' (blockout) exempts the LOD chain.
        res = run_rig(
            tmp_path, BODY_SIDECAR, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=GEO_LOD0, skin=True,
        )
        assert res.errors == [], res.errors

    def test_i020_missing_and_extra_bones_listed(self, tmp_path):
        bad = [b for b in CANON_BONES if b != "Jaw"] + ["ExtraBone"]
        res = run_rig(
            tmp_path, SKELETON_SIDECAR, "assets/art/char/skeleton.glb", joints=bad
        )
        assert "I020" in codes(res.errors)
        msg = next(m for m in res.errors if m.startswith("I020"))
        assert "Jaw" in msg and "ExtraBone" in msg

    def test_i020_does_not_require_full_set_on_armor(self, tmp_path):
        # Gear binds a SUBSET of canonical bones — no I020 on armor_model.
        res = run_rig(
            tmp_path, ARMOR_SIDECAR, "assets/art/armor/iron_chest.glb",
            joints=["Hips", "Spine", "Chest"],
            meshes=["chest_lod0", "chest_lod1"], skin=True,
        )
        assert res.errors == [], res.errors

    def test_i021_armor_binding_noncanonical_joint(self, tmp_path):
        res = run_rig(
            tmp_path, ARMOR_SIDECAR, "assets/art/armor/iron_chest.glb",
            joints=["Hips", "NotABone"],
            meshes=["chest_lod0", "chest_lod1"], skin=True,
        )
        assert "I021" in codes(res.errors)
        msg = next(m for m in res.errors if m.startswith("I021"))
        assert "NotABone" in msg
        assert "Hips" not in msg  # canonical joints are not flagged

    def test_i022_missing_region_at_lod0(self, tmp_path):
        incomplete = [m for m in GEO_LOD0 if m != "geo_feet_lod0"]
        res = run_rig(
            tmp_path, BODY_SIDECAR, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=incomplete, skin=True,
        )
        assert "I022" in codes(res.errors)
        assert any("feet" in m for m in res.errors if m.startswith("I022"))

    def test_i022_unknown_region(self, tmp_path):
        res = run_rig(
            tmp_path, BODY_SIDECAR, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=GEO_LOD0 + ["geo_tail_lod0"], skin=True,
        )
        assert "I022" in codes(res.errors)
        assert any("tail" in m for m in res.errors if m.startswith("I022"))

    def test_i022_non_geoset_named_body_mesh(self, tmp_path):
        res = run_rig(
            tmp_path, BODY_SIDECAR, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=GEO_LOD0 + ["torso_raw"], skin=True,
        )
        assert "I022" in codes(res.errors)
        assert any("torso_raw" in m for m in res.errors if m.startswith("I022"))

    def test_i023_missing_lod_chain(self, tmp_path):
        # lod0-only body WITHOUT the 'single' exemption → I023.
        sidecar = BODY_SIDECAR.replace("lod_policy: single", "lod_policy: authored")
        res = run_rig(
            tmp_path, sidecar, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=GEO_LOD0, skin=True,
        )
        assert "I023" in codes(res.errors)

    def test_i023_chain_present_passes(self, tmp_path):
        # Every region ships its own chain (lod0 + lod1) → no I023.
        sidecar = BODY_SIDECAR.replace("lod_policy: single", "lod_policy: authored")
        chains = GEO_LOD0 + [f"geo_{r}_lod1" for r in GEOSET_REGIONS]
        res = run_rig(
            tmp_path, sidecar, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=chains, skin=True,
        )
        assert res.errors == [], res.errors

    def test_i023_chain_is_per_prefix_not_per_file(self, tmp_path):
        # Reviewer counter-example (PR #517 N2): all 8 regions at lod0 plus a
        # single stray geo_head_lod1 must NOT satisfy the chain — the other 7
        # regions have no lod1+, and the chainless prefixes are named.
        sidecar = BODY_SIDECAR.replace("lod_policy: single", "lod_policy: authored")
        res = run_rig(
            tmp_path, sidecar, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=GEO_LOD0 + ["geo_head_lod1"], skin=True,
        )
        assert "I023" in codes(res.errors), res.errors
        msg = next(m for m in res.errors if m.startswith("I023"))
        assert "geo_torso" in msg and "geo_feet" in msg  # chainless prefixes named

    def test_i023_complete_prefix_not_flagged(self, tmp_path):
        # In a mixed body only the chainless prefixes appear in the message —
        # geo_head (complete chain) must not be listed.
        sidecar = BODY_SIDECAR.replace("lod_policy: single", "lod_policy: authored")
        res = run_rig(
            tmp_path, sidecar, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=GEO_LOD0 + ["geo_head_lod1"], skin=True,
        )
        msg = next(m for m in res.errors if m.startswith("I023"))
        listed = msg.split("(")[0]
        assert "geo_head," not in listed and " geo_head " not in listed

    def test_iglb_corrupt_but_loadable_glb_is_error_not_crash(self, tmp_path):
        # A glb that pygltflib loads but whose skin joints index out of range
        # must surface as a validation error, not crash the CI validator run.
        from pygltflib import GLTF2, Node, Scene, Skin

        content = build(
            tmp_path, {"tp/assets/art/asset.asset.yaml": SKELETON_SIDECAR}
        )
        glb = content / "tp" / "assets/art/char/skeleton.glb"
        glb.parent.mkdir(parents=True, exist_ok=True)
        gltf = GLTF2(
            nodes=[Node(name="Root")],
            skins=[Skin(name="skin", joints=[99])],  # out-of-range node index
            scenes=[Scene(nodes=[0])],
            scene=0,
        )
        gltf.save(str(glb))
        res = validate(content, PRESETS, "error", skeleton_defs_path=SKELETON_DEFS)
        assert "IGLB" in codes(res.errors), res.errors
        assert any("skeleton.glb" in m for m in res.errors if m.startswith("IGLB"))

    def test_i023_single_policy_exempts(self, tmp_path):
        # Same lod0-only glb, sidecar lod_policy 'single' → no I023.
        res = run_rig(
            tmp_path, BODY_SIDECAR, "assets/art/char/body.glb",
            joints=CANON_BONES, meshes=GEO_LOD0, skin=True,
        )
        assert "I023" not in codes(res.errors)

    def test_rules_skip_when_bone_enum_empty(self, tmp_path, monkeypatch):
        # Contract ① empty-means-skip: no boneName enum → I020-I023 silent.
        monkeypatch.setattr(
            validate_imports,
            "load_skeleton_defs",
            lambda path: {"bones": [], "regions": list(GEOSET_REGIONS)},
        )
        bad = [b for b in CANON_BONES if b != "Jaw"] + ["ExtraBone"]
        res = run_rig(
            tmp_path, SKELETON_SIDECAR, "assets/art/char/skeleton.glb", joints=bad
        )
        assert res.errors == [], res.errors

    def test_rules_skip_when_defs_file_absent(self, tmp_path):
        # A content tree without schema/content/skeleton.defs.yaml (default
        # resolution) behaves like the pre-T1 empty enum: silent skip.
        content = build(
            tmp_path, {"tp/assets/art/asset.asset.yaml": SKELETON_SIDECAR}
        )
        _make_glb(
            content / "tp" / "assets/art/char/skeleton.glb", joints=["OnlyBone"]
        )
        res = validate(content, PRESETS, "error")  # no skeleton_defs_path
        assert res.errors == [], res.errors

    def test_rules_skip_when_glb_absent(self, tmp_path):
        # Source existence is L020's scope, not the rig rules'.
        res = run_rig(
            tmp_path, SKELETON_SIDECAR, "assets/art/char/skeleton.glb",
            make_glb=False,
        )
        assert res.errors == [], res.errors

    def test_rules_skip_on_unparseable_glb(self, tmp_path):
        # An LFS pointer stub (or any unparseable blob) is not structurally
        # checkable — skip, do not crash.
        content = build(
            tmp_path, {"tp/assets/art/asset.asset.yaml": SKELETON_SIDECAR}
        )
        glb = content / "tp" / "assets/art/char/skeleton.glb"
        glb.parent.mkdir(parents=True, exist_ok=True)
        glb.write_text(
            "version https://git-lfs.github.com/spec/v1\n"
            "oid sha256:deadbeef\nsize 42\n"
        )
        res = validate(content, PRESETS, "error", skeleton_defs_path=SKELETON_DEFS)
        assert res.errors == [], res.errors

    def test_gltf_rules_respect_warn_mode(self, tmp_path):
        bad = [b for b in CANON_BONES if b != "Jaw"]
        res = run_rig(
            tmp_path, SKELETON_SIDECAR, "assets/art/char/skeleton.glb",
            joints=bad, imports_mode="warn",
        )
        assert res.errors == []
        assert "I020" in codes(res.warnings)

    def test_load_skeleton_defs_reads_bones_and_regions(self):
        defs = load_skeleton_defs(SKELETON_DEFS)
        assert defs["bones"] == CANON_BONES
        assert defs["regions"] == GEOSET_REGIONS

    def test_load_skeleton_defs_absent_file_is_empty(self, tmp_path):
        defs = load_skeleton_defs(tmp_path / "nope.yaml")
        assert defs == {"bones": [], "regions": []}


@pytest.mark.unit
class TestCharacterPresetLodPolicy:
    """The character preset must permit 'single' for script-generated blockouts."""

    def test_i004_character_single_lod_policy_permitted(self, tmp_path):
        single = CHAR.replace("lod_policy: authored", "lod_policy: single")
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": single})
        assert "I004" not in codes(res.errors)


@pytest.mark.integration
class TestRealContent:
    """(d) the real content/core assets validate clean against the shipped presets."""

    def test_repo_content_imports_validate_clean(self):
        res = validate(REPO / "content", PRESETS, "error")
        assert res.errors == [], res.errors
        assert res.warnings == [], res.warnings
        assert res.stats["governed_sidecars"] > 0
