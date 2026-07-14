"""Canonical bone table for the Meridian reference rig (spec 4 section 2).

Pure-Python, importable without Blender. Single source of truth for:

- the 56-bone Godot ``SkeletonProfileHumanoid`` set (exact names + hierarchy,
  verified 2026-07-10 against
  https://docs.godotengine.org/en/4.4/classes/class_skeletonprofilehumanoid.html
  -- 56 bones total: 26 "Body" group + 15 "LeftHand" + 15 "RightHand"), and
- the 7 Meridian socket bones (attach points for contract (1) gear sockets).

``schema/content/skeleton.defs.yaml`` ``$defs.boneName.enum`` is generated
from this module's ``bone_names()`` and MUST be kept in sync -- the drift
guard is ``tests/test_meridian_rig.py::test_skeleton_defs_bones_matches_table``.

Coordinates are metres, Y-up, in the reference "ardent_male" proportion
profile (~1.8 m humanoid), rest T-pose (arms out along +/-X at shoulder
height). Character's right side is +X, left side is -X; the two sides are
exact mirrors of each other. Precision is not the goal -- internal
consistency is: a child bone's ``head_m`` equals its anatomical parent's
``tail_m`` wherever the joint is a real anatomical hinge. Rig-only bones
(``Root``) and socket mount bones are exempt -- they are not anatomical
joints.

## Proportion profiles (spec 4: "proportions are parameters")

Every profile in :data:`VALID_PROFILES` shares one bone table -- **identical
bone NAMES and identical parent hierarchy** -- and differs ONLY in the rest
head/tail transforms. That shared-name contract is the whole reason gear
authored on one race binds to another by bone name (Race #2 / Dolmen, spec
``docs/superpowers/specs/2026-07-12-race-2-dolmen-design.md`` section 2). A
profile is therefore just a per-profile *point transform* applied to the one
reference geometry below: the structure (names/parents) is defined exactly
once, so a future race adds only a transform -- never a second 63-name list.
``ardent_male`` is the reference (identity transform); ``dolmen_male`` is the
shorter, broader "mountain/stone folk" build. Because each transform is a pure
function of position, shared anatomical joints stay shared and the +/-X mirror
is preserved automatically -- the shared-skeleton invariant is structural, not
hand-maintained.
"""
from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

Vec3 = tuple[float, float, float]


@dataclass(frozen=True)
class BoneSpec:
    name: str
    parent: str | None
    head_m: Vec3
    tail_m: Vec3


# ---------------------------------------------------------------------------
# Mirroring helpers -- the right-side chains are authored once and mirrored
# to the left side so the two sides can never drift apart.
# ---------------------------------------------------------------------------
def _mirror_name(name: str) -> str:
    assert name.startswith("Right"), f"expected a Right* bone name, got {name!r}"
    return "Left" + name[len("Right") :]


def _mirror_pt(p: Vec3) -> Vec3:
    x, y, z = p
    return (-x, y, z)


def _mirror_bone(b: BoneSpec) -> BoneSpec:
    parent = _mirror_name(b.parent) if b.parent and b.parent.startswith("Right") else b.parent
    return BoneSpec(_mirror_name(b.name), parent, _mirror_pt(b.head_m), _mirror_pt(b.tail_m))


# ---------------------------------------------------------------------------
# Central (unpaired) spine/head column -- X == 0.
# ---------------------------------------------------------------------------
_ROOT: list[BoneSpec] = [
    BoneSpec("Root", None, (0.0, 0.0, 0.0), (0.0, 0.02, 0.0)),
]

_SPINE: list[BoneSpec] = [
    BoneSpec("Hips", "Root", (0.0, 0.95, 0.0), (0.0, 1.05, 0.0)),
    BoneSpec("Spine", "Hips", (0.0, 1.05, 0.0), (0.0, 1.18, 0.0)),
    BoneSpec("Chest", "Spine", (0.0, 1.18, 0.0), (0.0, 1.32, 0.0)),
    BoneSpec("UpperChest", "Chest", (0.0, 1.32, 0.0), (0.0, 1.44, 0.0)),
    BoneSpec("Neck", "UpperChest", (0.0, 1.44, 0.0), (0.0, 1.52, 0.0)),
    BoneSpec("Head", "Neck", (0.0, 1.52, 0.0), (0.0, 1.72, 0.0)),
]

