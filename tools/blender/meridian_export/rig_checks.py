"""Pure-Python rig/geoset conformance checks for meridian_export (spec ④ §3/§4).

E-rule band, mirroring the naming/scale/pivot check style in :mod:`sidecar`: this
module is deliberately free of any ``bpy`` import so it is unit-testable without a
Blender install. The ``bpy``-dependent operator (``__init__.py``) extracts a plain
:class:`RigData` snapshot from the armature + meshes of a skeletal export
(``character_model`` / ``armor_model``) and hands it here.

Single-source-of-truth: bone names come from ``tools/blender/meridian_rig/bones.py``
(the ④/T1 canonical bone table, consumed via the same repo-relative ``sys.path``
pattern ``tests/test_meridian_rig.py`` uses); geoset regions come from
``schema/content/skeleton.defs.yaml`` (``$defs.geosetRegion.enum``), read at module
load the same way :func:`sidecar.load_budgets` resolves ``budgets.json``. Neither
enum is hardcoded a second time here.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

# tools/blender/meridian_export/rig_checks.py -> repo root is 3 parents up.
REPO_ROOT = Path(__file__).resolve().parents[3]
SKELETON_DEFS_PATH = REPO_ROOT / "schema" / "content" / "skeleton.defs.yaml"
RIG_DIR = REPO_ROOT / "tools" / "blender" / "meridian_rig"

if str(RIG_DIR) not in sys.path:  # same sys.path pattern as test_meridian_rig.py
    sys.path.insert(0, str(RIG_DIR))

import bones as _bones  # noqa: E402  (pure module — T1's canonical bone table)


def load_geoset_regions(path: Path | None = None) -> list[str]:
    """Load the 8 geoset region names from skeleton.defs.yaml (no second copy)."""
    defs = yaml.safe_load((path or SKELETON_DEFS_PATH).read_text(encoding="utf-8"))
    return list(defs["$defs"]["geosetRegion"]["enum"])


GEOSET_REGIONS: list[str] = load_geoset_regions()
CANONICAL_BONE_NAMES: set[str] = set(_bones.bone_names())
SOCKET_NAMES: list[str] = [b.name for b in _bones.SOCKET_BONES]

MAX_INFLUENCES = 4
# Region names may contain underscores (e.g. "hips_legs"); greedy backtracking
# still anchors correctly on the trailing "_lod<N>" suffix.
_GEO_NAME_RE = re.compile(r"^geo_(?P<region>[a-z0-9_]+)_lod(?P<lod>\d+)$")


@dataclass
class RigData:
    """Plain snapshot of a skeletal export, extracted by the operator.

    The operator reads these off the real armature/mesh objects; tests construct
    them directly (no ``bpy`` types involved).
    """

    asset_class: str  # "character_model" | "armor_model" (skeletal export classes)
    bone_names: list[str]  # armature bone names present in the export
    socket_names: list[str]  # subset of bone_names that are socket_* mounts
    mesh_names: list[str]  # exported mesh object names
    max_influences: int  # max vertex-group influences per vertex, across all meshes
    weights_normalized: bool  # whether skin weights sum to 1.0 per vertex


def check_bone_set(data: RigData) -> list[str]:
    """E100 — every exported bone must be in the canonical skeleton.defs set."""
    unknown = sorted(set(data.bone_names) - CANONICAL_BONE_NAMES)
    if unknown:
        return [f"E100 unknown bone name(s) not in skeleton.defs: {', '.join(unknown)}"]
    return []


def check_sockets(data: RigData) -> list[str]:
    """E101 — all 7 socket bones must be present on a skeleton export."""
    missing = sorted(set(SOCKET_NAMES) - set(data.socket_names))
    if missing:
        return [f"E101 missing required socket bone(s): {', '.join(missing)}"]
    return []


def check_geoset_naming(data: RigData) -> list[str]:
    """E102 — character_model bodies: geo_<region>_lod<N> naming, all 8 at lod0."""
    if data.asset_class != "character_model":
        return []
    lod0_regions: set[str] = set()
    for name in data.mesh_names:
        match = _GEO_NAME_RE.match(name)
        if (
            match
            and match.group("region") in GEOSET_REGIONS
            and int(match.group("lod")) == 0
        ):
            lod0_regions.add(match.group("region"))
    missing = sorted(set(GEOSET_REGIONS) - lod0_regions)
    if missing:
        return [
            "E102 character_model body missing LOD0 geoset region(s): "
            + ", ".join(missing)
        ]
    return []


def check_influences(data: RigData) -> list[str]:
    """E103 — at most 4 influences per vertex, and weights must be normalized."""
    errors: list[str] = []
    if data.max_influences > MAX_INFLUENCES:
        errors.append(
            f"E103 max {data.max_influences} influences/vertex exceeds the "
            f"{MAX_INFLUENCES} ceiling"
        )
    if not data.weights_normalized:
        errors.append("E103 vertex weights are not normalized")
    return errors


def check_geoset_regions(data: RigData) -> list[str]:
    """E104 — geo_*-named meshes must name a known region; armor_model must not
    carry geoset names at all (gear binds whole pieces, it doesn't hide regions)."""
    errors: list[str] = []
    for name in data.mesh_names:
        if data.asset_class == "armor_model":
            if name.startswith("geo_"):
                errors.append(
                    f"E104 armor_model mesh '{name}' must not use geoset naming "
                    f"(geo_*) — gear binds only to canonical bones"
                )
            continue
        if not name.startswith("geo_"):
            continue
        match = _GEO_NAME_RE.match(name)
        if not match or match.group("region") not in GEOSET_REGIONS:
            errors.append(f"E104 mesh '{name}' uses an unknown geoset region name")
    return errors


def check_rig(data: RigData) -> list[str]:
    """Run every E-rule for a skeletal export, in report order."""
    errors: list[str] = []
    errors += check_bone_set(data)
    errors += check_sockets(data)
    errors += check_geoset_naming(data)
    errors += check_influences(data)
    errors += check_geoset_regions(data)
    return errors
