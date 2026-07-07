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

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from validate_imports import (  # noqa: E402
    build_class_index,
    load_presets,
    parse_import_params,
    validate,
)

PRESETS = REPO / "client" / "import-presets" / "presets.json"

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


@pytest.mark.integration
class TestRealContent:
    """(d) the real content/core assets validate clean against the shipped presets."""

    def test_repo_content_imports_validate_clean(self):
        res = validate(REPO / "content", PRESETS, "error")
        assert res.errors == [], res.errors
        assert res.warnings == [], res.warnings
        assert res.stats["governed_sidecars"] > 0