_JAW: list[BoneSpec] = [
    BoneSpec("Jaw", "Head", (0.0, 1.54, 0.02), (0.0, 1.52, 0.09)),
]

_RIGHT_EYE: list[BoneSpec] = [
    BoneSpec("RightEye", "Head", (0.03, 1.64, 0.06), (0.03, 1.64, 0.10)),
]
_LEFT_EYE: list[BoneSpec] = [_mirror_bone(b) for b in _RIGHT_EYE]


# ---------------------------------------------------------------------------
# Right arm chain (shoulder girdle -> hand), authored once and mirrored.
# ---------------------------------------------------------------------------
_SHOULDER_Y = 1.42

_RIGHT_ARM: list[BoneSpec] = [
    BoneSpec("RightShoulder", "UpperChest", (0.02, _SHOULDER_Y, 0.0), (0.17, _SHOULDER_Y, 0.0)),
    BoneSpec("RightUpperArm", "RightShoulder", (0.17, _SHOULDER_Y, 0.0), (0.47, _SHOULDER_Y, 0.0)),
    BoneSpec("RightLowerArm", "RightUpperArm", (0.47, _SHOULDER_Y, 0.0), (0.72, _SHOULDER_Y, 0.0)),
    BoneSpec("RightHand", "RightLowerArm", (0.72, _SHOULDER_Y, 0.0), (0.82, _SHOULDER_Y, 0.0)),
]
_RIGHT_HAND_TAIL: Vec3 = _RIGHT_ARM[-1].tail_m

# (finger, segment suffixes, lateral Z fan-out offset, per-segment length)
_FINGER_PLAN: list[tuple[str, list[str], float, float]] = [
    ("Thumb", ["Metacarpal", "Proximal", "Distal"], -0.025, 0.030),
    ("Index", ["Proximal", "Intermediate", "Distal"], -0.012, 0.025),
    ("Middle", ["Proximal", "Intermediate", "Distal"], 0.0, 0.025),
    ("Ring", ["Proximal", "Intermediate", "Distal"], 0.012, 0.025),
    ("Little", ["Proximal", "Intermediate", "Distal"], 0.025, 0.020),
]


def _finger_chain(hand_tail: Vec3, hand_bone: str, finger: str, z_offset: float,
                   segments: list[str], seg_len: float) -> list[BoneSpec]:
    """Build one finger's bone chain, rooted at the hand's tail (wrist->palm)."""
    x, y, z = hand_tail
    z += z_offset
    out: list[BoneSpec] = []
    parent = hand_bone
    for seg in segments:
        name = f"{hand_bone[: -len('Hand')]}{finger}{seg}"
        head = (x, y, z)
        x += seg_len
        tail = (x, y, z)
        out.append(BoneSpec(name, parent, head, tail))
        parent = name
    return out


_RIGHT_FINGERS: list[BoneSpec] = [
    bone
    for finger, segments, z_off, seg_len in _FINGER_PLAN
    for bone in _finger_chain(_RIGHT_HAND_TAIL, "RightHand", finger, z_off, segments, seg_len)
]

_LEFT_ARM: list[BoneSpec] = [_mirror_bone(b) for b in _RIGHT_ARM]
_LEFT_FINGERS: list[BoneSpec] = [_mirror_bone(b) for b in _RIGHT_FINGERS]


# ---------------------------------------------------------------------------
# Right leg chain (hip -> toes), authored once and mirrored.
# ---------------------------------------------------------------------------
_HIP_X = 0.09

_RIGHT_LEG: list[BoneSpec] = [
    BoneSpec("RightUpperLeg", "Hips", (_HIP_X, 0.95, 0.0), (_HIP_X, 0.50, 0.0)),
    BoneSpec("RightLowerLeg", "RightUpperLeg", (_HIP_X, 0.50, 0.0), (_HIP_X, 0.10, 0.0)),
    BoneSpec("RightFoot", "RightLowerLeg", (_HIP_X, 0.10, 0.0), (_HIP_X, 0.05, 0.12)),
    BoneSpec("RightToes", "RightFoot", (_HIP_X, 0.05, 0.12), (_HIP_X, 0.02, 0.20)),
]
_LEFT_LEG: list[BoneSpec] = [_mirror_bone(b) for b in _RIGHT_LEG]


