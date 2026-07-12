"""Restyle a Meshy hair volume into a canonical Meridian hair asset (spec ⑤/S5).

Takes a raw Meshy-generated hair ``.glb`` (text-to-3D output, an unrigged single
mesh) and turns it into a pipeline-compliant *hair* asset:

* fitted to the canonical rig's HEAD region (the ``geo_head`` geoset span the
  greybox blockout is built from — crown at the Head bone tail, snug on the
  skull), so the hair mounts on the assembled body's head,
* budget-cleaned to a light, low-poly mesh (hair is a small attachment — well
  under the ``armor_model`` 8k-tri ceiling, Art PRD §2.1),
* single-influence skinned to the canonical ``Head`` bone (weight 1.0 per
  vertex) on the shared canonical armature, so it rides the head with the rest
  of the character,

then exported through the ``meridian_export`` E-rule gate (E100-E105) as an
``armor_model`` skinned piece — the class the pipeline uses for gear bound to a
SUBSET of canonical bones (I021/E100/E104 handle it; the strict body rules I020
/ I022 and the LOD-chain I023 apply only to ``character_model`` bodies, so hair
ships a single mesh under the documented ``lod_policy: single`` exemption).

The class choice is deliberate: the client assembler mounts a hair preset with
the same ``_mount_skinned`` path it uses for equipped gear — hair *is* a skinned
piece bound by canonical bone name — so ``armor_model`` is the class whose E/I
rules describe exactly this asset (single Head influence, no geoset naming, no
forced LOD chain). ``character_model`` would wrongly demand 63 joints + 8
geosets; ``prop`` would skip every rig check and mis-declare a lightmapped static
object. See ``tools/blender/README.md``.

Everything that does not need Blender (the head-region fit math, naming, arg
parsing) lives in importable module-level functions so the test suite covers it
with ``bpy`` absent. The mesh build + skin + glTF export only run inside
:func:`main`, reached solely under Blender.

Skinning note: like ``restyle_body``, weights are assigned deterministically in
pure Python (every vertex → ``Head`` at weight 1.0) rather than Blender's fragile
headless bone-heat, guaranteeing a valid, normalized single-influence skin that
E103/I021 accept.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bones  # noqa: E402
import generate_blockout  # noqa: E402
import generate_rig  # noqa: E402

Vec3 = tuple[float, float, float]

DEFAULT_OUT = "content/core/assets/art/char/ardent/male/hair_1.glb"

# The single canonical bone a hair asset binds to (spec ⑤/S5 — head-region mount).
HAIR_BONE = "Head"

# Exported mesh name. Deliberately NOT geo_<region>_lod<N>: E104 rejects geoset
# naming on an armor_model piece (gear binds bones, it is not a body geoset), and
# the single-mesh, no-LOD-chain shape is declared via lod_policy: single (I023).
HAIR_MESH_NAME = "hair"

# Target LOD0 triangle budget for a hair mesh. Hair is a light attachment: keep
# it low-poly, comfortably under the armor_model 8k ceiling (validate_content
# L070 / Art PRD §2.1). MAX is a hard cap the collapse-decimate never exceeds.
TARGET_TRIS = 2_500
MAX_TRIS = 6_000


def head_region_box() -> tuple[Vec3, Vec3]:
    """(lo, hi) AABB of the canonical HEAD geoset region in Blender Z-up space.

    Single-sourced from the same ``head`` region->bone-group table + radius the
    greybox blockout and ``restyle_body`` use, so the hair fits the exact head
    span the body's ``geo_head`` geoset occupies. Y-up bone points are mapped to
    the Blender Z-up space the imported sculpt lives in.
    """
    groups = generate_blockout.region_bone_groups()["head"]
    radius = generate_blockout.REGION_RADIUS.get(
        "head", generate_blockout._DEFAULT_RADIUS
    )
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for group in groups:
        lo_t, hi_t = generate_blockout.group_bbox(group, radius)
        for p in (generate_rig.yup_to_blender(lo_t), generate_rig.yup_to_blender(hi_t)):
            for i in range(3):
                lo[i] = min(lo[i], p[i])
                hi[i] = max(hi[i], p[i])
    return (lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2])


def fit_head_transform(
    mesh_min: Vec3, mesh_max: Vec3, head_lo: Vec3, head_hi: Vec3
) -> tuple[float, Vec3]:
    """Uniform scale + translation seating a hair AABB on the rig's head.

    Scales so the hair's horizontal footprint (the larger of its X/Y extent)
    matches the head region's horizontal width, then centres the hair on the
    head's vertical axis (X/Y) and aligns the hair's vertical centre with the
    head region's centre so the volume caps the skull. Returns ``(scale,
    offset)`` applied as ``p' = p * scale + offset`` per source vertex.
    """
    mesh_w = max(mesh_max[0] - mesh_min[0], mesh_max[1] - mesh_min[1], 1e-6)
    head_w = max(head_hi[0] - head_lo[0], head_hi[1] - head_lo[1])
    scale = head_w / mesh_w

    def _centre(lo: Vec3, hi: Vec3, i: int) -> float:
        return (lo[i] + hi[i]) / 2.0

    mesh_cx = _centre(mesh_min, mesh_max, 0)
    mesh_cy = _centre(mesh_min, mesh_max, 1)
    mesh_cz = _centre(mesh_min, mesh_max, 2)
    head_cx = _centre(head_lo, head_hi, 0)
    head_cy = _centre(head_lo, head_hi, 1)
    head_cz = _centre(head_lo, head_hi, 2)
    offset = (
        head_cx - mesh_cx * scale,
        head_cy - mesh_cy * scale,
        head_cz - mesh_cz * scale,
    )
    return scale, offset


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the restyle script's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="restyle_hair.py",
        description="Restyle a raw Meshy hair .glb into a canonical hair asset.",
    )
    parser.add_argument("--in", dest="in_glb", required=True, help="raw Meshy .glb")
    parser.add_argument(
        "--out", default=DEFAULT_OUT, help="output hair .glb (default: hair_1 path)"
    )
    parser.add_argument(
        "--rigdata-json",
        default=None,
        help=(
            "write the E-rule RigData snapshot (armor_model) here as JSON so the "
            "meridian_export E100-E105 gate can run from system Python. Defaults "
            "to <out>.rigdata.json."
        ),
    )
    parser.add_argument(
        "--profile",
        default="ardent_male",
        choices=list(bones.VALID_PROFILES),
        help="rig proportion profile (default: ardent_male)",
    )
    parser.add_argument(
        "--allow-unpinned-blender",
        action="store_true",
        help="DEVELOPMENT ONLY: proceed on a Blender that does not match the pin.",
    )
    return parser.parse_args(argv)


# ---------------------------------------------------------------------------
# Blender-only build. Not unit-tested (needs bpy). # pragma: no cover throughout.
# ---------------------------------------------------------------------------
def _import_and_join(in_glb: str):  # pragma: no cover - needs bpy
    """Import the Meshy .glb, join all meshes into one object, apply transforms."""
    import bpy  # noqa: PLC0415

    bpy.ops.import_scene.gltf(filepath=in_glb)
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit("restyle_hair: imported .glb carried no mesh")
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.parent = None
    for mod in list(obj.modifiers):
        obj.modifiers.remove(mod)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    obj.name = "_hair_src"
    obj.data.name = "_hair_src"
    return obj


def _obj_bbox(obj) -> tuple[Vec3, Vec3]:  # pragma: no cover - needs bpy
    """World-space AABB of a mesh object (transforms already applied)."""
    xs = [v.co.x for v in obj.data.vertices]
    ys = [v.co.y for v in obj.data.vertices]
    zs = [v.co.z for v in obj.data.vertices]
    return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def _apply_fit(obj, scale: float, offset: Vec3):  # pragma: no cover - needs bpy
    """Bake a uniform scale + translation into the mesh vertices."""
    import bpy  # noqa: PLC0415

    obj.scale = (scale, scale, scale)
    obj.location = offset
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def _tri_count(obj) -> int:  # pragma: no cover - needs bpy
    return sum(max(len(p.vertices) - 2, 0) for p in obj.data.polygons)


def _decimate_to(obj, target_tris: int):  # pragma: no cover - needs bpy
    """Collapse-decimate ``obj`` toward ``target_tris`` (no-op if already under)."""
    import bpy  # noqa: PLC0415

    cur = _tri_count(obj)
    if cur <= target_tris or cur == 0:
        return
    mod = obj.modifiers.new("decimate", "DECIMATE")
    mod.decimate_type = "COLLAPSE"
    mod.ratio = max(target_tris / cur, 0.01)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=mod.name)


def _single_bone_skin(mesh_obj, armature):  # pragma: no cover - needs bpy
    """Bind every vertex to the canonical Head bone at weight 1.0 (single influence).

    All 63 canonical vertex groups are created so the exported skin carries the
    shared canonical joint set (uniform with the body); only ``Head`` receives
    weight, so every vertex has exactly one influence (E103) and the skin binds
    only canonical bones (I021/E100).
    """
    groups = {
        name: mesh_obj.vertex_groups.new(name=name) for name in bones.bone_names()
    }
    head = groups[HAIR_BONE]
    indices = [v.index for v in mesh_obj.data.vertices]
    head.add(indices, 1.0, "REPLACE")

    mod = mesh_obj.modifiers.new("armature", "ARMATURE")
    mod.object = armature
    mesh_obj.parent = armature


def _rigdata_snapshot(
    context, meshes, armature
) -> dict:  # pragma: no cover - needs bpy
    """Plain-dict snapshot of the E-rule (E100-E105) inputs for an armor_model piece.

    Field names mirror ``rig_checks.RigData`` exactly; asset_class is
    ``armor_model`` so E101 (sockets) / E102 (geosets) correctly skip and E100
    (canonical bones) / E103 (influences) / E104 (no geoset naming) / E105
    (transforms + unit scale) apply.
    """
    max_infl = 0
    normalized = True
    mesh_max: dict[str, int] = {}
    unnormalized: list[str] = []
    for m in meshes:
        mm = 0
        mnorm = True
        for v in m.data.vertices:
            gs = [g for g in v.groups if g.weight > 0.0]
            mm = max(mm, len(gs))
            if gs and abs(sum(g.weight for g in gs) - 1.0) > 1e-3:
                mnorm = False
        mesh_max[m.name] = mm
        if not mnorm:
            unnormalized.append(m.name)
        max_infl = max(max_infl, mm)
        normalized = normalized and mnorm

    def _applied(o):
        return (
            all(abs(v) < 1e-4 for v in o.location)
            and all(abs(v) < 1e-4 for v in o.rotation_euler)
            and all(abs(v - 1.0) < 1e-4 for v in o.scale)
        )

    unit = context.scene.unit_settings
    return {
        "asset_class": "armor_model",
        "bone_names": list(armature.data.bones.keys()),
        "socket_names": [
            n for n in armature.data.bones.keys() if n.startswith("socket_")
        ],
        "mesh_names": [m.name for m in meshes],
        "max_influences": max_infl,
        "weights_normalized": normalized,
        "mesh_max_influences": mesh_max,
        "unnormalized_meshes": unnormalized,
        "object_transforms": [
            {"name": o.name, "transforms_applied": _applied(o)}
            for o in list(meshes) + [armature]
        ],
        "unit_scale_ok": (
            unit.system == "METRIC" and abs(unit.scale_length - 1.0) < 1e-4
        ),
    }


def _gate_export(
    context, meshes, armature, out_path, rigdata_json
):  # pragma: no cover - needs bpy
    """Write the E-rule RigData snapshot, then export the skinned hair .glb."""
    import json  # noqa: PLC0415

    import bpy  # noqa: PLC0415

    snapshot = _rigdata_snapshot(context, meshes, armature)
    Path(rigdata_json).parent.mkdir(parents=True, exist_ok=True)
    Path(rigdata_json).write_text(json.dumps(snapshot, indent=2), encoding="utf-8")
    print(f"[restyle_hair] wrote RigData snapshot {rigdata_json}")

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    for m in meshes:
        m.select_set(True)
    armature.select_set(True)
    bpy.context.view_layer.objects.active = armature
    bpy.ops.export_scene.gltf(
        filepath=out_path,
        export_format="GLB",
        use_selection=True,
        export_yup=True,
        export_apply=True,
        export_skins=True,
        export_animations=False,
    )
    print(f"[restyle_hair] wrote {out_path} ({len(meshes)} mesh)")


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - needs bpy
    import bpy  # noqa: PLC0415

    args = parse_args(
        generate_rig.argv_after_ddash(argv if argv is not None else sys.argv)
    )
    generate_rig.enforce_blender_pin(args.allow_unpinned_blender, tag="restyle_hair")

    context = bpy.context
    context.scene.unit_settings.system = "METRIC"
    context.scene.unit_settings.scale_length = 1.0

    generate_rig.reset_scene()
    src = _import_and_join(args.in_glb)

    head_lo, head_hi = head_region_box()
    mesh_min, mesh_max = _obj_bbox(src)
    scale, offset = fit_head_transform(mesh_min, mesh_max, head_lo, head_hi)
    _apply_fit(src, scale, offset)

    _decimate_to(src, min(TARGET_TRIS, MAX_TRIS))
    src.name = HAIR_MESH_NAME
    src.data.name = HAIR_MESH_NAME

    armature = generate_rig.build_armature(args.profile)
    _single_bone_skin(src, armature)

    rigdata_json = args.rigdata_json or (args.out + ".rigdata.json")
    _gate_export(context, [src], armature, args.out, rigdata_json)


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
