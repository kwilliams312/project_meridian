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

Coordinates are metres, Y-up, in the default "ardent_male" proportion
profile (~1.8 m humanoid), rest T-pose (arms out along +/-X at shoulder
height). Character's right side is +X, left side is -X; the two sides are
exact mirrors of each other. Precision is not the goal -- internal
consistency is: a child bone's ``head_m`` equals its anatomical parent's
``tail_m`` wherever the joint is a real anatomical hinge. Rig-only bones
(``Root``) and socket mount bones are exempt -- they are not anatomical
joints.
"""
from __future__ import annotations

from dataclasses import dataclass

Vec3 = tuple[float, float, float]


@dataclass(frozen=True)
class BoneSpec:
    name: str
    parent: str | None
    head_m: Vec3
    tail_m: Vec3


VALID_PROFILES = ("ardent_male",)


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


def bone_names() -> list[str]:
    """Bone names in canonical table order (this order == skeleton.defs.yaml enum)."""
    return [b.name for b in ALL_BONES]


def hierarchy() -> dict[str, str | None]:
    """Map bone name -> parent bone name (``None`` for the single root)."""
    return {b.name: b.parent for b in ALL_BONES}


def for_profile(profile: str) -> list[BoneSpec]:
    """Return the 56 profile bones (rest transforms) for a proportion profile.

    v1 supports only ``"ardent_male"``; an unknown key raises ``ValueError``
    naming the valid profiles.
    """
    if profile not in VALID_PROFILES:
        raise ValueError(
            f"unknown proportion profile {profile!r}; valid profiles: {list(VALID_PROFILES)}"
        )
    return PROFILE_BONES
