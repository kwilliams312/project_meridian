"""Restyle a Meshy base sculpt into the canonical Meridian body (spec ⑤/S4).

Takes a raw Meshy-generated humanoid ``.glb`` (text/image-to-3D output, an
unrigged single mesh) and turns it into the pipeline-compliant *body* asset:

* fitted to the canonical rig's scale + T-pose stance,
* budget-cleaned to the Art PRD §2.1 player-body band (45-60k LOD0) with an
  authored LOD0-3 chain,
* cut into the eight ``geo_<region>_lod<N>`` geoset meshes (spatial Voronoi
  partition by the same region bone-groups the greybox blockout uses),
* skinned to the canonical 63-bone armature by bone name with <=4 influences,
  normalized,

then exported through the ``meridian_export`` E-rule gate (E100-E105) as
``content/core/assets/art/char/sk_ardent_male_base.glb`` — the same asset id the
blockout occupied (a content swap; the reviewed PR flips ``restyle_status: done``).

This is the deterministic *restyle* half of the workflow; the non-deterministic
half (the Meshy generation) is driven separately by ``tools/meshy``. Everything
that does not need Blender (region partition math, LOD ratios, fit transform,
naming, arg parsing) lives in importable module-level functions so the test
suite covers it with ``bpy`` absent. The mesh build + skin + glTF export only run
inside :func:`main`, reached solely under Blender.

Skinning note: this module assigns skin weights by bone-segment proximity (each
vertex to its <=4 nearest canonical bones, inverse-square weighted, normalized)
rather than Blender's bone-heat ``ARMATURE_AUTO``. Bone-heat is fragile headless
on an arbitrary Meshy surface (it silently leaves vertices unweighted, which
would export zero-sum weights and fail the committed-body structural test);
proximity binding is deterministic and *guarantees* every vertex carries a
valid, normalized <=4-bone influence set. The contract E103/I020 enforce (bound
by canonical bone name, <=4 influences, normalized, exactly 63 skin joints) is
identical either way.
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

DEFAULT_OUT = "content/core/assets/art/char/sk_ardent_male_base.glb"

# Authored LOD chain: LOD0 is full, each subsequent level ~halves the triangle
# budget (decimate collapse ratio relative to LOD0). Four levels satisfy I023's
# "lod0 + lod1+" chain per geoset region; the ratios are Art PRD §2.2 guidance
# (progressive halving), not a hard lint.
LOD_RATIOS: tuple[float, ...] = (1.0, 0.5, 0.25, 0.125)

# Target LOD0 triangle budget for the whole body (sum across the 8 geosets).
# Inside the 45-60k player-body band, with headroom under the 60k class ceiling
# (validate_content L070) so the geoset cut's small per-mesh rounding can't push
# the total over.
TARGET_LOD0_TRIS = 52_000
MAX_LOD0_TRIS = 58_000


def mesh_name(region: str, lod: int) -> str:
    """Geoset mesh name: ``geo_<region>_lod<N>`` (spec ④ §3, E102/I022/I023)."""
    return f"geo_{region}_lod{lod}"


def region_anchors() -> list[tuple[str, Vec3]]:
    """Flat ``(region, center)`` anchor list in Blender Z-up space.

    One anchor per bone-group of each geoset region (so paired limbs get a
    left and a right anchor). A mesh point is assigned to the region of its
    nearest anchor (:func:`nearest_region`) — a Voronoi partition of the body
    into the eight geosets, using the exact region->bone-group table the
    greybox blockout is built from (single-sourced via
    :func:`generate_blockout.region_bone_groups`).
    """
    anchors: list[tuple[str, Vec3]] = []
    for region, groups in generate_blockout.region_bone_groups().items():
        radius = generate_blockout.REGION_RADIUS.get(
            region, generate_blockout._DEFAULT_RADIUS
        )
        for group in groups:
            lo_t, hi_t = generate_blockout.group_bbox(group, radius)
            center_t = tuple((a + b) / 2.0 for a, b in zip(lo_t, hi_t))
            anchors.append((region, generate_rig.yup_to_blender(center_t)))
    return anchors


def nearest_region(point: Vec3, anchors: list[tuple[str, Vec3]]) -> str:
    """Return the geoset region whose anchor is nearest ``point`` (squared dist)."""
    best_region = anchors[0][0]
    best_d2 = float("inf")
    for region, center in anchors:
        d2 = sum((p - c) ** 2 for p, c in zip(point, center))
        if d2 < best_d2:
            best_d2 = d2
            best_region = region
    return best_region


def rig_bounds_z() -> tuple[float, float]:
    """(min_z, max_z) of the canonical rig in Blender space, over all bone ends.

    Used to fit an imported sculpt to the rig's height. Bone table points are
    Y-up; :func:`generate_rig.yup_to_blender` maps them to the Blender Z-up
    space the imported mesh lives in.
    """
    zs: list[float] = []
    for spec in bones.ALL_BONES:
        for p in (spec.head_m, spec.tail_m):
            zs.append(generate_rig.yup_to_blender(p)[2])
    return min(zs), max(zs)


def fit_transform(
    mesh_min: Vec3, mesh_max: Vec3, rig_min_z: float, rig_max_z: float
) -> tuple[float, Vec3]:
    """Uniform scale + translation fitting a mesh AABB onto the rig's stance.

    Scales so the mesh height (Z extent) matches the rig's height, then
    translates so the scaled mesh's feet sit at the rig's floor and its X/Y
    centre is on the rig's vertical axis (X=Y=0). Returns ``(scale, offset)``
    applied as ``p' = p * scale + offset`` per source vertex.
    """
    mesh_h = max(mesh_max[2] - mesh_min[2], 1e-6)
    scale = (rig_max_z - rig_min_z) / mesh_h
    cx = (mesh_min[0] + mesh_max[0]) / 2.0
    cy = (mesh_min[1] + mesh_max[1]) / 2.0
    offset = (
        -cx * scale,
        -cy * scale,
        rig_min_z - mesh_min[2] * scale,
    )
    return scale, offset


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the restyle script's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="restyle_body.py",
        description="Restyle a raw Meshy humanoid .glb into the canonical body asset.",
    )
    parser.add_argument("--in", dest="in_glb", required=True, help="raw Meshy .glb")
    parser.add_argument(
        "--out", default=DEFAULT_OUT, help="output body .glb (default: committed path)"
    )
    parser.add_argument(
        "--rigdata-json",
        default=None,
        help=(
            "write the E-rule RigData snapshot (bone/mesh/influence/transform "
            "facts observed in Blender) here as JSON, so the meridian_export "
            "E100-E105 gate can run from system Python (Blender's bundled Python "
            "has no PyYAML, which rig_checks needs). Defaults to <out>.rigdata.json."
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
        raise SystemExit("restyle: imported .glb carried no mesh")
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    # Drop any imported parent/armature so only our canonical rig binds later.
    obj.parent = None
    for mod in list(obj.modifiers):
        obj.modifiers.remove(mod)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    obj.name = "_restyle_src"
    obj.data.name = "_restyle_src"
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


def _budget_lod0(obj):  # pragma: no cover - needs bpy
    """Fit the source mesh's LOD0 triangle count into the player-body band.

    Subdivides once if the raw sculpt is under-budget (Meshy's default polycount
    can land below the 45k floor), then collapse-decimates down to TARGET.
    """
    import bpy  # noqa: PLC0415

    if _tri_count(obj) < TARGET_LOD0_TRIS:
        bpy.context.view_layer.objects.active = obj
        mod = obj.modifiers.new("subsurf", "SUBSURF")
        mod.subdivision_type = "SIMPLE"
        mod.levels = 1
        mod.render_levels = 1
        bpy.ops.object.modifier_apply(modifier=mod.name)
    _decimate_to(obj, min(TARGET_LOD0_TRIS, MAX_LOD0_TRIS))


def _partition_regions(obj):  # pragma: no cover - needs bpy
    """Split ``obj`` into one mesh per geoset region by nearest-anchor Voronoi.

    Returns ``{region: object}``. Every one of the 8 regions is guaranteed a
    (non-empty) mesh: any region that captured no faces from the sculpt falls
    back to a blockout proxy box for that region so the geoset cut is always
    complete (I022 needs all 8 at lod0).
    """
    import bmesh  # noqa: PLC0415
    import bpy  # noqa: PLC0415

    anchors = region_anchors()
    regions = list(generate_blockout.region_bone_groups().keys())

    # Label every polygon with its region index via a face integer layer.
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.faces.ensure_lookup_table()
    layer = bm.faces.layers.int.new("region_idx")
    region_index = {r: i for i, r in enumerate(regions)}
    for f in bm.faces:
        c = f.calc_center_median()
        f[layer] = region_index[nearest_region((c.x, c.y, c.z), anchors)]
    bm.to_mesh(me)
    bm.free()

    result: dict[str, object] = {}
    for region in regions:
        idx = region_index[region]
        # Duplicate the source, keep only this region's faces, or fall back.
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.duplicate()
        dup = bpy.context.view_layer.objects.active
        kept = _keep_region_faces(dup, idx)
        if kept == 0:
            bpy.data.objects.remove(dup, do_unlink=True)
            result[region] = _region_fallback_box(region)
        else:
            dup.name = mesh_name(region, 0)
            dup.data.name = mesh_name(region, 0)
            result[region] = dup

    bpy.data.objects.remove(obj, do_unlink=True)
    return result


def _keep_region_faces(dup, region_idx: int) -> int:  # pragma: no cover - needs bpy
    """Delete every face NOT labelled ``region_idx``; return kept face count."""
    import bmesh  # noqa: PLC0415

    bm = bmesh.new()
    bm.from_mesh(dup.data)
    bm.faces.ensure_lookup_table()
    layer = bm.faces.layers.int.get("region_idx")
    doomed = [f for f in bm.faces if f[layer] != region_idx]
    kept = len(bm.faces) - len(doomed)
    if doomed:
        bmesh.ops.delete(bm, geom=doomed, context="FACES")
    bm.to_mesh(dup.data)
    bm.free()
    if kept:
        # Drop now-orphaned loose vertices left by face deletion.
        _remove_loose(dup)
    return kept


def _remove_loose(obj):  # pragma: no cover - needs bpy
    import bmesh  # noqa: PLC0415

    bm = bmesh.new()
    bm.from_mesh(obj.data)
    loose = [v for v in bm.verts if not v.link_faces]
    if loose:
        bmesh.ops.delete(bm, geom=loose, context="VERTS")
    bm.to_mesh(obj.data)
    bm.free()


def _region_fallback_box(region: str):  # pragma: no cover - needs bpy
    """A blockout-style proxy box for a region the sculpt didn't cover."""
    groups = generate_blockout.region_bone_groups()[region]
    radius = generate_blockout.REGION_RADIUS.get(
        region, generate_blockout._DEFAULT_RADIUS
    )
    # Merge the region's bone-groups into a single enclosing box.
    lo_all = [float("inf")] * 3
    hi_all = [float("-inf")] * 3
    for group in groups:
        lo_t, hi_t = generate_blockout.group_bbox(group, radius)
        lo_b = generate_rig.yup_to_blender(lo_t)
        hi_b = generate_rig.yup_to_blender(hi_t)
        for i in range(3):
            lo_all[i] = min(lo_all[i], lo_b[i], hi_b[i])
            hi_all[i] = max(hi_all[i], lo_b[i], hi_b[i])
    box = generate_blockout._add_box(tuple(lo_all), tuple(hi_all), mesh_name(region, 0))
    box.data.name = mesh_name(region, 0)
    return box


