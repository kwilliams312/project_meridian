"""Tests for the schema-driven struct generator (tools/schema_gen, issue #117).

Covers the three requirements from the task:
  (a) the generator produces the expected structs — a controlled fixture schema
      (fixtures/widget.schema.yaml) lowered to IR, plus the real content schema
      lowered to the full set of typed structs;
  (b) consumption — the generated types faithfully represent a real content
      entity (asserted structurally here; a real C++ compile lives in
      test_cpp_consumer.py);
  (c) determinism — regenerating twice yields byte-identical output, and the
      checked-in generated files match the schema (drift guard).
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml
from jsonschema import Draft202012Validator

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO))

from tools.schema_gen import emit_cpp, emit_csharp, generate  # noqa: E402
from tools.schema_gen.ir import (  # noqa: E402
    ArrayType,
    EnumType,
    NestedType,
    RefType,
    ScalarType,
    UnionType,
    SchemaError,
    build_model,
    enum_member_ident,
    pascal,
)

FIXTURE_DIR = Path(__file__).resolve().parent / "fixtures"
SCHEMA_DIR = REPO / "schema" / "content"


# --- helpers ---------------------------------------------------------------


def _struct(model, name):
    for s in model.structs:
        if s.name == name:
            return s
    raise AssertionError(
        f"struct {name!r} not generated; have {[s.name for s in model.structs]}"
    )


def _field(struct, name):
    for f in struct.fields:
        if f.name == name:
            return f
    raise AssertionError(
        f"field {name!r} not on {struct.name}; have {[f.name for f in struct.fields]}"
    )


# --- (a) fixture schema -> expected structs --------------------------------


@pytest.mark.unit
def test_pascal_and_enum_ident():
    assert pascal("attack_speed_ms") == "AttackSpeedMs"
    assert pascal("npc") == "Npc"
    # Enum values with separators / leading digits become valid identifiers.
    assert enum_member_ident("on_pickup") == "OnPickup"
    assert enum_member_ident("CC0-1.0") == "CC010"
    assert enum_member_ident("L1") == "L1"
    assert enum_member_ident("4.6")[0] == "_"


@pytest.mark.unit
def test_fixture_root_struct_required_vs_optional():
    model = build_model(FIXTURE_DIR)
    widget = _struct(model, "Widget")
    assert widget.schema_tag == "meridian/widget@1"
    # `schema` const is dropped (it is the type discriminator).
    assert "schema" not in [f.name for f in widget.fields]
    # Required scalars.
    assert _field(widget, "name").required is True
    assert isinstance(_field(widget, "name").type, ScalarType)
    # Optional scalar with a default is still optional.
    assert _field(widget, "active").required is False
    assert _field(widget, "weight").type == ScalarType("number")


@pytest.mark.unit
def test_fixture_typed_refs():
    model = build_model(FIXTURE_DIR)
    widget = _struct(model, "Widget")
    # id -> ContentId ref.
    assert _field(widget, "id").type == RefType(ref_kind="content", is_asset=False)
    # parts[].item -> typed content ref; link.art -> typed asset ref.
    part = _struct(model, "WidgetPart")
    assert _field(part, "item").type == RefType(ref_kind="item", is_asset=False)
    link = _struct(model, "WidgetLink")
    assert _field(link, "art").type == RefType(ref_kind="art", is_asset=True)


@pytest.mark.unit
def test_fixture_enums_local_and_shared():
    model = build_model(FIXTURE_DIR)
    widget = _struct(model, "Widget")
    kind = _field(widget, "kind").type
    assert isinstance(kind, EnumType)
    assert kind.values == ("alpha", "beta", "gamma")
    # Shared $def enum reused by canonical name.
    cat = _field(widget, "category").type
    assert isinstance(cat, EnumType) and cat.name == "ItemClass"
    assert cat.values == ("weapon", "armor", "consumable")


@pytest.mark.unit
def test_fixture_shared_struct_and_nested_object():
    model = build_model(FIXTURE_DIR)
    widget = _struct(model, "Widget")
    # Shared intRange -> IntRange nested struct.
    assert _field(widget, "size").type == NestedType("IntRange")
    intrange = _struct(model, "IntRange")
    assert [f.name for f in intrange.fields] == ["min", "max"]
    # Inline object -> WidgetSpecs.
    assert _field(widget, "specs").type == NestedType("WidgetSpecs")
    specs = _struct(model, "WidgetSpecs")
    assert _field(specs, "rating").required is True
    assert _field(specs, "note").required is False


@pytest.mark.unit
def test_fixture_arrays():
    model = build_model(FIXTURE_DIR)
    widget = _struct(model, "Widget")
    tags = _field(widget, "tags").type
    assert tags == ArrayType(ScalarType("string"))
    parts = _field(widget, "parts").type
    assert isinstance(parts, ArrayType) and parts.element == NestedType("WidgetPart")


@pytest.mark.unit
def test_fixture_discriminated_union():
    model = build_model(FIXTURE_DIR)
    widget = _struct(model, "Widget")
    actions = _field(widget, "actions").type
    assert isinstance(actions, ArrayType)
    assert actions.element == UnionType(
        name="WidgetAction", tag_field="kind", variants=()
    )
    union = _struct(model, "WidgetAction")
    assert union.is_union and union.tag_field == "kind"
    # Discriminator enum + all variant fields, variant fields optional.
    kind = _field(union, "kind")
    assert kind.required is True and isinstance(kind.type, EnumType)
    assert kind.type.values == ("grant", "link")
    assert _field(union, "amount").required is False
    assert _field(union, "target").type == RefType(ref_kind="item", is_asset=False)


@pytest.mark.unit
def test_fixture_object_with_oneof_required_is_a_plain_struct():
    # link: shared properties + oneOf-of-required (item XOR art). The typed shape
    # is just the object with both optional; the XOR is a lint rule, not a type.
    model = build_model(FIXTURE_DIR)
    link = _struct(model, "WidgetLink")
    assert not link.is_union
    assert _field(link, "item").required is False
    assert _field(link, "art").required is False


# --- (a) real content schema -> full typed set -----------------------------


@pytest.mark.integration
def test_real_schema_generates_all_root_types():
    model = build_model(SCHEMA_DIR)
    root_names = {s.schema_type for s in model.structs if s.schema_type}
    assert root_names == {
        "pack",
        "npc",
        "item",
        "ability",
        "quest",
        "loot",
        "vendor",
        "spawn",
        "zone",
        "asset",
        "appearance",
        "dye",
        "attribute",
        "equip_type",
        "race",
        "talent",
        "talent_tree",
        "class",
    }


@pytest.mark.integration
def test_real_npc_struct_matches_schema():
    model = build_model(SCHEMA_DIR)
    npc = _struct(model, "Npc")
    # Required per npc.schema.yaml: id, name, level, creature_type, faction, stats.
    required = {f.name for f in npc.fields if f.required}
    assert {"id", "name", "level", "creature_type", "faction", "stats"} <= required
    # Typed shapes.
    assert _field(npc, "id").type == RefType(ref_kind="content", is_asset=False)
    assert _field(npc, "level").type == NestedType("IntRange")
    assert isinstance(_field(npc, "creature_type").type, EnumType)
    assert _field(npc, "stats").type == NestedType("NpcStats")
    # Nested stats has the schema's fields with correct requiredness.
    stats = _struct(model, "NpcStats")
    assert _field(stats, "health").required is True
    assert _field(stats, "mana").required is False
    assert _field(stats, "damage").type == NestedType("IntRange")


@pytest.mark.integration
def test_real_schema_ref_kinds_complete():
    model = build_model(SCHEMA_DIR)
    # Every id-prefix in common.defs shows up as a typed ref somewhere.
    assert set(model.content_ref_kinds) >= {
        "content",
        "npc",
        "item",
        "quest",
        "ability",
        "loot",
        "vendor",
        "zone",
    }
    assert set(model.asset_ref_kinds) >= {"art", "mus", "sfx", "amb", "asset"}


@pytest.mark.integration
def test_form_descriptor_manifest_covers_authoring_asset_contract():
    manifest = json.loads(generate.render(SCHEMA_DIR)["descriptors"])
    assert manifest["schema"] == "meridian/codex-form-descriptors@1"
    schemas = {entry["schema_file"]: entry for entry in manifest["schemas"]}

    def field(schema_file, path):
        return next(
            descriptor
            for descriptor in schemas[schema_file]["fields"]
            if descriptor["path"] == path
        )

    assert field("pack.schema.yaml", "namespace")["ui"]["widget"] == "slug"
    assert field("pack.schema.yaml", "theme.preview_asset")["asset"]["allowed_classes"] == ["icon", "ui_art"]
    assert field("npc.schema.yaml", "visual.model")["asset"] == {
        "allowed_classes": ["creature_model"],
        "eligible_generators": ["meshy"],
    }
    assert field("appearance.schema.yaml", "body_model")["asset"]["allowed_classes"] == ["character_model"]
    assert field("appearance.schema.yaml", "skeleton")["asset"]["eligible_generators"] == []
    assert field("item.schema.yaml", "visual.icon")["asset"]["allowed_classes"] == ["icon"]
    assert field("item.schema.yaml", "visual.model")["asset"]["eligible_generators"] == ["meshy"]
    assert field("ability.schema.yaml", "audio_visual.cast_vfx")["asset"] == {
        "allowed_classes": ["vfx"],
        "eligible_generators": [],
    }
    assert field("ability.schema.yaml", "audio_visual.cast_sfx")["asset"]["allowed_classes"] == ["sfx"]


@pytest.mark.integration
def test_annotations_do_not_change_runtime_model_outputs(tmp_path):
    stripped = tmp_path / "schemas"
    stripped.mkdir()

    def strip_annotations(value):
        if isinstance(value, dict):
            return {
                key: strip_annotations(child)
                for key, child in value.items()
                if key not in {"x-meridian-ui", "x-meridian-asset"}
            }
        if isinstance(value, list):
            return [strip_annotations(child) for child in value]
        return value

    for source in SCHEMA_DIR.glob("*.yaml"):
        parsed = yaml.safe_load(source.read_text(encoding="utf-8"))
        (stripped / source.name).write_text(
            yaml.safe_dump(strip_annotations(parsed), sort_keys=False),
            encoding="utf-8",
        )

    annotated = generate.render(SCHEMA_DIR)
    without_annotations = generate.render(stripped)
    assert annotated["cpp"] == without_annotations["cpp"]
    assert annotated["csharp"] == without_annotations["csharp"]


@pytest.mark.unit
def test_descriptor_emitter_rejects_unknown_annotation_keys(tmp_path):
    for source in FIXTURE_DIR.glob("*.yaml"):
        shutil.copy2(source, tmp_path / source.name)
    fixture = tmp_path / "widget.schema.yaml"
    schema = yaml.safe_load(fixture.read_text(encoding="utf-8"))
    schema["properties"]["name"]["x-meridian-ui"] = {"gropu": "identity"}
    fixture.write_text(yaml.safe_dump(schema, sort_keys=False), encoding="utf-8")

    with pytest.raises(SchemaError, match=r"widget\.schema\.yaml:name:.*unknown key.*gropu"):
        generate.render(tmp_path)


@pytest.mark.unit
def test_asset_annotation_requires_an_asset_reference(tmp_path):
    for source in FIXTURE_DIR.glob("*.yaml"):
        shutil.copy2(source, tmp_path / source.name)
    (tmp_path / "asset.schema.yaml").write_text(
        """
        type: object
        required: [schema, class]
        properties:
          schema: { const: meridian/asset@1 }
          class: { enum: [icon] }
        """,
        encoding="utf-8",
    )
    fixture = tmp_path / "widget.schema.yaml"
    schema = yaml.safe_load(fixture.read_text(encoding="utf-8"))
    schema["properties"]["name"]["x-meridian-asset"] = {
        "allowed_classes": ["icon"],
        "eligible_generators": [],
    }
    fixture.write_text(yaml.safe_dump(schema, sort_keys=False), encoding="utf-8")

    with pytest.raises(SchemaError, match=r"widget\.schema\.yaml:name:.*asset-reference"):
        generate.render(tmp_path)


@pytest.mark.integration
def test_ui_annotations_are_non_validating_draft_2020_12_keywords():
    for schema_file in ("pack", "npc", "appearance", "item", "ability"):
        schema = yaml.safe_load((SCHEMA_DIR / f"{schema_file}.schema.yaml").read_text(encoding="utf-8"))
        Draft202012Validator.check_schema(schema)


# --- (c) determinism + drift guard -----------------------------------------


@pytest.mark.integration
def test_generation_is_deterministic():
    a = generate.render(SCHEMA_DIR)
    b = generate.render(SCHEMA_DIR)
    assert a == b
    # And stable across a fresh model build (no hidden global state).
    m = build_model(SCHEMA_DIR)
    assert emit_cpp.emit(m) == a["cpp"]
    assert emit_csharp.emit(m) == a["csharp"]


@pytest.mark.integration
def test_checked_in_output_matches_schema_no_drift():
    rendered = generate.render(SCHEMA_DIR)
    for target, (_emit, rel) in generate.TARGETS.items():
        out = generate.GENERATED_ROOT / rel
        assert out.exists(), f"missing generated file for {target}: {out}"
        current = out.read_text(encoding="utf-8")
        assert current == rendered[target], (
            f"{out.relative_to(REPO)} is stale — run "
            f"`uv run python -m tools.schema_gen`"
        )


@pytest.mark.integration
def test_check_cli_passes_when_in_sync():
    # The --check drift guard the CI job runs.
    assert generate.main(["--check"]) == 0


@pytest.mark.integration
def test_generate_module_entrypoint_has_no_runpy_warning():
    """The legacy fully-qualified entry point remains warning-clean (#720)."""

    result = subprocess.run(
        [sys.executable, "-W", "error", "-m", "tools.schema_gen.generate", "--check"],
        cwd=REPO,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "RuntimeWarning" not in result.stderr


# --- output well-formedness (cheap structural sanity) ----------------------


@pytest.mark.integration
def test_cpp_output_is_wellformed():
    text = (generate.GENERATED_ROOT / "cpp" / "content_types.gen.hpp").read_text()
    assert text.startswith("// content_types.gen.hpp")
    assert "namespace mcc::content {" in text
    assert text.count("{") == text.count("}")
    # Every enumerator / field identifier must be a valid C++ identifier char set.
    import re

    for m in re.finditer(r"enum class \w+ \{([^}]*)\}", text):
        for member in m.group(1).split(","):
            member = member.strip()
            if member:
                assert re.fullmatch(r"[A-Za-z_]\w*", member), member


@pytest.mark.integration
def test_csharp_output_is_wellformed():
    text = (generate.GENERATED_ROOT / "csharp" / "ContentTypes.g.cs").read_text()
    assert "namespace Meridian.Codex.Models;" in text
    assert "#nullable enable" in text
    assert text.count("{") == text.count("}")
    # A `type: null` schema field must not double up nullability (`object??` is a
    # C# syntax error — regression guard for the quest.script / zone.chunk_manifest
    # reserved-null fields).
    assert "??" not in text
    # Root records expose the schema envelope from the source schema so editors
    # never need to duplicate a versioned discriminator by hand.
    assert 'public const string SchemaTag = "meridian/item@2";' in text
