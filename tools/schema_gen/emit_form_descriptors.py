"""Emit Codex's deterministic form-descriptor manifest from schema annotations.

The manifest deliberately contains presentation metadata only.  Structural types
and validation constraints remain in the authoritative JSON Schema and continue
to be consumed through the existing IR/C++/C# paths.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

import yaml

from .ir import KNOWN_TYPES, SchemaError

UI_KEY = "x-meridian-ui"
ASSET_KEY = "x-meridian-asset"

UI_KEYS = {
    "group",
    "label",
    "widget",
    "unit",
    "reference_type",
    "help",
    "example",
    "constraint",
    "documentation",
}
UI_WIDGETS = {
    "single_line",
    "multiline",
    "slug",
    "semver",
    "number",
    "asset_picker",
    "reference_picker",
    "animation",
}
UI_UNITS = {"ms", "m", "mps", "percent", "copper", "scale"}
ASSET_KEYS = {"allowed_classes", "eligible_generators"}
ELIGIBLE_GENERATORS = {"meshy"}
MESHY_ASSET_CLASSES = {
    "character_model",
    "creature_model",
    "weapon_model",
    "armor_model",
    "kit_piece",
    "prop",
    "foliage",
    "hero_landmark",
}
ASSET_REFERENCE_CLASSES = {
    "artRef": {
        "character_model",
        "creature_model",
        "weapon_model",
        "armor_model",
        "kit_piece",
        "prop",
        "foliage",
        "hero_landmark",
        "texture_set",
        "icon",
        "vfx",
        "ui_art",
    },
    "musRef": {"music_stem", "music_stinger"},
    "sfxRef": {"sfx", "ui_sound"},
    "ambRef": {"ambience_bed", "ambience_emitter"},
}
REFERENCE_TYPES = {
    "contentId": "content:content",
    "npcRef": "content:npc",
    "itemRef": "content:item",
    "questRef": "content:quest",
    "abilityRef": "content:ability",
    "lootRef": "content:loot",
    "vendorRef": "content:vendor",
    "zoneRef": "content:zone",
    "dyeRef": "content:dye",
    "appearanceRef": "content:appearance",
    "attributeRef": "content:attribute",
    "equipTypeRef": "content:equip_type",
    "raceRef": "content:race",
    "talentRef": "content:talent",
    "talentTreeRef": "content:talent_tree",
    "equipTypeId": "content:content",
    "raceId": "content:content",
    "talentId": "content:content",
    "talentTreeId": "content:content",
    "classId": "content:content",
    "artRef": "asset:art",
    "musRef": "asset:mus",
    "sfxRef": "asset:sfx",
    "ambRef": "asset:amb",
    "assetId": "asset:asset",
}
_SLUG = re.compile(r"^[a-z][a-z0-9_]*$")


def emit(schema_dir: Path) -> str:
    """Return the canonical JSON descriptor manifest for ``schema_dir``."""
    present = {path.name.split(".")[0]: path for path in schema_dir.glob("*.schema.yaml")}
    ordered = [name for name in KNOWN_TYPES if name in present]
    ordered += sorted(name for name in present if name not in KNOWN_TYPES)
    asset_classes = _asset_classes(present.get("asset"))

    schemas: list[dict[str, Any]] = []
    for type_name in ordered:
        path = present[type_name]
        schema = yaml.safe_load(path.read_text(encoding="utf-8"))
        if not isinstance(schema, dict):
            raise SchemaError(f"{path.name}: root must be an object")
        fields: list[dict[str, Any]] = []
        _collect(schema, "", path.name, fields, asset_classes)
        schemas.append(
            {
                "schema_file": path.name,
                "schema_id": schema.get("$id"),
                "content_schema": schema.get("properties", {})
                .get("schema", {})
                .get("const"),
                "fields": fields,
            }
        )

    manifest = {
        "schema": "meridian/codex-form-descriptors@1",
        "schemas": schemas,
    }
    return json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"


def _collect(
    node: dict[str, Any],
    field_path: str,
    schema_file: str,
    fields: list[dict[str, Any]],
    asset_classes: set[str] | None,
) -> None:
    ui = node.get(UI_KEY)
    asset = node.get(ASSET_KEY)
    if ui is not None or asset is not None:
        if any(existing["path"] == field_path for existing in fields):
            _error(
                schema_file,
                field_path,
                "multiple annotated oneOf branches share one logical form path",
            )
        descriptor: dict[str, Any] = {"path": field_path}
        if ui is not None:
            descriptor["ui"] = _validate_ui(ui, schema_file, field_path, node)
        if asset is not None:
            descriptor["asset"] = _validate_asset(
                asset, schema_file, field_path, node, asset_classes
            )
        fields.append(descriptor)

    properties = node.get("properties")
    if isinstance(properties, dict):
        for name, child in properties.items():
            if isinstance(child, dict):
                child_path = name if not field_path else f"{field_path}.{name}"
                _collect(child, child_path, schema_file, fields, asset_classes)

    items = node.get("items")
    if isinstance(items, dict):
        _collect(items, f"{field_path}[]", schema_file, fields, asset_classes)

    variants = node.get("oneOf")
    if isinstance(variants, list):
        for variant in variants:
            if isinstance(variant, dict):
                _collect(variant, field_path, schema_file, fields, asset_classes)


def _validate_ui(
    value: Any, schema_file: str, path: str, field: dict[str, Any]
) -> dict[str, Any]:
    annotation = _mapping(value, UI_KEY, schema_file, path)
    _known_keys(annotation, UI_KEYS, UI_KEY, schema_file, path)
    for key in ("group", "label", "help", "constraint", "documentation"):
        if key in annotation and not _nonempty_string(annotation[key]):
            _error(schema_file, path, f"{UI_KEY}.{key} must be a non-empty string")
    if "documentation" in annotation and not _valid_documentation(
        annotation["documentation"]
    ):
        _error(
            schema_file,
            path,
            f"{UI_KEY}.documentation must be a repository docs path or HTTPS URL",
        )
    if "group" in annotation and (
        not isinstance(annotation["group"], str)
        or not _SLUG.fullmatch(annotation["group"])
    ):
        _error(schema_file, path, f"{UI_KEY}.group must be a lower_snake_case slug")
    if "widget" in annotation and (
        not isinstance(annotation["widget"], str)
        or annotation["widget"] not in UI_WIDGETS
    ):
        _error(schema_file, path, f"{UI_KEY}.widget must be one of {sorted(UI_WIDGETS)}")
    if "unit" in annotation and (
        not isinstance(annotation["unit"], str)
        or annotation["unit"] not in UI_UNITS
    ):
        _error(schema_file, path, f"{UI_KEY}.unit must be one of {sorted(UI_UNITS)}")
    if "reference_type" in annotation:
        reference_type = annotation["reference_type"]
        if not isinstance(reference_type, str) or reference_type not in set(
            REFERENCE_TYPES.values()
        ):
            _error(
                schema_file,
                path,
                f"{UI_KEY}.reference_type must be one of {sorted(set(REFERENCE_TYPES.values()))}",
            )
        # A field-local annotation must describe the exact typed $ref it accompanies;
        # pattern-only strings cannot claim a reference type without a schema ref.
        reference = field.get("$ref")
        annotation_ref = (
            REFERENCE_TYPES.get(reference.rsplit("/", 1)[-1])
            if isinstance(reference, str)
            else None
        )
        if annotation_ref != reference_type:
            _error(
                schema_file,
                path,
                f"{UI_KEY}.reference_type {reference_type!r} is incompatible with field reference {reference!r}",
            )
    if "example" in annotation and annotation["example"] is not None and not isinstance(
        annotation["example"], (str, int, float, bool)
    ):
        _error(schema_file, path, f"{UI_KEY}.example must be a scalar")
    return annotation


def _validate_asset(
    value: Any,
    schema_file: str,
    path: str,
    field: dict[str, Any],
    asset_classes_contract: set[str] | None,
) -> dict[str, Any]:
    annotation = _mapping(value, ASSET_KEY, schema_file, path)
    _known_keys(annotation, ASSET_KEYS, ASSET_KEY, schema_file, path)
    reference = field.get("$ref")
    reference_name = reference.rsplit("/", 1)[-1] if isinstance(reference, str) else None
    if reference_name not in ASSET_REFERENCE_CLASSES:
        _error(schema_file, path, f"{ASSET_KEY} requires an asset-reference field")
    if asset_classes_contract is None:
        _error(
            schema_file,
            path,
            f"{ASSET_KEY} cannot be checked without asset.schema.yaml",
        )
    asset_classes = annotation.get("allowed_classes")
    if (
        not isinstance(asset_classes, list)
        or not asset_classes
        or not all(isinstance(asset_class, str) for asset_class in asset_classes)
        or any(asset_class not in asset_classes_contract for asset_class in asset_classes)
    ):
        _error(
            schema_file,
            path,
            f"{ASSET_KEY}.allowed_classes must be a non-empty subset of {sorted(asset_classes_contract)}",
        )
    if len(asset_classes) != len(set(asset_classes)):
        _error(schema_file, path, f"{ASSET_KEY}.allowed_classes must be unique")
    compatible_classes = ASSET_REFERENCE_CLASSES[reference_name]
    if not set(asset_classes).issubset(compatible_classes):
        _error(
            schema_file,
            path,
            f"{ASSET_KEY}.allowed_classes must be compatible with {reference_name}: {sorted(compatible_classes)}",
        )
    generators = annotation.get("eligible_generators", [])
    if (
        not isinstance(generators, list)
        or not all(isinstance(generator, str) for generator in generators)
        or any(
            generator not in ELIGIBLE_GENERATORS for generator in generators
        )
    ):
        _error(
            schema_file,
            path,
            f"{ASSET_KEY}.eligible_generators must contain only {sorted(ELIGIBLE_GENERATORS)}",
        )
    if len(generators) != len(set(generators)):
        _error(schema_file, path, f"{ASSET_KEY}.eligible_generators must be unique")
    if "meshy" in generators and not set(asset_classes).issubset(MESHY_ASSET_CLASSES):
        _error(
            schema_file,
            path,
            f"{ASSET_KEY}.eligible_generators includes meshy but allowed_classes must be a subset of {sorted(MESHY_ASSET_CLASSES)}",
        )
    return annotation


def _asset_classes(asset_schema: Path | None) -> set[str] | None:
    if asset_schema is None:
        return None
    schema = yaml.safe_load(asset_schema.read_text(encoding="utf-8"))
    values = schema.get("properties", {}).get("class", {}).get("enum")
    if not isinstance(values, list) or not values or not all(
        isinstance(value, str) for value in values
    ):
        raise SchemaError(
            f"{asset_schema.name}: properties.class.enum must define asset classes"
        )
    return set(values)


def _mapping(value: Any, key: str, schema_file: str, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _error(schema_file, path, f"{key} must be an object")
    return value


def _known_keys(
    value: dict[str, Any],
    allowed: set[str],
    key: str,
    schema_file: str,
    path: str,
) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        _error(schema_file, path, f"{key} has unknown key(s): {', '.join(unknown)}")


def _nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _valid_documentation(value: str) -> bool:
    if ".." in Path(value).parts:
        return False
    if value.startswith("docs/") or value.startswith("schema/content/README.md"):
        return True
    parsed = urlparse(value)
    return parsed.scheme == "https" and bool(parsed.netloc)


def _error(schema_file: str, path: str, message: str) -> None:
    location = path or "<root>"
    raise SchemaError(f"{schema_file}:{location}: {message}")
