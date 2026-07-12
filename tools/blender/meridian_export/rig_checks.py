"""Pure-Python rig/geoset conformance checks for meridian_export (spec ④ §3/§4).

E-rule band, mirroring the naming/scale/pivot check style in :mod:`sidecar`: this
module is deliberately free of any ``bpy`` import so it is unit-testable without a
Blender install. The ``bpy``-dependent operator (``__init__.py``) extracts a plain
:class:`RigData` snapshot from the armature + meshes of a skeletal export
(``character_model`` / ``armor_model``) and hands it here.

Single-source-of-truth: bone names come from ``tools/blender/meridian_rig/bones.py``
(the ④/T1 canonical bone table, consumed via the same repo-relative ``sys.path``
pattern ``tests/test_meridian_rig.py`` uses); geoset regions come from
``schema/content/skeleton.defs.yaml`` (``$defs.geosetRegion.enum``), resolved
repo-relative the same way :func:`sidecar.load_budgets` resolves ``budgets.json``.
Neither enum is hardcoded a second time here.

Loading is LAZY: nothing is read from disk at import time, so the addon stays
fully importable (and non-skeletal exports keep working) when it is installed
zipped into Blender's addon directory, where the repo-relative files do not
exist. The vocabulary is loaded and cached on the first :func:`check_rig` call;
if the repo files are unreachable, that call raises a :class:`RuntimeError`
explaining that rig checks require running Blender from a repo checkout.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import yaml

# tools/blender/meridian_export/rig_checks.py -> repo root is 3 parents up.
# Paths are computed eagerly (cheap, no I/O); reads are deferred to first use.
REPO_ROOT = Path(__file__).resolve().parents[3]
SKELETON_DEFS_PATH = REPO_ROOT / "schema" / "content" / "skeleton.defs.yaml"
RIG_DIR = REPO_ROOT / "tools" / "blender" / "meridian_rig"

MAX_INFLUENCES = 4
# Region names may contain underscores (e.g. "hips_legs"); greedy backtracking
# still anchors correctly on the trailing "_lod<N>" suffix.
_GEO_NAME_RE = re.compile(r"^geo_(?P<region>[a-z0-9_]+)_lod(?P<lod>\d+)$")

# Lazily-populated vocabulary cache: GEOSET_REGIONS / CANONICAL_BONE_NAMES /
# SOCKET_NAMES. None until the first check_rig() (or attribute access) needs it.
_vocab: dict | None = None


def load_geoset_regions(path: Path | None = None) -> list[str]:
    """Load the 8 geoset region names from skeleton.defs.yaml (no second copy)."""
    defs = yaml.safe_load((path or SKELETON_DEFS_PATH).read_text(encoding="utf-8"))
    return list(defs["$defs"]["geosetRegion"]["enum"])


def _load_vocab() -> dict:
    """Load + cache the bone/socket/region vocabulary from the repo checkout.

    Raises an actionable RuntimeError when the repo-relative sources are
    unreachable (e.g. the addon was installed zipped into Blender's addon dir)
    instead of failing at import time and killing non-skeletal export paths.
    """
    global _vocab
    if _vocab is not None:
        return _vocab
    try:
        regions = load_geoset_regions()
        if str(RIG_DIR) not in sys.path:  # same sys.path pattern as the rig tests
            sys.path.insert(0, str(RIG_DIR))
        import bones as _bones  # pure module — T1's canonical bone table

        _vocab = {
            "GEOSET_REGIONS": regions,
            "CANONICAL_BONE_NAMES": set(_bones.bone_names()),
            "SOCKET_NAMES": [b.name for b in _bones.SOCKET_BONES],
        }
    except (OSError, ImportError, KeyError, yaml.YAMLError) as exc:
        raise RuntimeError(
            "meridian_export rig checks (E100-E105) require running Blender from "
            "a Project Meridian repo checkout: could not load "
            f"{SKELETON_DEFS_PATH} and/or {RIG_DIR}/bones.py ({exc!r}). "
            "Non-skeletal export classes are unaffected."
        ) from exc
    return _vocab


def __getattr__(name: str):
    """PEP 562 lazy module attributes for the vocabulary constants."""
    if name in ("GEOSET_REGIONS", "CANONICAL_BONE_NAMES", "SOCKET_NAMES"):
        return _load_vocab()[name]
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


@dataclass
class ObjectTransformState:
    """Per-object transform snapshot for E105 (spec ④ §4's dropped blocking
    promise): whether an exported object (armature or mesh) has any residual,
    unapplied object-level transform (location/rotation/scale off identity)."""

    name: str  # armature or mesh object name
    transforms_applied: bool  # True when location/rotation/scale are identity


@dataclass
class RigData:
    """Plain snapshot of a skeletal export, extracted by the operator.

    The operator reads these off the real armature/mesh objects; tests construct
    them directly (no ``bpy`` types involved).

    The fields below `weights_normalized` are additive extensions (#526): they
    carry the same information the original aggregate fields do, but broken
    down enough for E103/E105 to name the offending object. Old callers that
    only set the aggregate fields keep working — the newer, more specific
    fields default to "nothing to report" and the checks fall back to the
    aggregate fields when they're empty.
    """

    asset_class: str  # "character_model" | "armor_model" (skeletal export classes)
    bone_names: list[str]  # armature bone names present in the export
    socket_names: list[str]  # subset of bone_names that are socket_* mounts
    mesh_names: list[str]  # exported mesh object names
    max_influences: int  # max vertex-group influences per vertex, across all meshes
    weights_normalized: bool  # whether skin weights sum to 1.0 per vertex
    # E103 mesh identity (#526, T3 review minor): per-mesh breakdown of the
    # same influence data the aggregate fields above summarize.
    mesh_max_influences: dict[str, int] = field(default_factory=dict)
    unnormalized_meshes: list[str] = field(default_factory=list)
    # E105 (#526): per-object transform state + scene unit scale.
    object_transforms: list[ObjectTransformState] = field(default_factory=list)
    unit_scale_ok: bool = True  # scene resolves to 1 Blender unit = 1 m


def check_bone_set(data: RigData) -> list[str]:
    """E100 — every exported bone must be in the canonical skeleton.defs set."""
    unknown = sorted(set(data.bone_names) - _load_vocab()["CANONICAL_BONE_NAMES"])
    if unknown:
        return [f"E100 unknown bone name(s) not in skeleton.defs: {', '.join(unknown)}"]
    return []


def check_sockets(data: RigData) -> list[str]:
    """E101 — all 7 socket bones must be present on a skeleton export.

    character_model only: gear (armor_model) is a skinned piece bound to a
    SUBSET of canonical bones and never carries the socket mounts itself
    (spec ④ §4 — sockets are required "when exporting a skeleton asset").
    """
    if data.asset_class != "character_model":
        return []
    missing = sorted(set(_load_vocab()["SOCKET_NAMES"]) - set(data.socket_names))
    if missing:
        return [f"E101 missing required socket bone(s): {', '.join(missing)}"]
    return []


def check_geoset_naming(data: RigData) -> list[str]:
    """E102 — character_model bodies: geo_<region>_lod<N> naming, all 8 at lod0.

    Applies only when the export carries meshes: a skeleton-only
    character_model export (the committed rig asset — spec §5's skeleton-vs-body
    distinction) has no geosets to cover and passes E102.
    """
    if data.asset_class != "character_model" or not data.mesh_names:
        return []
    regions = _load_vocab()["GEOSET_REGIONS"]
    lod0_regions: set[str] = set()
    for name in data.mesh_names:
        match = _GEO_NAME_RE.match(name)
        if match and match.group("region") in regions and int(match.group("lod")) == 0:
            lod0_regions.add(match.group("region"))
    missing = sorted(set(regions) - lod0_regions)
    if missing:
        return [
            "E102 character_model body missing LOD0 geoset region(s): "
            + ", ".join(missing)
        ]
    return []


def check_influences(data: RigData) -> list[str]:
    """E103 — at most 4 influences per vertex, and weights must be normalized.

    Names the offending mesh when the operator supplies the per-mesh
    breakdown (T3 review minor, #526); falls back to the aggregate
    `max_influences`/`weights_normalized` fields otherwise.
    """
    errors: list[str] = []
    if data.mesh_max_influences:
        for name in sorted(data.mesh_max_influences):
            count = data.mesh_max_influences[name]
            if count > MAX_INFLUENCES:
                errors.append(
                    f"E103 mesh '{name}' has {count} influences/vertex, "
                    f"exceeds the {MAX_INFLUENCES} ceiling"
                )
    elif data.max_influences > MAX_INFLUENCES:
        errors.append(
            f"E103 max {data.max_influences} influences/vertex exceeds the "
            f"{MAX_INFLUENCES} ceiling"
        )
    if data.unnormalized_meshes:
        for name in sorted(data.unnormalized_meshes):
            errors.append(f"E103 mesh '{name}' vertex weights are not normalized")
    elif not data.weights_normalized:
        errors.append("E103 vertex weights are not normalized")
    return errors


def check_geoset_regions(data: RigData) -> list[str]:
    """E104 — geo_*-named meshes must name a known region; armor_model must not
    carry geoset names at all (gear binds whole pieces, it doesn't hide regions)."""
    errors: list[str] = []
    regions = _load_vocab()["GEOSET_REGIONS"]
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
        if not match or match.group("region") not in regions:
            errors.append(f"E104 mesh '{name}' uses an unknown geoset region name")
    return errors


def check_transforms_and_scale(data: RigData) -> list[str]:
    """E105 — transforms applied, correct unit scale (spec ④ §4's dropped
    blocking promise: "transforms applied, unit scale, correct axes").

    Axis conversion is not re-checked here: it is forced by the exporter's
    ``export_yup=True`` setting baked into the addon operator (Art SAD
    §2.1 — "contributors cannot get it wrong"), so there is no per-asset
    data to validate. This rule blocks on the two things a contributor CAN
    still get wrong before export: a residual object-level transform, and a
    scene unit scale that isn't 1 Blender unit = 1 m.
    """
    errors: list[str] = []
    unapplied = sorted(
        obj.name for obj in data.object_transforms if not obj.transforms_applied
    )
    if unapplied:
        errors.append(
            "E105 unapplied transform(s) on: "
            + ", ".join(unapplied)
            + " — apply all transforms (Ctrl+A) before export"
        )
    if not data.unit_scale_ok:
        errors.append(
            "E105 scene unit scale is not 1 Blender unit = 1 m — set the scene "
            "unit system to Metric with Unit Scale 1.0 before export"
        )
    return errors


def check_rig(data: RigData) -> list[str]:
    """Run every E-rule for a skeletal export, in report order.

    First call loads the bone/region vocabulary from the repo checkout; raises
    RuntimeError with a repo-checkout hint if those files are unreachable.
    """
    _load_vocab()  # fail fast, with the actionable error, before any partial output
    errors: list[str] = []
    errors += check_bone_set(data)
    errors += check_sockets(data)
    errors += check_geoset_naming(data)
    errors += check_influences(data)
    errors += check_geoset_regions(data)
    errors += check_transforms_and_scale(data)
    return errors