# ---------------------------------------------------------------------------
# PROFILE_BONES -- the 56 Godot SkeletonProfileHumanoid bones, "ardent_male"
# proportions. Order follows the class's Body / LeftHand / RightHand groups.
# ---------------------------------------------------------------------------
PROFILE_BONES: list[BoneSpec] = [
    *_ROOT,
    *_SPINE,
    *_LEFT_EYE,
    *_RIGHT_EYE,
    *_JAW,
    *_LEFT_ARM,
    *_LEFT_FINGERS,
    *_RIGHT_ARM,
    *_RIGHT_FINGERS,
    *_LEFT_LEG,
    *_RIGHT_LEG,
]

assert len(PROFILE_BONES) == 56, f"expected 56 profile bones, got {len(PROFILE_BONES)}"
assert len({b.name for b in PROFILE_BONES}) == 56, "duplicate bone name in PROFILE_BONES"


# ---------------------------------------------------------------------------
# SOCKET_BONES -- Meridian gear-attach mounts (contract 1 sockets), one bone
# per socket, parented per spec section 2.1's table. Short stub bones rooted
# at the parent's mount point.
# ---------------------------------------------------------------------------
_RIGHT_HAND_MOUNT: Vec3 = _RIGHT_ARM[-1].tail_m  # RightHand.tail_m
_LEFT_HAND_MOUNT: Vec3 = _LEFT_ARM[-1].tail_m  # LeftHand.tail_m
_CHEST_MOUNT: Vec3 = _SPINE[2].tail_m  # Chest.tail_m
_HIPS_MOUNT: Vec3 = _SPINE[0].head_m  # Hips.head_m (pelvis center)

SOCKET_BONES: list[BoneSpec] = [
    BoneSpec("socket_main_hand", "RightHand", _RIGHT_HAND_MOUNT,
             (_RIGHT_HAND_MOUNT[0] + 0.05, _RIGHT_HAND_MOUNT[1], _RIGHT_HAND_MOUNT[2])),
    BoneSpec("socket_off_hand", "LeftHand", _LEFT_HAND_MOUNT,
             (_LEFT_HAND_MOUNT[0] - 0.05, _LEFT_HAND_MOUNT[1], _LEFT_HAND_MOUNT[2])),
    BoneSpec("socket_shield", "LeftHand", _LEFT_HAND_MOUNT,
             (_LEFT_HAND_MOUNT[0], _LEFT_HAND_MOUNT[1], _LEFT_HAND_MOUNT[2] + 0.05)),
    BoneSpec("socket_back", "Chest", _CHEST_MOUNT,
             (_CHEST_MOUNT[0], _CHEST_MOUNT[1] - 0.12, _CHEST_MOUNT[2] - 0.10)),
    BoneSpec("socket_ranged", "Chest", _CHEST_MOUNT,
             (_CHEST_MOUNT[0], _CHEST_MOUNT[1] + 0.10, _CHEST_MOUNT[2] - 0.12)),
    BoneSpec("socket_hip_l", "Hips", (_HIPS_MOUNT[0] - _HIP_X, _HIPS_MOUNT[1], _HIPS_MOUNT[2]),
             (_HIPS_MOUNT[0] - _HIP_X, _HIPS_MOUNT[1] - 0.10, _HIPS_MOUNT[2] + 0.05)),
    BoneSpec("socket_hip_r", "Hips", (_HIPS_MOUNT[0] + _HIP_X, _HIPS_MOUNT[1], _HIPS_MOUNT[2]),
             (_HIPS_MOUNT[0] + _HIP_X, _HIPS_MOUNT[1] - 0.10, _HIPS_MOUNT[2] + 0.05)),
]

assert len(SOCKET_BONES) == 7, f"expected 7 socket bones, got {len(SOCKET_BONES)}"


ALL_BONES: list[BoneSpec] = PROFILE_BONES + SOCKET_BONES

assert len(ALL_BONES) == 63, f"expected 63 total bones, got {len(ALL_BONES)}"