def _bone_segments(armature):  # pragma: no cover - needs bpy
    """[(bone_name, head_vec, tail_vec)] in armature/object space."""
    segs = []
    for b in armature.data.bones:
        segs.append((b.name, b.head_local.copy(), b.tail_local.copy()))
    return segs


def _point_segment_dist2(p, a, b) -> float:  # pragma: no cover - needs bpy
    ab = b - a
    denom = ab.dot(ab)
    t = 0.0 if denom == 0.0 else max(0.0, min(1.0, (p - a).dot(ab) / denom))
    closest = a + ab * t
    return (p - closest).length_squared


def _proximity_skin(mesh_obj, armature, segments):  # pragma: no cover - needs bpy
    """Bind every vertex to its <=4 nearest canonical bones, normalized.

    Guarantees full coverage: every vertex gets a valid influence set, so the
    exported skin never carries a zero-sum-weight vertex. All 63 canonical
    vertex groups are created (mirroring ARMATURE_AUTO) so the exporter emits
    the full 63-joint skin the blockout established.
    """
    # Create a vertex group per canonical bone (empty groups are harmless).
    groups = {name: mesh_obj.vertex_groups.new(name=name) for name in bones.bone_names()}

    for v in mesh_obj.data.vertices:
        p = v.co
        dists = [
            (name, _point_segment_dist2(p, head, tail))
            for name, head, tail in segments
        ]
        dists.sort(key=lambda t: t[1])
        nearest = dists[:4]
        raw = [(name, 1.0 / (d2 + 1e-6)) for name, d2 in nearest]
        total = sum(w for _, w in raw)
        for name, w in raw:
            groups[name].add([v.index], w / total, "REPLACE")

    mod = mesh_obj.modifiers.new("armature", "ARMATURE")
    mod.object = armature
    mesh_obj.parent = armature


