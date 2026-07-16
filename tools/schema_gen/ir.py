"""Schema -> intermediate representation (IR) for the struct generator.

Reads the Content Schema v1 (JSON-Schema-as-YAML in /schema/content) and lowers
each type schema into a language-neutral IR: named types, each with typed fields
(scalar / enum / ref / array / nested-object), required vs optional. The C++ and
C# emitters consume this IR, so schema-shape knowledge lives here exactly once.

This is the schema-driven typed content model the Tools SAD calls for:
  - SAD §2.1: "one C++ struct per schema type, generated from /schema/content ...
    (the same generator emits the Server's table-loading structs)"
  - SAD §6.1: "Models/  # generated from /schema/content (same generator as mcc's
    structs, C# target)"

The generator is deterministic: field order follows the schema's `properties`
declaration order (PyYAML preserves mapping order on 3.12), nested types get
stable derived names, and no wall-clock/host state enters the output.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

# The eight content types + pack + asset — every *.schema.yaml under /schema/content.
# (file stem -> schema `type` const suffix). Order here is only used for iteration
# determinism when the loader globs the directory.
KNOWN_TYPES = (
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
)


class SchemaError(ValueError):
    """A schema construct the generator does not know how to lower."""


# --- IR node kinds ---------------------------------------------------------


@dataclass(frozen=True)
class ScalarType:
    """A JSON-Schema scalar: string / integer / number / boolean / null."""

    json_type: str  # "string" | "integer" | "number" | "boolean" | "null"


@dataclass(frozen=True)
class RefType:
    """A typed id reference (npcRef, itemRef, artRef, ...). Carries the target
    kind so emitters can produce a strongly-typed id newtype instead of a bare
    string, matching the schema README: "an `item:` field cannot accept an NPC ID."
    """

    ref_kind: str  # e.g. "npc", "item", "art", "mus", ... (the id prefix)
    is_asset: bool  # asset refs (art/mus/sfx/amb) vs content refs (npc/item/...)


@dataclass(frozen=True)
class EnumType:
    """A closed set of string values. `name` is the generated enum type name."""

    name: str
    values: tuple[str, ...]


@dataclass(frozen=True)
class NestedType:
    """An inline object lowered to its own generated struct (`name`)."""

    name: str


@dataclass(frozen=True)
class ArrayType:
    """A homogeneous array. `element` is any IR type node (including nested/union)."""

    element: "IrType"


@dataclass(frozen=True)
class UnionType:
    """A oneOf of object shapes, discriminated by a const field (e.g. quest
    objectives, ability effects, loot entry item-or-table). Lowered to a tagged
    struct: every variant's fields become optional on one struct plus the tag.
    Emitters render it as a single struct with a discriminator + optional members.
    """

    name: str
    tag_field: str | None  # the const key ("kind"/"type"), or None if untagged
    variants: tuple["IrStruct", ...]


IrType = ScalarType | RefType | EnumType | NestedType | ArrayType | UnionType


@dataclass
class IrField:
    name: str
    type: IrType
    required: bool
    description: str | None = None


@dataclass
class IrStruct:
    name: str  # generated type name, e.g. "Npc", "NpcStats", "NpcAiEntry"
    schema_type: str | None  # the `type` const suffix (e.g. "npc") for root structs
    schema_tag: str | None = None  # root `schema.const`, e.g. "meridian/npc@2"
    fields: list[IrField] = field(default_factory=list)
    description: str | None = None
    # True when this struct is a flattened tagged union (oneOf of variants). Then
    # `tag_field` names the discriminator member (an enum), and all other members
    # are optional variant fields.
    is_union: bool = False
    tag_field: str | None = None


@dataclass
class IrModel:
    """Everything the emitters need: the ordered structs, enums, and the set of
    distinct ref kinds encountered (so id newtypes are declared once)."""

    structs: list[IrStruct] = field(default_factory=list)
    enums: list[EnumType] = field(default_factory=list)
    content_ref_kinds: list[str] = field(default_factory=list)  # npc,item,...
    asset_ref_kinds: list[str] = field(default_factory=list)  # art,mus,sfx,amb


# --- naming ----------------------------------------------------------------


def pascal(snake: str) -> str:
    """snake_case (or single word) -> PascalCase. Deterministic, no locale."""
    return "".join(part[:1].upper() + part[1:] for part in snake.split("_") if part)


def enum_member_ident(value: str) -> str:
    """A safe C++/C# enum-member identifier for a schema enum string value.

    Schema enum values are mostly [a-z0-9_], but a few (SPDX licenses like
    `CC0-1.0`, music layers like `L1`) contain `-`/`.` or start with a digit.
    Split on any non-alphanumeric run, PascalCase the parts, and prefix `_` if the
    result would start with a digit. Deterministic and collision-stable.
    """
    parts = []
    cur = []
    for ch in value:
        if ch.isalnum():
            cur.append(ch)
        else:
            if cur:
                parts.append("".join(cur))
                cur = []
    if cur:
        parts.append("".join(cur))
    ident = "".join(p[:1].upper() + p[1:] for p in parts if p)
    if not ident:
        return "Value"
    if ident[0].isdigit():
        ident = "_" + ident
    return ident


# --- loader ----------------------------------------------------------------


def load_common_defs(schema_dir: Path) -> dict[str, Any]:
    """Return the merged shared `$defs` map, per the schema README: validators and
    generators must merge these into each type schema before use.

    Two shared def files are merged, exactly as tools/validate_content.py's
    load_schemas() does: common.defs.yaml (the base vocabulary) and
    skeleton.defs.yaml (meridian/skeleton@1 character vocabulary — geosetRegion,
    attachSocket, dyeChannel, raceName; contract ①/T1). item@2's visual.worn refs
    the skeleton enums, so the generator must resolve them too."""
    defs: dict[str, Any] = {}
    defs.update(
        yaml.safe_load((schema_dir / "common.defs.yaml").read_text(encoding="utf-8"))[
            "$defs"
        ]
    )
    # skeleton.defs.yaml is optional (isolated generator-fixture schema dirs omit it);
    # the real /schema/content always ships it.
    skeleton = schema_dir / "skeleton.defs.yaml"
    if skeleton.exists():
        defs.update(yaml.safe_load(skeleton.read_text(encoding="utf-8"))["$defs"])
    return defs


# The common.defs ref names that resolve to typed id references, and the id
# prefix each targets. Keeping this table explicit (rather than regex-sniffing
# the pattern) keeps lowering deterministic and reviewable.
_CONTENT_REF_DEFS = {
    "npcRef": "npc",
    "itemRef": "item",
    "questRef": "quest",
    "abilityRef": "ability",
    "lootRef": "loot",
    "vendorRef": "vendor",
    "zoneRef": "zone",
    "contentId": "content",  # the file's own fully-qualified id
    # equip_type (pack-contract spec §2.1) is a new content type whose ref defs
    # live locally in their schema files (not common.defs.yaml, owned by a sibling
    # story). equipTypeId is the equip_type entity's own id (generic self-id, like
    # contentId); equipTypeRef is a reference to an equip_type (e.g. item.equip_type).
    "equipTypeId": "content",
    "equipTypeRef": "equip_type",
    # race (pack-contract spec §2.3) is likewise a new content type with a local
    # self-id def (raceId, like equipTypeId/contentId). appearanceRef/attributeRef
    # live in common.defs.yaml and are first consumed by race.schema.yaml — a race
    # references its cosmetic appearance (spec §2.3) and, via attributeMods, tunes
    # attributes (spec §2.2). (raceRef, a reference TO a race, arrives with the
    # class schema's race_limits in a later story.)
    "raceId": "content",
    "appearanceRef": "appearance",
    "attributeRef": "attribute",
    # talent + talent_tree (pack-contract spec §2.5) follow the same local-def
    # pattern as equip_type. talentId/talentTreeId are the entities' own ids
    # (generic self-id, like contentId); talentRef references a talent (e.g.
    # talent_tree.tiers[].talents[]).
    "talentId": "content",
    "talentTreeId": "content",
    "talentRef": "talent",
    # class (pack-contract spec §2.4) is the 7-field integrator; like the other
    # newer content types its ref defs live locally in class.schema.yaml. classId
    # is the class entity's own id (generic self-id, like contentId). raceRef /
    # talentTreeRef are references TO a race / talent_tree, first consumed by a
    # class's race_limits / talent_tree (spec §2.4) — the reference-def counterparts
    # to race's raceId / talent_tree's talentTreeId self-ids.
    "classId": "content",
    "raceRef": "race",
    "talentTreeRef": "talent_tree",
    # dye (contract ① §6) — dyeRef lives in common.defs.yaml and is first consumed
    # by appearance_catalog.body_material.dyes[].dye (chibi colour races, design
    # 2026-07-14-chibi §6/R2): a race's fixed body dye names a meridian/dye@1 by id.
    "dyeRef": "dye",
}
_ASSET_REF_DEFS = {
    "artRef": "art",
    "musRef": "mus",
    "sfxRef": "sfx",
    "ambRef": "amb",
    "assetId": "asset",  # an asset sidecar's own id
}
# Common $defs that are structural objects/scalars rather than id refs.
_STRUCT_DEFS = {"intRange", "position"}
_SCALAR_DEFS = {
    "money": "integer",
    "pct": "number",
    "displayName": "string",
}
# Enum-valued shared $defs. Includes the meridian/skeleton@1 vocabulary enums
# (geosetRegion/attachSocket/dyeChannel/raceName) referenced by item@2 visual.worn.
_ENUM_DEFS = {
    "statKey",
    "school",
    "itemClass",
    "geosetRegion",
    "attachSocket",
    "dyeChannel",
    "raceName",
}


class _Lowerer:
    """Lowers one schema-dir worth of type schemas into an IrModel.

    Nested objects and unions are hoisted into named top-level structs as they
    are encountered; the derived name is the parent struct name + the field name
    (PascalCase), so output names are stable and collision-free within a type.
    """

    def __init__(self, common_defs: dict[str, Any]):
        self.common_defs = common_defs
        self.model = IrModel()
        self._enum_by_name: dict[str, EnumType] = {}
        self._seen_content_refs: dict[str, None] = {}
        self._seen_asset_refs: dict[str, None] = {}

    # -- public entry --

    def lower_type(self, type_name: str, schema: dict[str, Any]) -> None:
        """Lower one root type schema (e.g. npc.schema.yaml -> struct `Npc`)."""
        root_name = pascal(type_name)
        struct = self._lower_object(root_name, schema, schema_type=type_name)
        struct.description = _first_line(schema.get("description"))
        self.model.structs.append(struct)

    def finalize(self) -> IrModel:
        # Deterministic: enums sorted by name; ref kinds in schema-encounter order
        # captured as insertion-ordered dicts.
        self.model.enums = [self._enum_by_name[n] for n in sorted(self._enum_by_name)]
        self.model.content_ref_kinds = list(self._seen_content_refs)
        self.model.asset_ref_kinds = list(self._seen_asset_refs)
        return self.model

    # -- object / field lowering --

    def _lower_object(
        self, name: str, node: dict[str, Any], *, schema_type: str | None = None
    ) -> IrStruct:
        schema_tag = None
        if schema_type is not None:
            schema_tag = node.get("properties", {}).get("schema", {}).get("const")
            if not isinstance(schema_tag, str) or not schema_tag:
                raise SchemaError(
                    f"{name}: root schema must declare a non-empty string properties.schema.const"
                )
        struct = IrStruct(name=name, schema_type=schema_type, schema_tag=schema_tag)
        required = set(node.get("required", []))
        props: dict[str, Any] = node.get("properties", {})
        for key, prop in props.items():
            # The `schema:` envelope const and reserved null fields carry no data
            # worth a typed member beyond documentation; keep `schema` out of the
            # generated struct (it is the type discriminator itself).
            if key == "schema" and schema_type is not None:
                continue
            ir_type = self._lower_property(name, key, prop)
            struct.fields.append(
                IrField(
                    name=key,
                    type=ir_type,
                    required=key in required,
                    description=_first_line(prop.get("description")),
                )
            )
        return struct

    def _lower_property(self, owner: str, key: str, prop: dict[str, Any]) -> IrType:
        # Resolve a $ref into common.defs first.
        ref = prop.get("$ref")
        if ref is not None:
            return self._lower_ref(owner, key, ref, prop)

        # Two oneOf shapes appear in the content schema:
        #  (a) a discriminated union — oneOf of object *variants*, each carrying its
        #      own `properties` (quest objectives, ability effects). -> tagged struct.
        #  (b) a single object with shared `properties` plus a oneOf that only lists
        #      mutually-exclusive `required` keys (loot entry: item XOR table). The
        #      typed shape is just the object; the XOR is a presence rule the linter
        #      enforces (L052 etc.), not a type distinction. -> nested object.
        if "oneOf" in prop and "properties" not in prop:
            return self._lower_union(owner, key, prop["oneOf"])

        json_type = prop.get("type")
        if json_type == "object" or "properties" in prop:
            nested_name = owner + pascal(key)
            nested = self._lower_object(nested_name, prop)
            self.model.structs.append(nested)
            return NestedType(nested_name)
        if json_type == "array":
            element = self._lower_array_items(owner, key, prop.get("items", {}))
            return ArrayType(element)
        if "enum" in prop:
            return self._register_enum(owner + pascal(key), prop["enum"])
        if "const" in prop:
            # A lone const (e.g. content_schema_version: 1) — treat as its scalar.
            const = prop["const"]
            if isinstance(const, bool):
                return ScalarType("boolean")
            if isinstance(const, int):
                return ScalarType("integer")
            return ScalarType("string")
        if json_type in ("string", "integer", "number", "boolean", "null"):
            return ScalarType(json_type)
        raise SchemaError(f"{owner}.{key}: unsupported property shape {prop!r}")

    def _lower_array_items(self, owner: str, key: str, items: dict[str, Any]) -> IrType:
        ref = items.get("$ref")
        if ref is not None:
            return self._lower_ref(owner, key, ref, items)
        # See _lower_property for the two oneOf shapes; same rule applies to array
        # elements (loot `entries[]` = object+oneOf-required; quest `objectives[]`
        # = oneOf of variants).
        if "oneOf" in items and "properties" not in items:
            return self._lower_union(owner, key, items["oneOf"])
        if items.get("type") == "object" or "properties" in items:
            # Singularize an "-ies"/"-s" plural for the element name, best-effort.
            nested_name = owner + pascal(_singular(key))
            nested = self._lower_object(nested_name, items)
            self.model.structs.append(nested)
            return NestedType(nested_name)
        if "enum" in items:
            return self._register_enum(owner + pascal(_singular(key)), items["enum"])
        jt = items.get("type")
        if jt in ("string", "integer", "number", "boolean"):
            return ScalarType(jt)
        raise SchemaError(f"{owner}.{key}[]: unsupported array item {items!r}")

    def _lower_union(
        self, owner: str, key: str, one_of: list[dict[str, Any]]
    ) -> IrType:
        """Lower a oneOf of object variants into a single tagged struct.

        Content oneOf's here (quest objectives, ability effects, loot item-or-table)
        are discriminated either by a const `kind`/`type` field, or by which
        of a mutually-exclusive pair of keys is present (loot: `item` XOR `table`).
        We flatten to one struct: a discriminator enum (when a const tag exists)
        plus the union of all variant fields, each optional (only the fields of the
        active variant are populated). This is the same "tagged struct" shape the
        emitters render, and it keeps every model entry a plain IrStruct.
        """
        union_name = owner + pascal(_singular(key))
        tag_field: str | None = None
        tag_values: list[str] = []
        # Collect fields across variants, de-duplicated by name (first-seen type
        # wins; schema variants that share a field share its type).
        merged: dict[str, IrField] = {}
        for variant in one_of:
            v_props: dict[str, Any] = variant.get("properties", {})
            for pk, pv in v_props.items():
                if "const" in pv:
                    tag_field = pk
                    tag_values.append(str(pv["const"]))
                    continue
                if pk in merged:
                    continue
                ftype = self._lower_property(union_name, pk, pv)
                # Every variant field is optional on the flattened struct.
                merged[pk] = IrField(
                    name=pk,
                    type=ftype,
                    required=False,
                    description=_first_line(pv.get("description")),
                )
        struct = IrStruct(name=union_name, schema_type=None, is_union=True)
        struct.tag_field = tag_field
        if tag_field is not None:
            # Preserve first-seen order, de-duplicated.
            seen: dict[str, None] = {}
            for tv in tag_values:
                seen.setdefault(tv, None)
            enum = self._register_enum(union_name + "Kind", list(seen))
            struct.fields.append(
                IrField(
                    name=tag_field,
                    type=enum,
                    required=True,
                    description="Discriminator.",
                )
            )
        struct.fields.extend(merged.values())
        self.model.structs.append(struct)
        return UnionType(name=union_name, tag_field=tag_field, variants=())

    def _lower_ref(
        self, owner: str, key: str, ref: str, prop: dict[str, Any]
    ) -> IrType:
        # Refs are always "#/$defs/<name>".
        def_name = ref.rsplit("/", 1)[-1]
        if def_name in _CONTENT_REF_DEFS:
            kind = _CONTENT_REF_DEFS[def_name]
            self._seen_content_refs.setdefault(kind, None)
            return RefType(ref_kind=kind, is_asset=False)
        if def_name in _ASSET_REF_DEFS:
            kind = _ASSET_REF_DEFS[def_name]
            self._seen_asset_refs.setdefault(kind, None)
            return RefType(ref_kind=kind, is_asset=True)
        if def_name in _ENUM_DEFS:
            enum_def = self.common_defs[def_name]
            return self._register_enum(pascal(def_name), enum_def["enum"])
        if def_name in _SCALAR_DEFS:
            return ScalarType(_SCALAR_DEFS[def_name])
        if def_name in _STRUCT_DEFS:
            # Lower the shared object def as a named nested struct, reused by name.
            struct_name = pascal(def_name)
            if not any(s.name == struct_name for s in self.model.structs):
                nested = self._lower_object(struct_name, self.common_defs[def_name])
                nested.description = _first_line(
                    self.common_defs[def_name].get("description")
                )
                self.model.structs.append(nested)
            return NestedType(struct_name)
        if def_name == "attributeMods":
            # attributeMods (common.defs §attributeMods, spec §2.2) is a shared
            # ARRAY def — a list of {attribute, value} — reused by race and class.
            # Lower it to a vector of a single shared `AttributeMod` element struct,
            # declared once (like IntRange/Position, but array-valued) so every
            # consumer shares the element type.
            element_name = "AttributeMod"
            if not any(s.name == element_name for s in self.model.structs):
                item_schema = self.common_defs[def_name]["items"]
                nested = self._lower_object(element_name, item_schema)
                self.model.structs.append(nested)
            return ArrayType(NestedType(element_name))
        raise SchemaError(f"{owner}.{key}: unknown $ref {ref}")

    def _register_enum(self, name: str, values: list[str]) -> EnumType:
        et = EnumType(name=name, values=tuple(values))
        existing = self._enum_by_name.get(name)
        if existing is not None and existing.values != et.values:
            raise SchemaError(f"enum name collision with differing values: {name}")
        self._enum_by_name[name] = et
        return et


def _first_line(desc: Any) -> str | None:
    if not isinstance(desc, str):
        return None
    line = " ".join(desc.split())
    return line or None


def _singular(word: str) -> str:
    """Best-effort singularize a field name for element/variant naming."""
    if word.endswith("ies"):
        return word[:-3] + "y"
    if word.endswith("sses") or word.endswith("ches") or word.endswith("shes"):
        return word[:-2]
    if word.endswith("s") and not word.endswith("ss"):
        return word[:-1]
    return word


def build_model(schema_dir: Path) -> IrModel:
    """Load every type schema in `schema_dir` and lower it to an IrModel.

    Iterates schema files in a fixed order (KNOWN_TYPES) so struct/enum ordering
    in the output is deterministic regardless of filesystem glob order.
    """
    common = load_common_defs(schema_dir)
    lowerer = _Lowerer(common)
    # Deterministic type order: the canonical KNOWN_TYPES first (in that order),
    # then any other *.schema.yaml alphabetically (so fixtures/new types are stable
    # without touching KNOWN_TYPES).
    present = {p.name.split(".")[0]: p for p in schema_dir.glob("*.schema.yaml")}
    ordered = [t for t in KNOWN_TYPES if t in present]
    ordered += sorted(t for t in present if t not in KNOWN_TYPES)
    for type_name in ordered:
        schema = yaml.safe_load(present[type_name].read_text(encoding="utf-8"))
        lowerer.lower_type(type_name, schema)
    return lowerer.finalize()