# ---------------------------------------------------------------------------
# Proportion profiles -- ONE shared bone table (the names + hierarchy authored
# above), per-profile REST TRANSFORMS only. Per the module docstring a profile
# is a pure position->position map applied to the reference (ardent_male)
# geometry, so every profile carries the identical 63 bone names and parent
# hierarchy and differs only in where the joints sit. Adding a race == adding
# one transform here -- never a second 63-name list.
# ---------------------------------------------------------------------------
_REFERENCE_BONES: list[BoneSpec] = ALL_BONES  # ardent_male IS the reference pose


def _ardent_pt(p: Vec3) -> Vec3:
    """Reference profile: identity (ardent_male is the authored geometry)."""
    return p


# Dolmen "mountain/stone folk": shorter overall with disproportionately shorter
# legs, and broader across the shoulders/hips. Height splits at the hip line
# (Y=0.95): legs below compress harder than the torso above, and every X widens
# -- a stockier humanoid that stays a valid T-pose. Bulk/thickness is the BODY
# mesh's job (Dolmen D2); these spans only set the proportions the body + gear
# fit to.
_DOLMEN_HIP_Y = 0.95  # pelvis line: legs below, torso/arms/head above
_DOLMEN_LEG_SCALE = 0.84  # legs shortened most (stocky stance)
_DOLMEN_TORSO_SCALE = 0.93  # torso/neck/head shortened gently
_DOLMEN_WIDTH_SCALE = 1.10  # broader shoulders + hips (also arm span + sockets)


def _dolmen_pt(p: Vec3) -> Vec3:
    """Dolmen point map: piecewise-Y height compression + uniform X widening.

    Continuous at the hip line (both branches meet at Y=0.95, so no joint kinks)
    and a pure function of position, so shared heads/tails and the +/-X mirror
    are preserved. Depth (Z) is untouched -- bones are thin; the body mesh adds
    bulk.
    """
    x, y, z = p
    if y <= _DOLMEN_HIP_Y:
        y2 = y * _DOLMEN_LEG_SCALE
    else:
        y2 = _DOLMEN_HIP_Y * _DOLMEN_LEG_SCALE + (y - _DOLMEN_HIP_Y) * _DOLMEN_TORSO_SCALE
    return (x * _DOLMEN_WIDTH_SCALE, y2, z)


# Chibi "pill folk": the compact toy-proportioned base (spec 5 chibi theme, epic
# #722 / story #745). The keystone body is a rounded PILL sculpt -- a smooth blob
# a little taller than it is wide, with no protruding limbs -- so the reference
# ardent_male arm span (hands out near a full human reach) lands far OUTSIDE that
# little pill volume. The restyle's geoset cut then finds no sculpt geometry for
# the arm/hand regions and falls back to proxy boxes that render as white bars
# floating at shoulder height. The chibi map pulls every joint INSIDE the pill:
# height compresses hard piecewise at the hip line (stubby legs, short torso that
# drops the shoulder line down into the pill's wide belly), and both lateral axes
# (X arm span + hip width, Z finger/foot depth) pull toward the axis, so all
# eight geoset region anchors sit within the fitted pill and capture real
# geometry. Depth is scaled too (unlike Dolmen) because the pill is shallow front
# to back -- untouched finger/toe reach would poke out through its surface.
_CHIBI_HIP_Y = 0.95  # pelvis line: legs below, torso/arms/head above (as reference)
_CHIBI_LEG_SCALE = 0.42  # legs very short + lifted so foot/lower-leg anchors sit in the pill
_CHIBI_TORSO_SCALE = 0.46  # short torso: drops the shoulder line into the wide belly band
_CHIBI_WIDTH_SCALE = 0.40  # arms reach OUT to the pill's stubby T-pose arms (hand |X| 0.82 -> ~0.33)
_CHIBI_DEPTH_SCALE = 0.55  # tuck fingers/feet fore-aft inside the shallow pill depth


