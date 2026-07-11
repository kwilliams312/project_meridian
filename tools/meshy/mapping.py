"""PURE bone-map loading + Meshy→canonical rig conversion planning (spec ④ §7.3).

No Blender, no network. Loads the versioned mapping table
(:file:`tools/meshy/bone_map.yaml`) and resolves a flat list of Meshy rig bone
names into a :class:`ConversionPlan`:

- **renames** — a Meshy bone that the map assigns a canonical target (spec ④ §2
  bone from :mod:`meridian_rig.bones`); the vertex group is renamed in place.
- **merges** — a helper/twist bone *absent* from the map but named as a
  CamelCase/underscore descendant of a mapped bone (e.g. ``LeftForeArmTwist``
  under ``LeftForeArm``); its skin weights merge into that ancestor's canonical
  target before the group is deleted. The "nearest mapped ancestor" is resolved
  by **longest matching name prefix** — Meshy/Mixamo-style twist and roll bones
  are named as suffixed children of the joint they help, so the flat name alone
  determines the ancestor (the pure function never sees the armature hierarchy).
- **unmapped** — a bone with neither a map entry nor a mapped-ancestor prefix.
  A non-empty ``unmapped`` list is a **hard error at the CLI boundary** (the
  table grows deliberately, never silently); this module surfaces the names,
  the caller raises.

An unknown Meshy model version raises :class:`UnknownVersionError` naming the
known versions — the map is keyed by model version because Meshy's rig naming
can drift between releases (spec ④ §7.3).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import yaml

_DEFAULT_MAP_PATH = Path(__file__).resolve().parent / "bone_map.yaml"


class MappingError(Exception):
    """Base error for bone-map loading / plan resolution."""


class UnknownVersionError(MappingError):
    """Raised for a Meshy model version absent from the map."""


@dataclass(frozen=True)
class BoneMap:
    """One model version's Meshy→canonical table plus its verification state."""

    version: str
    verified: bool
    bones: dict[str, str]


@dataclass
class ConversionPlan:
    """The resolved conversion for a set of Meshy bones (spec ④ §7.3)."""

    renames: dict[str, str] = field(default_factory=dict)
    merges: dict[str, str] = field(default_factory=dict)
    unmapped: list[str] = field(default_factory=list)


def _load_doc(path: Path) -> dict:
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(doc, dict):
        raise MappingError(
            f"{path}: bone map must be a mapping, got {type(doc).__name__}"
        )
    return doc


def known_versions(*, path: Path = _DEFAULT_MAP_PATH) -> list[str]:
    """Sorted list of Meshy model versions the map defines."""
    versions = _load_doc(path).get("versions") or {}
    return sorted(versions)


def load_map(version: str, *, path: Path = _DEFAULT_MAP_PATH) -> BoneMap:
    """Load one version's table; raise :class:`UnknownVersionError` if absent."""
    versions = _load_doc(path).get("versions") or {}
    block = versions.get(version)
    if block is None:
        raise UnknownVersionError(
            f"unknown Meshy model version {version!r}; "
            f"known versions: {sorted(versions)}"
        )
    return BoneMap(
        version=version,
        verified=bool(block.get("verified", False)),
        bones=dict(block.get("bones") or {}),
    )


def _is_camel_or_sep_boundary(ch: str) -> bool:
    """True where ``ch`` starts a new name token: uppercase, digit, or separator.

    This keeps ``LeftForeArm`` from matching a hypothetical ``LeftForeArmpit``
    (lowercase 'p' — same token) while still matching ``LeftForeArmTwist`` (upper
    'T'), ``LeftForeArm1`` (digit), and ``LeftForeArm_twist`` (separator '_').
    """
    return ch.isupper() or ch.isdigit() or not ch.isalnum()


def _nearest_mapped_ancestor(bone: str, mapped_by_len_desc: list[str]) -> str | None:
    """Longest mapped bone name that is a token-boundary prefix of ``bone``."""
    for candidate in mapped_by_len_desc:
        if bone == candidate or not bone.startswith(candidate):
            continue
        remainder = bone[len(candidate) :]
        if remainder and _is_camel_or_sep_boundary(remainder[0]):
            return candidate
    return None


def plan_with_map(meshy_bones: list[str], bone_map: BoneMap) -> ConversionPlan:
    """Resolve ``meshy_bones`` against an already-loaded :class:`BoneMap`."""
    table = bone_map.bones
    mapped_by_len_desc = sorted(table, key=len, reverse=True)
    result = ConversionPlan()
    for bone in meshy_bones:
        target = table.get(bone)
        if target is not None:
            result.renames[bone] = target
            continue
        ancestor = _nearest_mapped_ancestor(bone, mapped_by_len_desc)
        if ancestor is not None:
            result.merges[bone] = table[ancestor]
        else:
            result.unmapped.append(bone)
    return result


def plan(
    meshy_bones: list[str], version: str, *, path: Path = _DEFAULT_MAP_PATH
) -> ConversionPlan:
    """Load ``version``'s table and resolve ``meshy_bones`` into a plan."""
    return plan_with_map(meshy_bones, load_map(version, path=path))


def joint_names_from_glb(glb_path: Path) -> list[str]:
    """Named skin-joint nodes of a rigged .glb, in skin-joint order (pygltflib).

    Mirrors :func:`validate_imports._joint_names`' skin-first logic but preserves
    order and returns a list. Skinless glbs fall back to named non-mesh nodes.
    """
    from pygltflib import GLTF2

    gltf = GLTF2().load(str(glb_path))
    names: list[str] = []
    seen: set[str] = set()

    def _add(name: str | None) -> None:
        if name and name not in seen:
            seen.add(name)
            names.append(name)

    if gltf.skins:
        for skin in gltf.skins:
            for j in skin.joints or []:
                _add(gltf.nodes[j].name)
    else:
        for node in gltf.nodes:
            if node.mesh is None:
                _add(node.name)
    return names