def _derive_lods(region_mesh, region: str):  # pragma: no cover - needs bpy
    """Return [lod0, lod1, lod2, lod3] objects for a skinned region mesh.

    lod0 is the input; each lower LOD is a decimated duplicate that inherits
    interpolated skin weights, re-clamped to <=4 influences and re-normalized.
    """
    import bpy  # noqa: PLC0415

    region_mesh.name = mesh_name(region, 0)
    region_mesh.data.name = mesh_name(region, 0)
    lods = [region_mesh]
    base_tris = _tri_count(region_mesh)
    for lod, ratio in enumerate(LOD_RATIOS[1:], start=1):
        bpy.ops.object.select_all(action="DESELECT")
        region_mesh.select_set(True)
        bpy.context.view_layer.objects.active = region_mesh
        bpy.ops.object.duplicate()
        dup = bpy.context.view_layer.objects.active
        dup.name = mesh_name(region, lod)
        dup.data.name = mesh_name(region, lod)
        _decimate_to(dup, max(int(base_tris * ratio), 4))
        _clamp_and_normalize(dup)
        lods.append(dup)
    return lods


def _clamp_and_normalize(mesh_obj):  # pragma: no cover - needs bpy
    """<=4 influences per vertex + renormalize (E103)."""
    import bpy  # noqa: PLC0415

    bpy.ops.object.select_all(action="DESELECT")
    mesh_obj.select_set(True)
    bpy.context.view_layer.objects.active = mesh_obj
    bpy.ops.object.vertex_group_limit_total(limit=4)
    bpy.ops.object.vertex_group_normalize_all(lock_active=False)