def _chibi_pt(p: Vec3) -> Vec3:
    """Chibi point map: piecewise-Y height compression + hard X/Z inward pull.

    Same shape as :func:`_dolmen_pt` (piecewise-linear height about the hip line,
    continuous at Y=0.95, linear per branch so straight bones stay straight, a
    pure function of position so shared heads/tails and the +/-X mirror are
    preserved automatically) -- only far more compact, and with the depth axis
    (Z) scaled too. X (arm span, hip width) and Z (finger/foot depth) pull hard
    toward the axis so every limb joint -- hence every one of the eight geoset
    region anchors -- lands INSIDE the compact pill rather than out at the full
    ardent human reach, where the arm/hand geosets would fall back to floating
    proxy boxes. The pill's own arms are short and stubby (a chibi T-pose), so
    the arm span shrinks a lot (hand |X| 0.82 -> ~0.33, well under half ardent)
    but still reaches OUT to where those little arms actually are, so the hand
    bone runs the length of the arm instead of stopping short in the torso.
    """
    x, y, z = p
    if y <= _CHIBI_HIP_Y:
        y2 = y * _CHIBI_LEG_SCALE
    else:
        y2 = _CHIBI_HIP_Y * _CHIBI_LEG_SCALE + (y - _CHIBI_HIP_Y) * _CHIBI_TORSO_SCALE
    return (x * _CHIBI_WIDTH_SCALE, y2, z * _CHIBI_DEPTH_SCALE)


# Registry: profile name -> its point transform. VALID_PROFILES and the built
# per-profile bone tables below both derive from this single dict.
_PROFILE_POINT_XFORMS: dict[str, Callable[[Vec3], Vec3]] = {
    "ardent_male": _ardent_pt,
    "dolmen_male": _dolmen_pt,
    "chibi": _chibi_pt,
}

VALID_PROFILES: tuple[str, ...] = tuple(_PROFILE_POINT_XFORMS)


def _xform_bone(fn: Callable[[Vec3], Vec3], b: BoneSpec) -> BoneSpec:
    """Apply a point transform to a bone's head + tail (name/parent unchanged)."""
    return BoneSpec(b.name, b.parent, fn(b.head_m), fn(b.tail_m))


# Materialised once at import: every profile's full 63-bone table (56 profile +
# 7 sockets), all sharing the reference names/parents.
_PROFILE_BONES_BY_NAME: dict[str, list[BoneSpec]] = {
    name: [_xform_bone(fn, b) for b in _REFERENCE_BONES]
    for name, fn in _PROFILE_POINT_XFORMS.items()
}

# Shared-skeleton invariant, guarded at import: every profile is name- and
# hierarchy-identical to the reference -- only rest transforms may differ.
_REF_NAMES = [b.name for b in _REFERENCE_BONES]
_REF_HIERARCHY = {b.name: b.parent for b in _REFERENCE_BONES}
for _pname, _ptable in _PROFILE_BONES_BY_NAME.items():
    assert [b.name for b in _ptable] == _REF_NAMES, f"{_pname}: bone names diverge from reference"
    assert {b.name: b.parent for b in _ptable} == _REF_HIERARCHY, (
        f"{_pname}: hierarchy diverges from reference"
    )
    assert len(_ptable) == 63, f"{_pname}: {len(_ptable)} bones, expected 63"


def for_profile(profile: str) -> list[BoneSpec]:
    """Return the full 63-bone rest table (56 profile + 7 sockets) for ``profile``.

    Every profile shares the reference bone NAMES and parent hierarchy; only the
    rest head/tail transforms differ -- the shared-skeleton contract that lets
    gear authored on one race bind to another by bone name. An unknown key raises
    ``ValueError`` naming the valid profiles.
    """
    if profile not in _PROFILE_BONES_BY_NAME:
        raise ValueError(
            f"unknown proportion profile {profile!r}; valid profiles: {list(VALID_PROFILES)}"
        )
    return _PROFILE_BONES_BY_NAME[profile]


def bone_names(profile: str = "ardent_male") -> list[str]:
    """Bone names in canonical table order (identical across every profile).

    This order == ``schema/content/skeleton.defs.yaml``'s ``boneName`` enum.
    ``profile`` is accepted for symmetry with :func:`for_profile`; names never
    vary by profile (that is the shared-skeleton contract).
    """
    return [b.name for b in for_profile(profile)]


def hierarchy(profile: str = "ardent_male") -> dict[str, str | None]:
    """Map bone name -> parent name (``None`` for the single root; profile-invariant)."""
    return {b.name: b.parent for b in for_profile(profile)}
