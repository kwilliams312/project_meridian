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
  L034   explore objectives reference a POI defined in the zone manifest
  L035   quest giver / turn-in / deliver NPCs have a spawn point (warn)
  L052   loot-table nesting exceeds one level (Tools PRD §4.4)
  L062   vendor item has neither item price.buy nor a price_override

L021/L022 (sidecar source existence in LFS, provenance completeness on release
branches) arrive with `mcc`; the asset schema already enforces the conditional
provenance fields where JSON Schema can.

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
