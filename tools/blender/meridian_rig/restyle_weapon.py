"""Restyle a Meshy weapon sculpt into a canonical socket-mounted weapon (#605).

Takes a raw Meshy-generated weapon ``.glb`` (text-to-3D output, a single unrigged
mesh — e.g. a sword blade + crossguard + grip + pommel) and turns it into a
pipeline-compliant *weapon* asset the assembler mounts on a bone socket.

A weapon is far simpler than armor (``restyle_armor.py``): it is a STATIC rigid
mesh attached to a ``socket_*`` bone via ``BoneAttachment3D`` (assembler
``_mount_socketed``), NOT skinned to the body and NOT fit to a body region. So the
restyle does NO body-fit, NO skinning, NO dye UV — it only needs to:

* orient the sculpt to the canonical socketed-weapon convention proven by the
  rusty_pickaxe (which renders correctly on ``main_hand``): the weapon's LONG axis
  (the blade) points along Blender +Z — which ``export_yup`` maps to glTF +Y, "up"
  out of the fist — the second axis (the crossguard) lies along X, and the thin
  flat of the blade along Y; the grip/guard end sits at the bottom (−Z) where the
  hand grips, the blade tip at the top,
* scale the whole piece to a plausible one-handed length (~0.9 m blade+grip),
* seat the pommel just below the socket origin (matching the pickaxe, whose grip
  bottom sits at ~−0.06 m) so the fist closes around the grip,
* decimate to the Art PRD §2.1 weapon band (a one-hander stays ≤12k, under the
  15k legendary ceiling / ``weapon_model`` L070 budget),
* strip Meshy's baked photo-texture + material and assign a flat steel PBR base
  (the multi-MB source texture never ships; the shipped ``.glb`` stays a small
  static mesh matching the pickaxe's single-node / no-skin / no-animation shape),

then export a single static mesh (no armature, no skin, no animations) — the exact
structure ``_mount_socketed`` loads for the pickaxe today.

The deterministic orientation is the whole game: Meshy emits an arbitrary pose, so
the pure functions below choose the up axis from the AABB (longest extent = blade),
choose which END is the grip from the vertex CENTROID (the guard/grip/pommel mass
outweighs the thin blade, so the centroid sits toward the grip), and build a signed
axis permutation (a proper rotation, det +1) that lands the canonical pose. Given
the committed sculpt this pass is deterministic — pure AABB/centroid math + a fixed
decimate ratio.

Everything that does not need Blender (axis choice, permutation, scale, seat) lives
in importable module-level functions the test suite covers with ``bpy`` absent; the
mesh import + decimate + material + export only run inside :func:`main`.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_rig  # noqa: E402

Vec3 = tuple[float, float, float]

DEFAULT_OUT = "content/core/assets/art/item/weapon/iron_sword.glb"

# Exported mesh name — the per-asset name, matching the pickaxe's mesh name
# ("pickaxe_rusty"). A weapon is not a geoset, so no geo_<region>_lod<N> naming.
DEFAULT_MESH_NAME = "iron_sword"

# Canonical socketed-weapon pose (Blender Z-up; export_yup maps Z->glTF Y).
# blade -> Z (up out of the fist), crossguard -> X, blade flat -> Y. This is the
# exact convention the rusty_pickaxe .glb ships in and the assembler mounts today.
UP_AXIS = 2  # Blender Z
GUARD_AXIS = 0  # Blender X
FLAT_AXIS = 1  # Blender Y

# Target total length (blade + grip) of a one-handed arming sword, metres. The
# pickaxe is 0.80 m; a 1h sword reads a touch longer. Uniform-scaled from the raw
# sculpt's blade extent so the blade proportion is preserved.
TARGET_LEN_M = 0.90

# Where the pommel (grip bottom) seats on the up axis, metres. Matches the pickaxe
# (grip bottom ~−0.06), so the fist at the socket origin closes around the grip.
POMMEL_SEAT_M = -0.06

# Decimate target / hard cap for a one-handed weapon (Art PRD §2.1 weapon 6-12k;
# 2-hander/legendary up to 15k). A 1h sword targets the middle of the band and
# never exceeds MAX, staying under the 15k weapon_model L070 ceiling.
TARGET_TRIS = 10_000
MAX_TRIS = 12_000

# Flat steel base, replacing Meshy's baked photo material (kept off the shipped
# .glb). A plain polished-steel Principled BSDF — no dye path for weapons (M1).
STEEL_BASE_COLOR = (0.60, 0.62, 0.66, 1.0)
STEEL_METALLIC = 0.85
STEEL_ROUGHNESS = 0.35


def sorted_axes_by_extent(extent: Vec3) -> tuple[int, int, int]:
    """Axis indices ordered by AABB extent, longest first (blade, guard, flat).

    Ties break by axis index (deterministic). The longest extent of a
    blade+crossguard+grip sculpt is the blade; the next is the crossguard span;
    the shortest is the blade's flat thickness.
    """
    return tuple(sorted(range(3), key=lambda i: (-extent[i], i)))  # type: ignore[return-value]


def grip_on_positive_side(centroid: Vec3, aabb_center: Vec3, blade_axis: int) -> bool:
    """Whether the grip end lies on the +blade_axis side of the mesh.

    The guard + grip + pommel carry far more geometry than the thin blade, so the
    vertex CENTROID sits toward the grip end. If the centroid is beyond the AABB
    centre on the +blade_axis side, the grip is on the positive side (and must be
    flipped DOWN so the blade points up).
    """
    return centroid[blade_axis] > aabb_center[blade_axis]


def signed_permutation(
    axes: tuple[int, int, int], grip_positive: bool
) -> tuple[tuple[int, int], tuple[int, int], tuple[int, int]]:
    """Signed axis permutation mapping the sculpt's axes to the canonical pose.

    ``axes`` = (blade, guard, flat) source axis indices (from
    :func:`sorted_axes_by_extent`). Returns three ``(source_axis, sign)`` pairs —
    one per TARGET axis in order (X=GUARD_AXIS, Y=FLAT_AXIS, Z=UP_AXIS) — so a
    caller builds ``new[t] = sign * old[source]``. The blade maps to +Z unless the
    grip is on the +blade side (then −Z, flipping the grip DOWN). The flat-axis
    sign is chosen last to force a PROPER rotation (determinant +1, no mirroring) —
    safe because the blade flat is symmetric.
    """
    blade, guard, flat = axes
    z_sign = -1 if grip_positive else 1
    x_sign = 1
    # Rows indexed by target axis: X<-guard, Y<-flat, Z<-blade.
    rows = {GUARD_AXIS: (guard, x_sign), FLAT_AXIS: (flat, 1), UP_AXIS: (blade, z_sign)}
    det = _perm_determinant(rows)
    if det < 0:
        # Flip the (symmetric) flat axis to make the transform a proper rotation.
        s, sign = rows[FLAT_AXIS]
        rows[FLAT_AXIS] = (s, -sign)
    return (rows[0], rows[1], rows[2])


def _perm_determinant(
    rows: dict[int, tuple[int, int]],
) -> int:
    """Determinant (+1/−1) of the 3x3 signed-permutation built from target rows."""
    mat = [[0, 0, 0], [0, 0, 0], [0, 0, 0]]
    for target, (source, sign) in rows.items():
        mat[target][source] = sign
    a, b, c = mat[0]
    d, e, f = mat[1]
    g, h, i = mat[2]
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


def uniform_scale(blade_extent: float, target_len: float = TARGET_LEN_M) -> float:
    """Uniform scale mapping the raw blade extent to the target weapon length."""
    return target_len / max(blade_extent, 1e-6)


def pommel_offset(min_up_after_scale: float, seat: float = POMMEL_SEAT_M) -> float:
    """Up-axis translation seating the scaled pommel (min on up axis) at ``seat``."""
    return seat - min_up_after_scale


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the restyle script's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="restyle_weapon.py",
        description="Restyle a raw Meshy weapon .glb into a canonical socketed weapon.",
    )
    parser.add_argument("--in", dest="in_glb", required=True, help="raw Meshy .glb")
    parser.add_argument(
        "--out", default=DEFAULT_OUT, help="output weapon .glb (default: iron_sword)"
    )
    parser.add_argument(
        "--mesh-name",
        default=DEFAULT_MESH_NAME,
        help="exported mesh/object name (default: iron_sword)",
    )
    parser.add_argument(
        "--target-len",
        type=float,
        default=TARGET_LEN_M,
        help=f"total weapon length in metres (default: {TARGET_LEN_M})",
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
    """Import the Meshy .glb, join all meshes into one, apply transforms."""
    import bpy  # noqa: PLC0415

    bpy.ops.import_scene.gltf(filepath=in_glb)
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit("restyle_weapon: imported .glb carried no mesh")
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
    obj.name = "_weapon_src"
    obj.data.name = "_weapon_src"
    return obj


def _obj_bbox(obj) -> tuple[Vec3, Vec3]:  # pragma: no cover - needs bpy
    """World-space AABB of a mesh object (transforms already applied)."""
    xs = [v.co.x for v in obj.data.vertices]
    ys = [v.co.y for v in obj.data.vertices]
    zs = [v.co.z for v in obj.data.vertices]
    return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def _obj_centroid(obj) -> Vec3:  # pragma: no cover - needs bpy
    """Mean vertex position (grip end carries the mass; blade is thin)."""
    n = max(len(obj.data.vertices), 1)
    sx = sum(v.co.x for v in obj.data.vertices)
    sy = sum(v.co.y for v in obj.data.vertices)
    sz = sum(v.co.z for v in obj.data.vertices)
    return (sx / n, sy / n, sz / n)


def _apply_orient(obj, perm):  # pragma: no cover - needs bpy
    """Bake the signed axis permutation into the mesh vertices (canonical pose)."""
    import bmesh  # noqa: PLC0415

    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    for v in bm.verts:
        old = (v.co.x, v.co.y, v.co.z)
        new = [0.0, 0.0, 0.0]
        for target in range(3):
            source, sign = perm[target]
            new[target] = sign * old[source]
        v.co = new
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    me.update()


def _apply_scale_seat(
    obj, scale: float, seat_off: float
):  # pragma: no cover - needs bpy
    """Uniform-scale the mesh, then seat the pommel on the up axis."""
    import bpy  # noqa: PLC0415

    obj.scale = (scale, scale, scale)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    loc = [0.0, 0.0, 0.0]
    loc[UP_AXIS] = seat_off
    obj.location = tuple(loc)
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


def _apply_steel_material(obj):  # pragma: no cover - needs bpy
    """Strip Meshy's textured material and assign the flat steel base."""
    import bpy  # noqa: PLC0415

    obj.data.materials.clear()
    mat = bpy.data.materials.new("m_iron_sword")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = STEEL_BASE_COLOR
        bsdf.inputs["Metallic"].default_value = STEEL_METALLIC
        bsdf.inputs["Roughness"].default_value = STEEL_ROUGHNESS
    obj.data.materials.append(mat)


