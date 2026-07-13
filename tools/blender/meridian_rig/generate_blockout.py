"""Deterministic greybox blockout-body generator (spec ④ §6 / T5).

Builds the Meridian humanoid *body* as eight geoset-cut primitive meshes skinned
to the canonical armature, and exports it as a single skinned glTF binary
(``.glb``). This is the end-to-end pipeline proof: the same armature machinery as
:mod:`generate_rig` (bone table -> Z-up edit bones -> ``export_yup`` round-trip),
now carrying meshes named per the geoset convention (``geo_<region>_lod0``),
parented with automatic weights and clamped to <=4 influences.

The committed artifact is regenerated with::

    /path/to/blender --background --factory-startup -noaudio \
        --python tools/blender/meridian_rig/generate_blockout.py -- \
        --profile ardent_male \
        --out content/core/assets/art/char/sk_ardent_male_base.glb

Everything that does not need Blender (region->bone mapping, bbox sizing, mesh
naming, argument parsing) lives in importable module-level functions so the test
suite covers it with ``bpy`` absent. The mesh build + skin + glTF export only run
inside :func:`main`, reached solely under Blender. The armature itself is built by
:func:`generate_rig.build_armature` — this module never duplicates that machinery
(including its ``yup_to_blender`` rest-transform handling, which fixed a real
-90 deg rotation bug).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

# The bones table + rig generator are pure/importable; reuse them directly.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bones  # noqa: E402
import generate_rig  # noqa: E402

Vec3 = tuple[float, float, float]

# Repo-root-relative default output path for the committed blockout body.
DEFAULT_OUT = "content/core/assets/art/char/sk_ardent_male_base.glb"


def _finger_bone_names(side: str) -> list[str]:
    """Return the finger bone names for one side ('Left'/'Right'), table order."""
    tokens = ("Thumb", "Index", "Middle", "Ring", "Little")
    return [
        b.name
        for b in bones.ALL_BONES
        if b.name.startswith(side) and any(tok in b.name for tok in tokens)
    ]


def region_bone_groups() -> dict[str, list[list[str]]]:
    """Map each geoset region to its primitive bone-groups (one box per group).

    Keys are exactly the 8 ``skeleton.defs.yaml`` geoset regions — drift-guarded
    by ``tests/test_meridian_rig.py``. Values are lists of bone-name groups; each
    group is sized into one box via :func:`group_bbox`, and a region's boxes are
    joined into a single ``geo_<region>_lod0`` mesh. Paired (left/right) regions
    use one group per side so each box hugs its own limb and automatic weights
    stay sane. Region->bones is the ONLY body-specific knowledge in this module;
    the region vocabulary itself stays single-sourced in skeleton.defs.yaml.
    """
    return {
        "head": [["Head", "Neck"]],
        "torso": [["Spine", "Chest", "UpperChest", "LeftShoulder", "RightShoulder"]],
        # `forearms` is the WHOLE arm (shoulder joint -> wrist), elbow-agnostic:
        # each arm carries TWO anchors so the geoset cut claims the entire arm and
        # never leaves an upper-arm patch behind in `torso` (issue #587). Without
        # the upper-arm anchor the only per-arm anchor sat near the elbow, so
        # shoulder-line upper-arm surface fell to the nearer `torso` anchor and was
        # erased whenever a torso-hiding chest was worn, orphaning the forearm.
        #   * ``[<side>UpperArm]``            — anchor on the upper arm, claims the
        #     shoulder-line surface for the arm (the #587 fix).
        #   * ``[<side>UpperArm, <side>LowerArm]`` — the original elbow-centred
        #     anchor, kept verbatim so the forearm/hand (wrist) seam is unchanged.
        "forearms": [
            ["RightUpperArm"],
            ["RightUpperArm", "RightLowerArm"],
            ["LeftUpperArm"],
            ["LeftUpperArm", "LeftLowerArm"],
        ],
        "hands": [
            ["RightHand", *_finger_bone_names("Right")],
            ["LeftHand", *_finger_bone_names("Left")],
        ],
        "waist": [["Hips"]],
        "hips_legs": [["RightUpperLeg"], ["LeftUpperLeg"]],
        "lower_legs": [["RightLowerLeg"], ["LeftLowerLeg"]],
        "feet": [["RightFoot", "RightToes"], ["LeftFoot", "LeftToes"]],
    }


# Minimum half-extent (metres) added around each region's bone span so thin
# single-axis spans still yield a box with plausible limb girth. Larger for the
# bulky central regions, smaller for the extremities.
REGION_RADIUS: dict[str, float] = {
    "head": 0.11,
    "torso": 0.17,
    "forearms": 0.06,
    "hands": 0.05,
    "waist": 0.16,
    "hips_legs": 0.09,
    "lower_legs": 0.07,
    "feet": 0.06,
}
_DEFAULT_RADIUS = 0.08


def mesh_name(region: str) -> str:
    """Geoset mesh name for a region: ``geo_<region>_lod0`` (spec ④ §3, E102/I022)."""
    return f"geo_{region}_lod0"


def _bone_by_name(profile: str = "ardent_male") -> dict[str, bones.BoneSpec]:
    return {b.name: b for b in bones.for_profile(profile)}


def group_bbox(
    bone_names: list[str], radius: float, profile: str = "ardent_male"
) -> tuple[Vec3, Vec3]:
    """Axis-aligned bounding box (table Y-up space) over a bone group's head+tail.

    Every axis is expanded so its half-extent is at least ``radius``, giving even
    a single collinear bone chain a solid volume to skin. ``profile`` selects the
    proportion profile's rest geometry (default ``ardent_male``); a second race
    (Dolmen D2) sizes the geoset boxes from its own shorter/broader bone table.
    """
    table = _bone_by_name(profile)
    pts: list[Vec3] = []
    for name in bone_names:
        spec = table[name]
        pts.append(spec.head_m)
        pts.append(spec.tail_m)
    lo = [min(p[i] for p in pts) for i in range(3)]
    hi = [max(p[i] for p in pts) for i in range(3)]
    for i in range(3):
        center = (lo[i] + hi[i]) / 2.0
        half = max((hi[i] - lo[i]) / 2.0, radius)
        lo[i], hi[i] = center - half, center + half
    return (lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2])


# ---------------------------------------------------------------------------
# CLI helpers (pure; reuse generate_rig's ``--`` splitting).
# ---------------------------------------------------------------------------
def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the generator's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="generate_blockout.py",
        description="Generate the Meridian greybox blockout body .glb from the bones table.",
    )
    parser.add_argument(
        "--profile",
        default="ardent_male",
        choices=list(bones.VALID_PROFILES),
        help="proportion profile to build (default: ardent_male)",
    )
    parser.add_argument(
        "--out",
        default=DEFAULT_OUT,
        help="output .glb path (default: the committed blockout-body path)",
    )
    parser.add_argument(
        "--allow-unpinned-blender",
        action="store_true",
        help=(
            "DEVELOPMENT ONLY: proceed even though the running Blender does "
            "not match blender_pin.PINNED_VERSION. Without it, an unpinned "
            "Blender is refused so the export is never silently "
            "non-byte-identical to the committed artifact."
        ),
    )
    return parser.parse_args(argv)


# ---------------------------------------------------------------------------
# Blender-only build (mesh primitives + skin + export). Not unit-tested.
# ---------------------------------------------------------------------------
def _add_box(min_b: Vec3, max_b: Vec3, name: str):  # pragma: no cover - needs bpy
    """Add a cube filling the Blender-space AABB [min_b, max_b], baked + named."""
    import bpy  # noqa: PLC0415 - Blender-only import

    center = tuple((a + b) / 2.0 for a, b in zip(min_b, max_b))
    half = tuple(max((b - a) / 2.0, 1e-4) for a, b in zip(min_b, max_b))
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=center)  # unit cube: verts +/-1
    obj = bpy.context.active_object
    obj.scale = half
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    obj.name = name
    return obj


def _join(objs, name: str):  # pragma: no cover - needs bpy
    """Join ``objs`` into a single mesh object named ``name`` and return it."""
    import bpy  # noqa: PLC0415 - Blender-only import

    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    if len(objs) > 1:
        bpy.ops.object.join()
    joined = bpy.context.view_layer.objects.active
    joined.name = name
    joined.data.name = name
    return joined


def build_body(profile: str):  # pragma: no cover - requires bpy/Blender
    """Build the skinned blockout body: armature + 8 geoset meshes, <=4 influences.

    Reuses :func:`generate_rig.build_armature` for the armature (never duplicates
    the bone-table -> Z-up conversion). Each geoset region's bone-groups become
    boxes (``bpy.ops.mesh.primitive_cube_add``), sized from the bone span via
    :func:`group_bbox` and mapped table Y-up -> Blender Z-up with
    :func:`generate_rig.yup_to_blender`, joined per region into
    ``geo_<region>_lod0``, then parented to the armature with automatic weights
    and clamped/normalised to <=4 influences per vertex (E103/I023).
    """
    import bpy  # noqa: PLC0415 - Blender-only import

    armature = generate_rig.build_armature(profile)

    region_meshes = []
    for region, bone_groups in region_bone_groups().items():
        radius = REGION_RADIUS.get(region, _DEFAULT_RADIUS)
        boxes = []
        for i, group in enumerate(bone_groups):
            lo_t, hi_t = group_bbox(group, radius, profile)
            lo_b = generate_rig.yup_to_blender(lo_t)
            hi_b = generate_rig.yup_to_blender(hi_t)
            # yup_to_blender negates one axis, so recompute min/max per axis.
            min_b = tuple(min(a, b) for a, b in zip(lo_b, hi_b))
            max_b = tuple(max(a, b) for a, b in zip(lo_b, hi_b))
            boxes.append(_add_box(min_b, max_b, f"_tmp_{region}_{i}"))
        region_meshes.append(_join(boxes, mesh_name(region)))

    # Parent every geoset mesh to the armature with automatic (bone-heat) weights.
    bpy.ops.object.select_all(action="DESELECT")
    for m in region_meshes:
        m.select_set(True)
    armature.select_set(True)
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")

    # Clamp to <=4 influences and renormalise weights (E103 ceiling).
    for m in region_meshes:
        bpy.ops.object.select_all(action="DESELECT")
        m.select_set(True)
        bpy.context.view_layer.objects.active = m
        bpy.ops.object.vertex_group_limit_total(limit=4)
        bpy.ops.object.vertex_group_normalize_all(lock_active=False)

    return armature, region_meshes


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - requires bpy
    """Blender entry point: build the skinned blockout body and write the .glb."""
    args = parse_args(
        generate_rig.argv_after_ddash(argv if argv is not None else sys.argv)
    )
    generate_rig.enforce_blender_pin(
        args.allow_unpinned_blender, tag="generate_blockout"
    )
    generate_rig.reset_scene()
    build_body(args.profile)
    # Reuse the rig exporter's settings verbatim (GLB, Y-up, skins on, whole
    # scene) — the only difference from the skeleton export is that the scene
    # now carries meshes.
    generate_rig.export_glb(args.out)
    print(f"[generate_blockout] wrote {args.out} for profile {args.profile!r}")


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
