#!/usr/bin/env python3
"""Reference validator for the Meridian content database (TLS-07, lint stage 1).

Validates every YAML file under /content against the JSON Schemas in
/schema/content, then cross-checks references:

  L001  file suffix matches the declared envelope type
  L002  entity id namespace matches its pack's namespace
  L010  no duplicate ids across the content tree
  L011  every content reference (npc./item./quest./...) resolves to a defined id

Asset references (art./mus./sfx.) are grammar-checked by the schemas; existence
checks arrive with the asset registry (Baseline §5.3) and are reported as info.

This is the stopgap CI gate until `mcc` (C++) subsumes it; keep rule ids stable.

Usage: py tools/validate_content.py [--root <repo-root>]
"""

import argparse
import re
import sys
from pathlib import Path

import yaml
from jsonschema import Draft202012Validator
from jsonschema.exceptions import best_match

CONTENT_TYPES = ("npc", "item", "quest", "ability", "loot", "vendor", "spawn", "zone")
REF_RE = re.compile(
    r"^(?:([a-z][a-z0-9_]{1,31}):)?"
    r"((npc|item|quest|ability|loot|vendor|spawn|zone)\.[a-z0-9_]+(?:\.[a-z0-9_]+)*)$"
)
ASSET_RE = re.compile(
    r"^(?:([a-z][a-z0-9_]{1,31}):)?((art|mus|sfx)\.[a-z0-9_]+(?:\.[a-z0-9_]+)*)$"
)


def load_schemas(schema_dir: Path) -> dict:
    common = yaml.safe_load((schema_dir / "common.defs.yaml").read_text(encoding="utf-8"))
    validators = {}
    for path in sorted(schema_dir.glob("*.schema.yaml")):
        if path.name == "common.defs.yaml":
            continue
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
    if len(parts) == 3 and parts[1] in CONTENT_TYPES and parts[2] == "yaml":
        return parts[1]
    return None


def walk_strings(node, path="$"):
    if isinstance(node, dict):
        for key, value in node.items():
            if path == "$" and key == "id":
                continue  # the definition itself, not a reference
            yield from walk_strings(value, f"{path}.{key}")
    elif isinstance(node, list):
        for i, value in enumerate(node):
            yield from walk_strings(value, f"{path}[{i}]")
    elif isinstance(node, str):
        yield path, node


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()

    schema_dir = args.root / "schema" / "content"
    content_dir = args.root / "content"
    validators = load_schemas(schema_dir)

    errors: list[str] = []
    ids: dict[str, Path] = {}
    refs: list[tuple[Path, str, str]] = []  # (file, location, normalized ref)
    asset_refs: set[str] = set()
    pack_namespaces: dict[Path, str] = {}  # pack dir -> namespace
    files = sorted(content_dir.rglob("*.yaml"))

    # Pass 1: schema validation, id collection.
    for path in files:
        rel = path.relative_to(args.root)
        ftype = file_type(path)
        if ftype is None:
            errors.append(f"L001 {rel}: filename must be '<name>.<type>.yaml' or 'pack.yaml'")
            continue
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        declared = doc.get("schema", "")
        if declared != f"meridian/{ftype}@1":
            errors.append(f"L001 {rel}: declares '{declared}', expected 'meridian/{ftype}@1'")
            continue
        validator = validators.get(ftype)
        schema_errors = sorted(validator.iter_errors(doc), key=lambda e: e.json_path)
        if schema_errors:
            for err in schema_errors:
                err = best_match([err]) or err
                errors.append(f"SCHEMA {rel} at {err.json_path}: {err.message}")
            continue

        if ftype == "pack":
            pack_namespaces[path.parent] = doc["namespace"]
            continue

        doc_id = doc["id"]
        if doc_id in ids:
            errors.append(f"L010 {rel}: duplicate id '{doc_id}' (also in {ids[doc_id].relative_to(args.root)})")
        ids[doc_id] = path

        namespace = doc_id.split(":", 1)[0]
        for loc, value in walk_strings(doc):
            m = REF_RE.match(value)
            if m:
                ref_ns = m.group(1) or namespace
                refs.append((path, loc, f"{ref_ns}:{m.group(2)}"))
                continue
            m = ASSET_RE.match(value)
            if m:
                asset_refs.add(f"{(m.group(1) or namespace)}:{m.group(2)}")

    # Pass 2: namespace ownership (L002) — nearest ancestor pack.yaml owns the file.
    for doc_id, path in ids.items():
        namespace = doc_id.split(":", 1)[0]
        owner = next(
            (ns for d, ns in pack_namespaces.items() if path.is_relative_to(d)), None
        )
        if owner is None:
            errors.append(f"L002 {path.relative_to(args.root)}: no pack.yaml found in ancestor directories")
        elif owner != namespace:
            errors.append(
                f"L002 {path.relative_to(args.root)}: id namespace '{namespace}' "
                f"but pack namespace is '{owner}'"
            )

    # Pass 3: reference resolution (L011).
    for path, loc, ref in refs:
        if ref not in ids:
            errors.append(f"L011 {path.relative_to(args.root)} at {loc}: unresolved reference '{ref}'")

    print(f"Validated {len(files)} files: {len(ids)} entities, "
          f"{len(refs)} content refs, {len(asset_refs)} distinct asset refs "
          f"(asset existence checks pending asset registry).")
    if errors:
        print(f"\n{len(errors)} error(s):")
        for e in errors:
            print(f"  {e}")
        return 1
    print("OK — schema-valid, all content references resolve.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