def _export(obj, out_path: str):  # pragma: no cover - needs bpy
    """Export the single static weapon mesh (no armature/skin/animations)."""
    import bpy  # noqa: PLC0415

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(
        filepath=out_path,
        export_format="GLB",
        use_selection=True,
        export_yup=True,
        export_apply=True,
        export_skins=False,
        export_animations=False,
    )
    print(f"[restyle_weapon] wrote {out_path} ({_tri_count(obj)} tris)")


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - needs bpy
    import bpy  # noqa: PLC0415

    args = parse_args(
        generate_rig.argv_after_ddash(argv if argv is not None else sys.argv)
    )
    generate_rig.enforce_blender_pin(args.allow_unpinned_blender, tag="restyle_weapon")

    context = bpy.context
    context.scene.unit_settings.system = "METRIC"
    context.scene.unit_settings.scale_length = 1.0

    generate_rig.reset_scene()
    src = _import_and_join(args.in_glb)

    lo, hi = _obj_bbox(src)
    extent = (hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2])
    axes = sorted_axes_by_extent(extent)
    blade_axis = axes[0]
    centroid = _obj_centroid(src)
    center = ((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2)
    grip_pos = grip_on_positive_side(centroid, center, blade_axis)
    perm = signed_permutation(axes, grip_pos)
    _apply_orient(src, perm)

    # Post-orient the blade runs along UP_AXIS; scale by its (now up-axis) extent.
    lo2, hi2 = _obj_bbox(src)
    blade_extent = hi2[UP_AXIS] - lo2[UP_AXIS]
    scale = uniform_scale(blade_extent, args.target_len)
    seat_off = pommel_offset(lo2[UP_AXIS] * scale)
    _apply_scale_seat(src, scale, seat_off)

    _decimate_to(src, min(TARGET_TRIS, MAX_TRIS))
    src.name = args.mesh_name
    src.data.name = args.mesh_name

    _apply_steel_material(src)
    _export(src, args.out)


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