def _rigdata_snapshot(context, meshes, armature) -> dict:  # pragma: no cover - needs bpy
    """Plain-dict snapshot of the E-rule (E100-E105) inputs observed in Blender.

    Serialized to JSON so the real ``meridian_export.rig_checks`` gate can run in
    system Python (Blender's bundled Python has no PyYAML, which rig_checks needs
    to load the geoset/bone vocabulary). Field names mirror
    ``rig_checks.RigData`` exactly.
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
        "asset_class": "character_model",
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


def _gate_export(context, meshes, armature, out_path, rigdata_json):  # pragma: no cover - needs bpy
    """Write the E-rule RigData snapshot, then export the whole scene .glb.

    The E100-E105 verdict is rendered by the system-Python gate that reads the
    snapshot JSON (see the module docstring); here we only capture the facts and
    perform the export with meridian_export's baked-in axis/unit conventions.
    """
    import json  # noqa: PLC0415

    import bpy  # noqa: PLC0415

    snapshot = _rigdata_snapshot(context, meshes, armature)
    Path(rigdata_json).parent.mkdir(parents=True, exist_ok=True)
    Path(rigdata_json).write_text(json.dumps(snapshot, indent=2), encoding="utf-8")
    print(f"[restyle] wrote RigData snapshot {rigdata_json}")

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
    print(f"[restyle] wrote {out_path} ({len(meshes)} geoset meshes)")


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - needs bpy
    import bpy  # noqa: PLC0415

    args = parse_args(
        generate_rig.argv_after_ddash(argv if argv is not None else sys.argv)
    )
    generate_rig.enforce_blender_pin(args.allow_unpinned_blender, tag="restyle_body")

    context = bpy.context
    context.scene.unit_settings.system = "METRIC"
    context.scene.unit_settings.scale_length = 1.0

    generate_rig.reset_scene()
    src = _import_and_join(args.in_glb)

    rig_min_z, rig_max_z = rig_bounds_z()
    mesh_min, mesh_max = _obj_bbox(src)
    scale, offset = fit_transform(mesh_min, mesh_max, rig_min_z, rig_max_z)
    _apply_fit(src, scale, offset)

    _budget_lod0(src)
    region_meshes = _partition_regions(src)  # consumes src

    armature = generate_rig.build_armature(args.profile)
    segments = _bone_segments(armature)

    all_meshes = []
    for region, mesh0 in region_meshes.items():
        # Skin LOD0, then derive lower LODs by decimation. Duplicating a skinned
        # mesh copies its armature modifier + parent, so every LOD stays bound.
        _proximity_skin(mesh0, armature, segments)
        _clamp_and_normalize(mesh0)
        all_meshes.extend(_derive_lods(mesh0, region))

    rigdata_json = args.rigdata_json or (args.out + ".rigdata.json")
    _gate_export(context, all_meshes, armature, args.out, rigdata_json)


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
