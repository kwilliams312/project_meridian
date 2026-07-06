#!/usr/bin/env python3
"""Reference validator for the Meridian content database (TLS-07, lint stage 1).

Validates every YAML file under /content against the JSON Schemas in
/schema/content, then cross-checks references and semantics:

  PARSE  file is malformed YAML or not a mapping (reported, never a crash)
  L001   file suffix matches the declared envelope type
  L002   entity id namespace matches its pack's namespace
  L003   entity id type segment matches the file's schema type
  L004   intRange min <= max (JSON Schema cannot express this)
  L010   no duplicate ids across the content tree
  L011   every content reference (npc./item./quest./...) resolves to a defined id
  L020   every asset reference (art./mus./sfx./amb.) resolves to an IF-8 sidecar
         (severity via --assets: warn [default] | error | ignore — interim posture
         per Tools SAD §4.3 until the first art drop)
  L021   asset sidecar provenance is complete + internally consistent (TD-09;
         Art PRD §3, Art SAD §3.2) — always a hard error
  L022   asset sidecar license is on the CI allowlist and its origin is not an
         engine-locked marketplace (TD-09; Art PRD §3, "fails CI outright") —
         always a hard error
  L070   declared LOD0 tri count is within the Art PRD §2.1 class ceiling — error
  L071   declared texture dimension is within the Art PRD §2.3 class cap — error
  L072   declared material-set count is within the Art PRD §2.3/§2.4 cap — error
  L034   explore objectives reference a POI defined in the zone manifest
  L035   quest giver / turn-in / deliver NPCs have a spawn point (warn)
  L052   loot-table nesting exceeds one level (Tools PRD §4.4)
  L062   vendor item has neither item price.buy nor a price_override

L021/L022 provenance completeness + license/origin policy and L070-072 budget
caps are enforced here now (TD-09 / Art PRD §3, §2.1/§2.3/§2.4; issue #142). The
asset schema still enforces the conditional provenance fields where JSON Schema
can — these lints add the semantic policy (allowlist, engine-locked-origin
denylist, per-class budget rows) and named rule ids on top. Sidecar-source
existence in LFS (the remaining L021 scope) still arrives with `mcc`.

This is the stopgap CI gate until `mcc` (C++) subsumes it; keep rule ids stable
(they match the Tools SAD §2.2 lint bands).

Usage: python3 tools/validate_content.py [--root <repo-root>] [--assets warn|error|ignore]
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import yaml
from jsonschema import Draft202012Validator

CONTENT_TYPES = ("npc", "item", "quest", "ability", "loot", "vendor", "spawn", "zone")
ASSET_PREFIXES = ("art", "mus", "sfx", "amb")

# --- Provenance / license policy (TD-09; Art PRD §3, Art SAD §3.2) ------------
# License allowlist: original art is CC-BY-4.0, third-party must be CC0 or CC-BY.
# Anything else "fails CI outright" (Art PRD §3, line 164). Matches the schema
# `license` enum, restated here so L022 is a lint in its own right and does not
# depend on the schema enum staying tight.
PERMITTED_LICENSES = ("CC0-1.0", "CC-BY-4.0")

# Engine-locked marketplace origins are disallowed regardless of the license tag:
# their terms bind assets to a specific engine and are incompatible with the open,
# redistributable Godot stack (TD-09; Art PRD §3 / §3.1, D-18 §60). Matched as a
# case-insensitive substring against provenance.origin_url.
ENGINE_LOCKED_ORIGINS = (
    "quixel.com",
    "megascans",
    "fab.com",
    "unrealengine.com",
    "unreal marketplace",
    "assetstore.unity.com",
    "unity asset store",
)

# source_tier -> extra provenance fields required beyond {source_tier, authors}.
# Mirrors the schema's conditional allOf (asset.schema.yaml) so the lint gives a
# named rule id + human message rather than a raw JSON-Schema failure, and so the
# requirement survives even if the schema is loosened. (Art SAD §3.1 field table.)
PROVENANCE_TIER_REQUIRES = {
    "original": (),
    "ai": ("origin_url", "ai"),
    "cc0": ("origin_url", "license_verified_on"),
    "cc_by": ("origin_url", "attribution", "license_verified_on"),
}

# --- Per-asset budget table (Art PRD §2.1 geometry, §2.3 texture, §2.4 material).
# Keyed by the sidecar `class` enum. Each row: the hard ceiling for a declared
# budget field, with the PRD cite. Ranges in the PRD ("45-60k") take the TOP of the
# band as the LOD0 ceiling; classes with an explicit upper cap use it. Classes not
# listed for a metric are unconstrained for that metric (no PRD row -> no check).
# Every number is a direct PRD figure — see the cite string on each row.
ASSET_BUDGETS: dict[str, dict[str, tuple[int, str]]] = {
    # class:            metric        (ceiling, PRD cite)
    "character_model": {
        "lod0_tris": (60_000, "Art PRD §2.1 player character body 45-60k"),
        "texture_max_px": (2048, "Art PRD §2.3 player character body 2048² set"),
        "vram_mb": (24, "Art PRD §2.3 player character body ≤ 24 MB"),
    },
    "armor_model": {
        "lod0_tris": (8_000, "Art PRD §2.1 armor set piece 3-8k per slot"),
        "texture_max_px": (2048, "Art PRD §2.3 armor set shared 2048²"),
        "vram_mb": (32, "Art PRD §2.3 armor set (8 slots) ≤ 32 MB"),
    },
    "kit_piece": {
        "lod0_tris": (
            20_000,
            "Art PRD §2.1 environment kit piece ≤ 20k large set pieces",
        ),
        "texture_max_px": (2048, "Art PRD §2.3 environment kit tiling 2048² materials"),
        "material_sets": (12, "Art PRD §2.3/§2.4 ≤ 12 unique material sets per kit"),
        "vram_mb": (224, "Art PRD §2.3 environment kit ≤ 224 MB per kit resident"),
    },
    "weapon_model": {
        "lod0_tris": (
            15_000,
            "Art PRD §2.1 weapon 6-12k; 2-hander/legendary up to 15k",
        ),
        "texture_max_px": (2048, "Art PRD §2.3 weapon 1024² (2048² for legendary)"),
        "vram_mb": (6, "Art PRD §2.3 weapon ≤ 6 MB"),
    },
    "creature_model": {
        "lod0_tris": (90_000, "Art PRD §2.1 large creature/boss 60-90k"),
        "texture_max_px": (4096, "Art PRD §2.3 boss 4096² allowed"),
        "vram_mb": (64, "Art PRD §2.3 boss ≤ 64 MB"),
    },
    "prop": {
        "lod0_tris": (3_000, "Art PRD §2.1 prop, small 0.5-3k"),
        "texture_max_px": (1024, "Art PRD §2.3 prop 512²-1024²"),
        "vram_mb": (4, "Art PRD §2.3 prop ≤ 4 MB"),
    },
    "foliage": {
        "lod0_tris": (20_000, "Art PRD §2.1 foliage: tree 10-20k"),
    },
    "hero_landmark": {
        "lod0_tris": (80_000, "Art PRD §2.1 hero landmark / POI piece 40-80k"),
    },
}
REF_RE = re.compile(
    r"^(?:([a-z][a-z0-9_]{1,31}):)?"
    r"((npc|item|quest|ability|loot|vendor|spawn|zone)\.[a-z0-9_]+(?:\.[a-z0-9_]+)*)$"
)
ASSET_RE = re.compile(
    r"^(?:([a-z][a-z0-9_]{1,31}):)?((art|mus|sfx|amb)\.[a-z0-9_]+(?:\.[a-z0-9_]+)*)$"
)


@dataclass
class Result:
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    stats: dict[str, int] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return not self.errors


def load_schemas(schema_dir: Path) -> dict[str, Draft202012Validator]:
    common = yaml.safe_load(
        (schema_dir / "common.defs.yaml").read_text(encoding="utf-8")
    )
    validators: dict[str, Draft202012Validator] = {}
    for path in sorted(schema_dir.glob("*.schema.yaml")):
        schema = yaml.safe_load(path.read_text(encoding="utf-8"))
        # Contract from schema/content/README.md: merge shared $defs before use.
        schema.setdefault("$defs", {}).update(common["$defs"])
        Draft202012Validator.check_schema(schema)
        type_name = path.name.split(".")[0]
        validators[type_name] = Draft202012Validator(schema)
    return validators


def file_type(path: Path) -> str | None:
    if path.name == "pack.yaml":
        return "pack"
    parts = path.name.split(".")
    if len(parts) == 3 and parts[2] == "yaml" and parts[1] in (*CONTENT_TYPES, "asset"):
        return parts[1]
    return None


def walk_strings(node, path="$"):
    """Yield (json-path, value) for every string; skips the top-level `id` (the definition)."""
    if isinstance(node, dict):
        for key, value in node.items():
            if path == "$" and key == "id":
                continue
            yield from walk_strings(value, f"{path}.{key}")
    elif isinstance(node, list):
        for i, value in enumerate(node):
            yield from walk_strings(value, f"{path}[{i}]")
    elif isinstance(node, str):
        yield path, node


def walk_ranges(node, path="$"):
    """Yield (json-path, min, max) for every {min, max} numeric pair (intRange shape)."""
    if isinstance(node, dict):
        lo, hi = node.get("min"), node.get("max")
        if isinstance(lo, (int, float)) and isinstance(hi, (int, float)):
            yield path, lo, hi
        for key, value in node.items():
            yield from walk_ranges(value, f"{path}.{key}")
    elif isinstance(node, list):
        for i, value in enumerate(node):
            yield from walk_ranges(value, f"{path}[{i}]")


def _resolve(ref: str, namespace: str) -> str:
    return ref if ":" in ref else f"{namespace}:{ref}"


def check_provenance(doc: dict, rel_path: Path) -> list[str]:
    """L021/L022 — provenance completeness/consistency + license/origin policy.

    Hard errors, independent of the schema's structural checks (TD-09; Art PRD §3,
    Art SAD §3.2). Assumes `doc` already passed schema validation, so the core
    shape (license, provenance{source_tier, authors}) is present; this layer adds
    the semantic policy the schema cannot fully express and gives named rule ids.
    """
    errors: list[str] = []
    prov = doc.get("provenance", {})

    # L022 — license allowlist. Original art is CC-BY-4.0; third-party must be
    # CC0/CC-BY. Anything else fails CI outright (Art PRD §3).
    license_id = doc.get("license")
    if license_id not in PERMITTED_LICENSES:
        errors.append(
            f"L022 {rel_path}: license '{license_id}' is not on the CI allowlist "
            f"({', '.join(PERMITTED_LICENSES)}) — TD-09 / Art PRD §3"
        )

    # L022 — engine-locked marketplace origins are disallowed regardless of license.
    origin = str(prov.get("origin_url", "")).lower()
    for marker in ENGINE_LOCKED_ORIGINS:
        if marker in origin:
            errors.append(
                f"L022 {rel_path}: origin_url names an engine-locked marketplace "
                f"('{marker}') — engine-locked content is disallowed (TD-09 / Art PRD §3.1)"
            )
            break

    # L021 — provenance completeness per source tier.
    tier = prov.get("source_tier")
    required = PROVENANCE_TIER_REQUIRES.get(tier)
    if required is None:
        # source_tier absent/unknown (schema should have caught it) — flag anyway.
        errors.append(
            f"L021 {rel_path}: provenance.source_tier '{tier}' is missing or unknown "
            f"(expected one of {', '.join(PROVENANCE_TIER_REQUIRES)}) — TD-09"
        )
    else:
        for field_name in required:
            if not prov.get(field_name):
                errors.append(
                    f"L021 {rel_path}: provenance is incomplete for source_tier "
                    f"'{tier}': missing '{field_name}' (Art SAD §3.1 / TD-09)"
                )
        if tier == "ai":
            ai = prov.get("ai") or {}
            for ai_field in ("tool", "prompts_file"):
                if not ai.get(ai_field):
                    errors.append(
                        f"L021 {rel_path}: AI-tier provenance missing 'ai.{ai_field}' "
                        f"(prompt hygiene, Art PRD §3.2)"
                    )

    # L021 — authors must be non-empty (schema enforces minItems:1, restated so a
    # loosened schema still fails here).
    if not prov.get("authors"):
        errors.append(
            f"L021 {rel_path}: provenance.authors is empty — every asset names an "
            f"author (TD-09)"
        )

    return errors


def check_budget(doc: dict, rel_path: Path) -> list[str]:
    """L070/L071/L072 — declared budget figures vs the Art PRD class ceilings.

    Only checks fields the sidecar actually declares (the `budget` block is
    optional until the LFS source drop, Art SAD §4.2). Every threshold is a direct
    Art PRD §2.1/§2.3/§2.4 number, carried in ASSET_BUDGETS with its cite.
    """
    errors: list[str] = []
    budget = doc.get("budget")
    if not budget:
        return errors

    asset_class = doc.get("class")
    caps = ASSET_BUDGETS.get(asset_class, {})

    checks = (
        ("lod0_tris", "L070", "tri"),
        ("texture_max_px", "L071", "px"),
        ("material_sets", "L072", "material set"),
        ("vram_mb", "L071", "MB VRAM"),
    )
    for metric, rule, unit in checks:
        declared = budget.get(metric)
        if declared is None:
            continue
        cap_row = caps.get(metric)
        if cap_row is None:
            continue  # no PRD ceiling for this class/metric -> unconstrained
        ceiling, cite = cap_row
        if declared > ceiling:
            errors.append(
                f"{rule} {rel_path}: {metric} {declared} {unit} exceeds the "
                f"{ceiling} ceiling for class '{asset_class}' ({cite})"
            )

    return errors


def validate(content_dir: Path, schema_dir: Path, assets_mode: str = "warn") -> Result:
    validators = load_schemas(schema_dir)
    res = Result()
    root = content_dir.parent

    ids: dict[str, Path] = {}  # every entity + asset id -> defining file
    content_ids: set[str] = set()  # ids resolvable by content refs (L011)
    asset_ids: set[str] = set()  # ids resolvable by asset refs (L020)
    docs_by_id: dict[str, dict] = {}  # content docs for semantic checks
    refs: list[tuple[Path, str, str]] = []  # (file, location, normalized content ref)
    asset_refs: list[
        tuple[Path, str, str]
    ] = []  # (file, location, normalized asset ref)
    pack_namespaces: dict[Path, str] = {}
    spawn_namespaces: set[str] = set()
    files = sorted(content_dir.rglob("*.yaml"))

    def rel(path: Path) -> Path:
        try:
            return path.relative_to(root)
        except ValueError:
            return path

    # Pass 1: parse, schema-validate, collect ids and references.
    parsed: list[tuple[Path, str, dict]] = []
    for path in files:
        ftype = file_type(path)
        if ftype is None:
            res.errors.append(
                f"L001 {rel(path)}: filename must be '<name>.<type>.yaml' or 'pack.yaml'"
            )
            continue
        try:
            doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError as exc:
            mark = getattr(exc, "problem_mark", None)
            where = f" (line {mark.line + 1})" if mark else ""
            res.errors.append(f"PARSE {rel(path)}: invalid YAML{where}")
            continue
        if not isinstance(doc, dict):
            res.errors.append(f"PARSE {rel(path)}: document is not a YAML mapping")
            continue
        declared = doc.get("schema", "")
        if declared != f"meridian/{ftype}@1":
            res.errors.append(
                f"L001 {rel(path)}: declares '{declared}', expected 'meridian/{ftype}@1'"
            )
            continue
        schema_errors = sorted(
            validators[ftype].iter_errors(doc), key=lambda e: e.json_path
        )
        if schema_errors:
            for err in schema_errors:
                res.errors.append(
                    f"SCHEMA {rel(path)} at {err.json_path}: {err.message}"
                )
            continue
        parsed.append((path, ftype, doc))

    for path, ftype, doc in parsed:
        if ftype == "pack":
            pack_namespaces[path.parent] = doc["namespace"]
            continue

        doc_id = doc["id"]
        namespace, local = doc_id.split(":", 1)

        # L003 — id type segment must match the file's schema type.
        type_segment = local.split(".", 1)[0]
        if ftype == "asset":
            if type_segment not in ASSET_PREFIXES:
                res.errors.append(
                    f"L003 {rel(path)}: asset id must use an "
                    f"{'/'.join(ASSET_PREFIXES)} prefix, got '{type_segment}'"
                )
                continue
        elif type_segment != ftype:
            res.errors.append(
                f"L003 {rel(path)}: id type segment '{type_segment}' does not match schema type '{ftype}'"
            )
            continue

        # L010 — duplicates (first definition wins for resolution).
        if doc_id in ids:
            res.errors.append(
                f"L010 {rel(path)}: duplicate id '{doc_id}' (also in {rel(ids[doc_id])})"
            )
            continue
        ids[doc_id] = path

        # L004 — intRange sanity.
        for loc, lo, hi in walk_ranges(doc):
            if lo > hi:
                res.errors.append(f"L004 {rel(path)} at {loc}: min ({lo}) > max ({hi})")

        if ftype == "asset":
            asset_ids.add(doc_id)
            # L021/L022 provenance + license/origin policy, L070-072 budget caps.
            res.errors.extend(check_provenance(doc, rel(path)))
            res.errors.extend(check_budget(doc, rel(path)))
            continue  # sidecar-internal refs (stem_set, variation_group) are metadata, not L011/L020 refs

        content_ids.add(doc_id)
        docs_by_id[doc_id] = doc
        if ftype == "spawn":
            spawn_namespaces.add(namespace)

        for loc, value in walk_strings(doc):
            m = REF_RE.match(value)
            if m:
                refs.append((path, loc, _resolve(m.group(2), m.group(1) or namespace)))
                continue
            m = ASSET_RE.match(value)
            if m:
                asset_refs.append(
                    (path, loc, _resolve(m.group(2), m.group(1) or namespace))
                )

    # Pass 2: namespace ownership (L002) — nearest ancestor pack.yaml owns the file.
    for doc_id, path in ids.items():
        namespace = doc_id.split(":", 1)[0]
        owner = next(
            (ns for d, ns in pack_namespaces.items() if path.is_relative_to(d)), None
        )
        if owner is None:
            res.errors.append(
                f"L002 {rel(path)}: no pack.yaml found in ancestor directories"
            )
        elif owner != namespace:
            res.errors.append(
                f"L002 {rel(path)}: id namespace '{namespace}' but pack namespace is '{owner}'"
            )

    # Pass 3: reference resolution (L011 content, L020 assets).
    for path, loc, ref in refs:
        if ref not in content_ids:
            res.errors.append(
                f"L011 {rel(path)} at {loc}: unresolved reference '{ref}'"
            )
    if assets_mode != "ignore":
        sink = res.errors if assets_mode == "error" else res.warnings
        for path, loc, ref in asset_refs:
            if ref not in asset_ids:
                sink.append(
                    f"L020 {rel(path)} at {loc}: asset reference '{ref}' has no IF-8 sidecar"
                )

    # Pass 4: semantic lints over resolved content.
    def doc_of(ref: str, namespace: str) -> dict | None:
        return docs_by_id.get(_resolve(ref, namespace))

    spawned_npcs: set[str] = set()
    loot_children: dict[str, set[str]] = {}
    for doc_id, doc in docs_by_id.items():
        namespace = doc_id.split(":", 1)[0]
        dtype = doc_id.split(":", 1)[1].split(".", 1)[0]
        if dtype == "spawn":
            for spawn in doc.get("spawns", []):
                spawned_npcs.add(_resolve(spawn["npc"], namespace))
        elif dtype == "loot":
            children = {
                _resolve(e["table"], namespace)
                for e in doc.get("entries", [])
                if "table" in e
            } | {
                _resolve(e["table"], namespace)
                for g in doc.get("groups", [])
                for e in g.get("entries", [])
                if "table" in e
            }
            if children:
                loot_children[doc_id] = children

    for doc_id, doc in docs_by_id.items():
        namespace = doc_id.split(":", 1)[0]
        dtype = doc_id.split(":", 1)[1].split(".", 1)[0]
        path = ids[doc_id]

        if dtype == "quest":
            # L034 — explore objectives must name a POI the zone manifest defines.
            for i, obj in enumerate(doc.get("objectives", [])):
                if obj.get("type") != "explore":
                    continue
                zone_doc = doc_of(obj["zone"], namespace)
                if zone_doc is None:
                    continue  # L011 already reported the dangling zone ref
                poi_ids = {p["id"] for p in zone_doc.get("pois", [])}
                if obj["poi"] not in poi_ids:
                    res.errors.append(
                        f"L034 {rel(path)} at $.objectives[{i}]: POI '{obj['poi']}' "
                        f"not defined in zone '{_resolve(obj['zone'], namespace)}'"
                    )
            # L035 — giver/turn-in/deliver NPCs must spawn somewhere (warn; only
            # meaningful once the namespace has any spawn files at all).
            if namespace in spawn_namespaces:
                npc_refs = {doc["giver"], doc.get("turn_in", doc["giver"])}
                npc_refs |= {
                    o["to"]
                    for o in doc.get("objectives", [])
                    if o.get("type") == "deliver"
                }
                for npc_ref in sorted(npc_refs):
                    full = _resolve(npc_ref, namespace)
                    if full in content_ids and full not in spawned_npcs:
                        res.warnings.append(
                            f"L035 {rel(path)}: quest NPC '{full}' has no spawn point in any spawn file"
                        )

        elif dtype == "loot":
            # L052 — one level of nesting: a nested table may not itself reference tables.
            for child in sorted(loot_children.get(doc_id, ())):
                if loot_children.get(child):
                    res.errors.append(
                        f"L052 {rel(path)}: nested table '{child}' itself references tables "
                        f"(one level of nesting allowed, Tools PRD §4.4)"
                    )

        elif dtype == "vendor":
            # L062 — every sold item needs a buy price from one side or the other.
            for i, entry in enumerate(doc.get("items", [])):
                if "price_override" in entry:
                    continue
                item_doc = doc_of(entry["item"], namespace)
                if (
                    item_doc is not None
                    and item_doc.get("price", {}).get("buy") is None
                ):
                    res.errors.append(
                        f"L062 {rel(path)} at $.items[{i}]: '{_resolve(entry['item'], namespace)}' "
                        f"has no price.buy and the vendor sets no price_override"
                    )

    res.stats = {
        "files": len(files),
        "entities": len(content_ids),
        "assets": len(asset_ids),
        "content_refs": len(refs),
        "asset_refs": len(asset_refs),
    }
    return res


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parent.parent
    )
    parser.add_argument(
        "--assets",
        choices=("warn", "error", "ignore"),
        default="warn",
        help="severity for L020 asset-sidecar checks (default: warn)",
    )
    args = parser.parse_args()

    res = validate(args.root / "content", args.root / "schema" / "content", args.assets)

    s = res.stats
    print(
        f"Validated {s['files']} files: {s['entities']} entities, {s['assets']} asset sidecars, "
        f"{s['content_refs']} content refs, {s['asset_refs']} asset refs."
    )
    if res.warnings:
        print(f"\n{len(res.warnings)} warning(s):")
        for w in res.warnings:
            print(f"  {w}")
    if res.errors:
        print(f"\n{len(res.errors)} error(s):")
        for e in res.errors:
            print(f"  {e}")
        return 1
    print("OK — schema-valid, all references resolve.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
