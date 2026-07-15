#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Staged-model coverage gate (story #569).

The client renders equipped gear and characters by loading model .glb bytes from
the STAGED client pack (``client/project/meridian/core/...``) via
``ContentDB.model_path(id) → res://meridian/core/<id-path>.scn`` with a .glb
fallback next to it. At M0 the pack is DECLARATIVE (no Godot importer), so those
bytes only exist if the source .glb was hand-staged (``scripts/check-golden.sh``
STAGED_ART). PR #581 shipped six Warden's Kit plates whose bytes were committed to
the CONTENT tree but never staged → every plate hit ``_load_model_scene``'s
``_fail`` path and the kit was invisible in-engine, yet every CI gate stayed green.

This gate closes that hole: it collects every model asset id a renderable content
doc references (item ``visual.model`` / ``visual.worn.models[].model`` /
``race_overrides``, NPC/creature ``visual.model``, appearance ``skeleton`` /
``body_model`` / preset ``model`` entries) and, for any whose asset sidecar names a
source .glb that ACTUALLY EXISTS
on disk, requires a staged .glb at the by-id path. A model whose source is still a
placeholder (no committed bytes — NPC/creature blockouts at M0) is skipped: it is
unavailable everywhere, not a staging regression. So the gate fails precisely on
"committed real model bytes but forgot to stage them", never on known placeholders.

Usage:
    python3 tools/check_staged_models.py [--content content] [--staged client/project/meridian/core]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml


def _iter_docs(content_dir: Path, suffix: str):
    for path in sorted(content_dir.rglob(f"*.{suffix}.yaml")):
        try:
            doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError:
            continue
        if isinstance(doc, dict):
            yield path, doc


def _asset_source_index(content_dir: Path) -> dict[str, tuple[Path, str]]:
    """id -> (pack_root, source) for every asset sidecar (source is pack-root-relative)."""
    index: dict[str, tuple[Path, str]] = {}
    for path, doc in _iter_docs(content_dir, "asset"):
        aid = doc.get("id")
        source = doc.get("source")
        if not aid or not source:
            continue
        # pack root = the dir holding pack.yaml nearest above the sidecar; sidecars
        # live at <pack_root>/assets/**, so climb until the 'assets' segment's parent.
        pack_root = path
        for parent in path.parents:
            if (parent / "pack.yaml").is_file():
                pack_root = parent
                break
        index[aid] = (pack_root, source)
    return index


def _id_to_staged_glb(asset_id: str) -> str:
    """core:art.item.armor.warden_chest -> art/item/armor/warden_chest.glb (by-id layout)."""
    local = asset_id.split(":", 1)[-1]
    return local.replace(".", "/") + ".glb"


def _referenced_model_ids(content_dir: Path) -> set[str]:
    """Model (mesh) asset ids referenced by renderable content — items + NPCs + appearance."""
    ids: set[str] = set()

    for _path, doc in _iter_docs(content_dir, "item"):
        visual = doc.get("visual") or {}
        if isinstance(visual.get("model"), str):
            ids.add(visual["model"])
        worn = visual.get("worn") or {}
        for m in worn.get("models") or []:
            if isinstance(m, dict) and isinstance(m.get("model"), str):
                ids.add(m["model"])
        for _race, ov in (worn.get("race_overrides") or {}).items():
            for m in (ov or {}).get("models") or []:
                if isinstance(m, dict) and isinstance(m.get("model"), str):
                    ids.add(m["model"])

    # NPCs / creatures declare their mesh the same way an item does — visual.model
    # (npc.schema.yaml). Inert today (every NPC/creature model is a byteless M0
    # placeholder, so the source-bytes check in check() skips them), but collecting
    # them keeps the gate honest with its own docstring: it must cover a real NPC
    # .glb the day one is staged, exactly as it covers item plates.
    for _path, doc in _iter_docs(content_dir, "npc"):
        visual = doc.get("visual") or {}
        if isinstance(visual.get("model"), str):
            ids.add(visual["model"])

    for _path, doc in _iter_docs(content_dir, "appearance"):
        for key in ("skeleton", "body_model"):
            if isinstance(doc.get(key), str):
                ids.add(doc[key])
        presets = doc.get("presets") or {}
        for _name, entries in presets.items():
            for e in entries or []:
                if isinstance(e, dict) and isinstance(e.get("model"), str):
                    ids.add(e["model"])

    # Keep only art.* asset ids (drop any stray non-asset refs).
    return {i for i in ids if i.split(":", 1)[-1].startswith("art.")}


def check(content_dir: Path, staged_dir: Path) -> list[str]:
    """Return a list of failure messages (empty == pass)."""
    sources = _asset_source_index(content_dir)
    # This gate polices ONE staged mount (client/project/meridian/<ns>), and each
    # pack mounts under its own res://meridian/<namespace> (ContentDB.DEFAULT_PACK_DIR
    # is per-pack). So a model referenced by content in ANOTHER pack belongs to that
    # pack's mount, not this one — requiring a chibi body under the core mount would
    # be wrong (the client resolves chibi ids under res://meridian/chibi, set up by
    # its own staging in a later story). Scope the coverage to models whose namespace
    # matches the staged mount's (its dir name), so cross-pack refs are that pack's
    # mount's responsibility. Same-pack (core→core) behaviour is unchanged.
    staged_ns = staged_dir.name
    failures: list[str] = []
    for asset_id in sorted(_referenced_model_ids(content_dir)):
        if asset_id.split(":", 1)[0] != staged_ns:
            continue  # a different pack's mount owns this model (see note above)
        entry = sources.get(asset_id)
        if entry is None:
            continue  # unresolved ref — validate_content/L020 owns that report
        pack_root, source = entry
        source_path = pack_root / source
        if source_path.suffix.lower() != ".glb":
            continue  # not a mesh model (texture/palette ref) — not staged as .glb
        if not source_path.is_file():
            continue  # placeholder: no committed bytes yet, unavailable everywhere
        staged = staged_dir / _id_to_staged_glb(asset_id)
        if not staged.is_file():
            failures.append(
                f"{asset_id}: source bytes committed ({source}) but NOT staged at "
                f"{staged} — the client would _fail to load this model (story #569)"
            )
    return failures


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    root = Path(__file__).resolve().parent.parent
    parser.add_argument("--content", type=Path, default=root / "content")
    parser.add_argument(
        "--staged", type=Path, default=root / "client/project/meridian/core"
    )
    args = parser.parse_args(argv)

    failures = check(args.content, args.staged)
    if failures:
        print(f"{len(failures)} staged-model coverage failure(s):", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print("OK — every committed model referenced by content is staged.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
