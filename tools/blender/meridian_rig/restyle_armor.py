"""Restyle a Meshy armor sculpt into a canonical Meridian armor piece (spec ⑤/S4, #595).

Takes a raw Meshy-generated armor ``.glb`` (text-to-3D output, an unrigged single
mesh — e.g. a plate cuirass / breastplate) and turns it into a pipeline-compliant
*armor* asset, the SAME way ``restyle_body`` / ``restyle_hair`` source their meshes
from Meshy and fit them to the canonical rig:

* fitted to the canonical rig's target body REGION (for the chest: the ``torso``
  geoset span the greybox blockout + body geosets are built from — Spine → UpperChest
  and out to the shoulder girdle), inflated a hair OUTWARD so the plate sits just
  proud of the body surface (no void between plate and body) and extended UPWARD
  toward the neck/shoulders so a chest cuirass bridges the shoulder seam,
* budget-cleaned to the Art PRD §2.1 armor band (3–8k tris/slot),
* given a deterministic cylindrical UV unwrap (u = angle about the mesh's vertical
  axis, v = normalized height over the mesh's actual fitted span) replacing the
  arbitrary UVs Meshy emits. The per-slot RGB dye masks (``warden_<slot>_mask.png``,
  tools/art/generate_warden_kit.py) are horizontally uniform and banded purely by
  HEIGHT (a dominant primary body, a secondary trim band, an accent piping edge),
  so a v = normalized-height mapping tints the whole plate correctly — the chest
  dyes russet as intended (⑤/S6 dye finding) — irrespective of the u convention
  (the procedural plates used per-face planar box UVs; this is a cylindrical wrap,
  but both agree on v = height, which is all the height-banded mask depends on),
* single-influence skinned (weight 1.0 per vertex) to the canonical bones the piece
  rides — the chest binds Spine/Chest/UpperChest, the nearest owns each vertex — on
  the shared canonical armature, so the plate deforms with the torso,

then exported through the ``meridian_export`` E-rule gate (E100–E105) as an
``armor_model`` skinned piece (the class the pipeline uses for gear bound to a
SUBSET of canonical bones — I021/E100/E103/E104 handle it; the strict body rules
I020/I022 and the LOD-chain I023 apply only to ``character_model`` bodies, so a
plate ships a single mesh under the documented ``lod_policy: single`` exemption).

This is the deterministic *restyle* half of the workflow: the non-deterministic
half (the Meshy generation) is driven separately by ``tools/meshy`` and its exact
prompt + task id are recorded in the asset's provenance. Given the committed Meshy
sculpt, this pass is deterministic (fit math is a pure AABB transform, the skin is
nearest-bone single-influence, the UV is a pure projection).

Everything that does not need Blender (region-box math, per-axis fit transform,
cylindrical UV projection, bind-bone choice, naming, arg parsing) lives in
importable module-level functions so the test suite covers it with ``bpy`` absent.
The mesh build + skin + glTF export only run inside :func:`main`, reached solely
under Blender.

Region-generic by construction: :data:`REGION_BIND` maps every equip region to its
bind-bone set + fit tuning, so the V2 rollout (head, shoulders, hands, legs, feet)
runs the SAME pipeline with ``--region <slot>`` — the chest just proves it first.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bones  # noqa: E402
import generate_blockout  # noqa: E402
import generate_rig  # noqa: E402

Vec3 = tuple[float, float, float]

DEFAULT_OUT = "content/core/assets/art/item/armor/warden_chest.glb"

# The committed stylized body whose geoset surface each plate conforms to (⑤/S4).
DEFAULT_BODY_GLB = "content/core/assets/art/char/sk_ardent_male_base.glb"

# Exported mesh name. Deliberately NOT geo_<region>_lod<N>: E104 rejects geoset
# naming on an armor_model piece (gear binds bones, it is not a body geoset). The
# single-mesh, no-LOD-chain shape is declared via lod_policy: single (I023).
ARMOR_MESH_NAME = "armor"

# Target / hard-cap LOD0 triangle budget for one armor plate (Art PRD §2.1: armor
# set piece 3–8k). TARGET is the decimate goal; MAX is a hard cap the collapse
# never exceeds, keeping every slot inside the band under the 8k armor_model
# ceiling (validate_content L070). MIN documents the band floor for tests.
TARGET_TRIS = 6_000
MAX_TRIS = 7_600
MIN_TRIS = 3_000

# Authored plate base material — LIGHT brushed steel, identical to the procedural
# Warden's Kit plates (tools/art/generate_warden_kit.py BASE_COLOR). Meshy bakes a
# photographic albedo texture into its sculpt; the dye path MULTIPLIES the base by
# the mask (albedo = base × dye), so the plate MUST be a flat, light base for a
# russet dye to read as russet rather than muddy (⑤/S6 lead GPU finding). The
# restyle therefore STRIPS Meshy's material + embedded texture and reassigns this
# flat PBR base, so every restyled plate dyes exactly like the procedural kit — and
# the multi-MB Meshy texture never rides into the shipped .glb. Low metallic so the
# dyed albedo drives the diffuse look (a high-metallic surface swallows the albedo).
DYE_BASE_COLOR = (0.74, 0.76, 0.80, 1.0)
DYE_BASE_METALLIC = 0.15
DYE_BASE_ROUGHNESS = 0.55


class ArmorRegion:
    """Fit + bind configuration for one equip region (single source for all slots).

    ``region`` is the canonical geoset region name (keys ``generate_blockout``'s
    region→bone-group table). ``bind`` is the canonical bone set the plate skins
    to (nearest owns each vertex, single influence). ``inflate`` pushes the fitted
    plate outward from the body surface (no void); ``up_extend`` raises the top of
    the plate toward the adjacent region so it bridges the seam (the chest cuirass
    reaches up over the shoulder girdle). ``uv_axis`` is the plate's long/vertical
    axis in Blender Z-up space (2 = Z for the torso; the cylindrical unwrap wraps u
    about it).
    """

    def __init__(
        self,
        region: str,
        bind: tuple[str, ...],
        *,
        inflate: float = 0.06,
        up_extend: float = 0.06,
        uv_axis: int = 2,
        floor: bool = False,
    ) -> None:
        self.region = region
        self.bind = bind
        self.inflate = inflate
        self.up_extend = up_extend
        self.uv_axis = uv_axis
        # Ground-contact slots (feet) rest their sole ON the floor plane: the fit's
        # inflate would otherwise push the bottom BELOW the ground (region_lo −
        # inflate < 0), sinking the sabaton through the floor so it reads as a boot
        # floating below the legs (the #599 GPU-render finding). ``floor`` lifts the
        # fitted piece so its lowest point sits at Z=0 — a rigid shift, shape intact.
        self.floor = floor


# The proven chest region (this story) + the V2 slots pre-wired to the SAME
# pipeline. Bind sets are canonical bones (bones.py); the chest rides the spine
# column (Spine/Chest/UpperChest) exactly as the procedural plate did (#569/#589),
# so equipping it does not fly apart with arm motion, while its geometry reaches up
# over the shoulder girdle to bridge the neck/shoulder seam.
REGION_BIND: dict[str, ArmorRegion] = {
    "chest": ArmorRegion(
        "torso", ("Spine", "Chest", "UpperChest"), inflate=0.06, up_extend=0.07
    ),
    "head": ArmorRegion("head", ("Head",), inflate=0.02, up_extend=0.0),
    "shoulders": ArmorRegion(
        "torso", ("LeftShoulder", "RightShoulder"), inflate=0.03, up_extend=0.02
    ),
    "hands": ArmorRegion(
        "hands", ("LeftHand", "RightHand"), inflate=0.02, up_extend=0.0, uv_axis=0
    ),
    "legs": ArmorRegion(
        "hips_legs", ("LeftUpperLeg", "RightUpperLeg"), inflate=0.03, up_extend=0.0
    ),
    "feet": ArmorRegion(
        "feet", ("LeftFoot", "RightFoot"), inflate=0.03, up_extend=0.0, floor=True
    ),
}


def region_box(region: str) -> tuple[Vec3, Vec3]:
    """(lo, hi) AABB of a canonical geoset region in Blender Z-up space.

    Single-sourced from the same region→bone-group table + radius the greybox
    blockout, ``restyle_body`` and ``generate_warden_kit`` use, so the plate fits
    the exact body span the region's ``geo_<region>`` geoset occupies. Y-up bone
    points are mapped to the Blender Z-up space the imported sculpt lives in.
    """
    groups = generate_blockout.region_bone_groups()[region]
    radius = generate_blockout.REGION_RADIUS.get(
        region, generate_blockout._DEFAULT_RADIUS
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


def _read_glb_mesh_aabb(glb_path: str, mesh_name: str) -> tuple[Vec3, Vec3] | None:
    """(min, max) of a named glTF-binary mesh's POSITION, in glTF Y-up space.

    A minimal stdlib glTF-binary reader (no pygltflib — Blender's bundled Python
    lacks it), mirroring ``generate_warden_kit._read_body_positions``. Returns
    ``None`` if the mesh (or file) is absent, so the caller can fall back to the
    nominal bone-group box.
    """
    import json  # noqa: PLC0415
    import struct  # noqa: PLC0415

    try:
        glb = Path(glb_path).read_bytes()
    except OSError:
        return None
    if glb[:4] != b"glTF":
        return None
    _magic, _ver, total = struct.unpack_from("<4sII", glb, 0)
    off, doc, bin_off = 12, None, None
    while off < total:
        clen, ctype = struct.unpack_from("<II", glb, off)
        off += 8
        if ctype == 0x4E4F534A:  # 'JSON'
            doc = json.loads(glb[off : off + clen])
        elif ctype == 0x004E4942:  # 'BIN\0'
            bin_off = off
        off += clen + ((4 - clen % 4) % 4)
    if doc is None or bin_off is None:
        return None
    mesh = next((m for m in doc.get("meshes", []) if m.get("name") == mesh_name), None)
    if mesh is None:
        return None
    acc = doc["accessors"][mesh["primitives"][0]["attributes"]["POSITION"]]
    if acc.get("min") and acc.get("max"):  # glTF requires min/max on POSITION
        return (tuple(acc["min"]), tuple(acc["max"]))
    bv = doc["bufferViews"][acc["bufferView"]]
    base = bin_off + bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride") or 12
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for i in range(acc["count"]):
        p = struct.unpack_from("<3f", glb, base + i * stride)
        for k in range(3):
            lo[k] = min(lo[k], p[k])
            hi[k] = max(hi[k], p[k])
    return ((lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2]))


def body_region_box(body_glb: str, region: str) -> tuple[Vec3, Vec3] | None:
    """(lo, hi) AABB of the real BODY surface for a region, in Blender Z-up space.

    Reads the committed stylized body's ``geo_<region>_lod0`` geoset(s) — the true
    surface the plate must cover — instead of the nominal bone-group box, because
    the Meshy body bulges beyond the nominal box (e.g. the torso is wider in X than
    the shoulder-bone span). Fitting to THIS makes the plate sit just outside the
    real body with no void. glTF Y-up AABB is mapped to Blender Z-up via
    :func:`generate_rig.yup_to_blender`. Returns ``None`` if the body geosets are
    unavailable (unsmudged LFS pointer / missing) — the caller falls back to
    :func:`region_box`.
    """
    mesh_names = [f"geo_{region}_lod0"]
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    found = False
    for name in mesh_names:
        aabb = _read_glb_mesh_aabb(body_glb, name)
        if aabb is None:
            continue
        found = True
        g_lo, g_hi = aabb
        # Map both AABB corners through the Y-up→Z-up transform; axis-2 (Y→Z) and
        # axis-1 (Z→-Y) reorder, so union the mapped corners rather than assume order.
        for corner in (g_lo, g_hi):
            b = generate_rig.yup_to_blender(corner)
            for i in range(3):
                lo[i] = min(lo[i], b[i])
                hi[i] = max(hi[i], b[i])
        # yup_to_blender flips Z→-Y, so the two mapped corners bracket the Y range
        # only if we also feed the mixed corners; do it explicitly for safety.
        for gx in (g_lo[0], g_hi[0]):
            for gy in (g_lo[1], g_hi[1]):
                for gz in (g_lo[2], g_hi[2]):
                    b = generate_rig.yup_to_blender((gx, gy, gz))
                    for i in range(3):
                        lo[i] = min(lo[i], b[i])
                        hi[i] = max(hi[i], b[i])
    if not found:
        return None
    return (lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2])


def fit_region_transform(
    mesh_min: Vec3,
    mesh_max: Vec3,
    region_lo: Vec3,
    region_hi: Vec3,
    *,
    inflate: float = 0.06,
    up_extend: float = 0.06,
    up_axis: int = 2,
    floor_z: float | None = None,
) -> tuple[Vec3, Vec3]:
    """Per-axis scale + translation seating an armor AABB onto a body region.

    Maps the sculpt's AABB onto the region's AABB, expanded outward by ``inflate``
    on every axis (so the plate sits just proud of the body — no void) and extended
    by ``up_extend`` on the top of ``up_axis`` (so the plate reaches up toward the
    adjacent region and bridges the seam). Returns ``(scale, offset)`` — each a
    per-axis Vec3 — applied as ``p'[i] = p[i] * scale[i] + offset[i]`` per source
    vertex, so the fitted plate provably spans the (inflated) region: full coverage
    and no gap by construction, independent of the raw sculpt's proportions.

    ``floor_z`` (ground-contact slots only, e.g. feet) clamps the fitted piece so
    its lowest point on ``up_axis`` rests AT the floor plane instead of below it:
    inflate pushes the bottom to ``region_lo − inflate`` which, for a slot already
    on the ground, dips through the floor and the piece reads as floating below the
    body (#599). The clamp is a RIGID lift (offset only, scale untouched) applied
    solely when the fitted bottom would fall below ``floor_z`` — it never lowers a
    piece — so the sabaton's shape is preserved and its sole sits on the ground.
    """
    scale = [1.0, 1.0, 1.0]
    offset = [0.0, 0.0, 0.0]
    for i in range(3):
        tgt_lo = region_lo[i] - inflate
        tgt_hi = region_hi[i] + inflate
        if i == up_axis:
            tgt_hi += up_extend
        mesh_extent = max(mesh_max[i] - mesh_min[i], 1e-6)
        s = (tgt_hi - tgt_lo) / mesh_extent
        scale[i] = s
        offset[i] = tgt_lo - mesh_min[i] * s
    if floor_z is not None:
        fitted_bottom = mesh_min[up_axis] * scale[up_axis] + offset[up_axis]
        if fitted_bottom < floor_z:
            offset[up_axis] += floor_z - fitted_bottom
    return (scale[0], scale[1], scale[2]), (offset[0], offset[1], offset[2])


def cylindrical_uv(point: Vec3, region_lo: Vec3, region_hi: Vec3, up_axis: int = 2):
    """Deterministic cylindrical UV for a fitted armor vertex.

    ``u`` = angle about the vertical (``up_axis``) axis through the bounds'
    cross-section centre, normalized to [0, 1); ``v`` = normalized height along
    ``up_axis``. ``region_lo``/``region_hi`` are the bounds v is normalized against;
    callers pass the mesh's ACTUAL fitted AABB (not the pre-fit region box) so the
    whole mesh — including the inflated margin and shoulder-bridge geometry — maps
    across the mask's full 0..1 range rather than clamping at the extremes. The [0,
    1] clamp here is only a defensive guard against float error at the extremes.

    The dye masks (generate_warden_kit.py) are horizontally uniform and banded by
    HEIGHT (primary body / secondary trim band / accent piping), so this v = height
    mapping tints the whole piece correctly; the u convention is immaterial to the
    height-banded mask. Pure: computed from geometry, no Blender.
    """
    cross = tuple(i for i in range(3) if i != up_axis)
    cu = (region_lo[cross[0]] + region_hi[cross[0]]) / 2.0
    cw = (region_lo[cross[1]] + region_hi[cross[1]]) / 2.0
    du = point[cross[0]] - cu
    dw = point[cross[1]] - cw
    u = (math.atan2(dw, du) % (2.0 * math.pi)) / (2.0 * math.pi)
    span = max(region_hi[up_axis] - region_lo[up_axis], 1e-6)
    v = (point[up_axis] - region_lo[up_axis]) / span
    return (round(u, 6), round(min(max(v, 0.0), 1.0), 6))


def nearest_bind_bone(point: Vec3, bind: tuple[str, ...]) -> str:
    """Canonical bind bone whose rest midpoint is nearest ``point`` (its owner).

    Single-influence skinning (weight 1.0) to this bone — mirrors
    ``generate_warden_kit._nearest_bone``. All bind names are canonical (bones.py),
    so the skin binds only canonical joints (I021/E100). ``point`` is in Blender
    Z-up space; the Y-up bone rest midpoints are mapped to match before comparing.
    """
    table = {b.name: b for b in bones.ALL_BONES}

    def _mid(name: str) -> Vec3:
        h, t = table[name].head_m, table[name].tail_m
        return ((h[0] + t[0]) / 2.0, (h[1] + t[1]) / 2.0, (h[2] + t[2]) / 2.0)

    def _d2(name: str) -> float:
        m = generate_rig.yup_to_blender(_mid(name))
        return sum((point[i] - m[i]) ** 2 for i in range(3))

    return min(bind, key=_d2)


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the restyle script's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="restyle_armor.py",
        description="Restyle a raw Meshy armor .glb into a canonical armor plate.",
    )
    parser.add_argument("--in", dest="in_glb", required=True, help="raw Meshy .glb")
    parser.add_argument(
        "--region",
        default="chest",
        choices=sorted(REGION_BIND),
        help="equip region the plate covers (default: chest)",
    )
    parser.add_argument(
        "--out",
        default=DEFAULT_OUT,
        help="output plate .glb (default: warden_chest path)",
    )
    parser.add_argument(
        "--body-glb",
        default=DEFAULT_BODY_GLB,
        help=(
            "committed body .glb whose geo_<region>_lod0 surface the plate conforms "
            "to (fit sits just outside the real body — no void). Falls back to the "
            "nominal bone-group box if the geoset is unavailable."
        ),
    )
    parser.add_argument(
        "--rigdata-json",
        default=None,
        help=(
            "write the E-rule RigData snapshot (armor_model) here as JSON so the "
            "meridian_export E100–E105 gate can run from system Python. Defaults "
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
        raise SystemExit("restyle_armor: imported .glb carried no mesh")
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
    obj.name = "_armor_src"
    obj.data.name = "_armor_src"
    return obj


def _obj_bbox(obj) -> tuple[Vec3, Vec3]:  # pragma: no cover - needs bpy
    """World-space AABB of a mesh object (transforms already applied)."""
    xs = [v.co.x for v in obj.data.vertices]
    ys = [v.co.y for v in obj.data.vertices]
    zs = [v.co.z for v in obj.data.vertices]
    return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def _apply_fit(obj, scale: Vec3, offset: Vec3):  # pragma: no cover - needs bpy
    """Bake a per-axis scale + translation into the mesh vertices."""
    import bpy  # noqa: PLC0415

    obj.scale = scale
    obj.location = offset
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


# Weld distance (metres, post-fit body scale) for the heal pass. Meshy sculpts
# export with split vertices along every UV/normal seam, so ~⅔ of a plate's edges
# read as false boundaries; welding sub-⅓-mm-coincident verts fuses those seams so
# only GENUINE holes remain for the fill to close. Small enough never to collapse
# real detail on a ~0.5 m plate.
HEAL_MERGE_DIST = 0.0003


def _heal_mesh(
    obj, merge_dist: float = HEAL_MERGE_DIST
):  # pragma: no cover - needs bpy
    """Make a raw Meshy sculpt watertight: weld split verts, fill holes, fix normals.

    Meshy text-to-3D sculpts are not clean closed surfaces — they carry split
    vertices along seams and genuine open boundary loops (the #599 chest cuirass
    showed a topology hole punched through its centre). This deterministic bmesh
    pass runs BEFORE the decimate so the collapse operates on a repaired surface:

    1. ``remove_doubles`` welds coincident vertices, fusing the export-split seams
       that otherwise masquerade as boundary edges,
    2. ``holes_fill`` caps every remaining open boundary loop (``sides=0`` = all
       sizes) — an equipped, body-conforming plate is a closed shell from the
       outside, so capping the interior artifact hole removes the visible defect
       without altering the plate's outward silhouette,
    3. ``recalc_face_normals`` orients the (now-closed) surface consistently so the
       new cap faces shade outward.

    Deterministic: a fixed weld distance + full hole fill on the committed sculpt.
    """
    import bmesh  # noqa: PLC0415

    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=merge_dist)
    bmesh.ops.holes_fill(bm, edges=bm.edges, sides=0)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    me.update()


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


def _apply_cylindrical_uv(
    obj, region_lo, region_hi, up_axis
):  # pragma: no cover - needs bpy
    """Replace the mesh's UVs with the deterministic cylindrical unwrap.

    Meshy emits arbitrary UVs; the dye mask needs a predictable wrap. Build a fresh
    UV layer whose per-loop coordinate is :func:`cylindrical_uv` of the loop's
    vertex. ``region_lo``/``region_hi`` MUST be the mesh's actual fitted bounds so
    the height-banded dye mask (primary body / secondary trim / accent piping) maps
    across the whole plate — margins and shoulder-bridge included — without the v
    clamping that flattens the banding.
    """
    me = obj.data
    # Drop Meshy's own UV layer(s) so only the deterministic dye wrap remains as
    # TEXCOORD_0 (the channel the dye mask samples).
    while me.uv_layers:
        me.uv_layers.remove(me.uv_layers[0])
    uv_layer = me.uv_layers.new(name="dye")
    me.uv_layers.active = uv_layer
    coords = {
        v.index: cylindrical_uv((v.co.x, v.co.y, v.co.z), region_lo, region_hi, up_axis)
        for v in me.vertices
    }
    for loop in me.loops:
        uv_layer.data[loop.index].uv = coords[loop.vertex_index]


def _apply_dye_base_material(obj, slot):  # pragma: no cover - needs bpy
    """Strip Meshy's textured material and assign the flat light-steel dye base.

    Removes every material slot (dropping Meshy's baked albedo image so it never
    exports) and assigns a single Principled-BSDF material matching the procedural
    kit (:data:`DYE_BASE_COLOR` / metallic / roughness), so the dye shader
    multiplies a light, flat base and the plate tints true (⑤/S6).
    """
    import bpy  # noqa: PLC0415

    obj.data.materials.clear()
    mat = bpy.data.materials.new(f"m_warden_{slot}")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = DYE_BASE_COLOR
        bsdf.inputs["Metallic"].default_value = DYE_BASE_METALLIC
        bsdf.inputs["Roughness"].default_value = DYE_BASE_ROUGHNESS
    obj.data.materials.append(mat)


def _single_influence_skin(mesh_obj, armature, bind):  # pragma: no cover - needs bpy
    """Bind each vertex to its nearest bind bone at weight 1.0 (single influence).

    All 63 canonical vertex groups are created so the exported skin carries the
    shared canonical joint set (uniform with the body); only the per-vertex nearest
    bind bone receives weight, so every vertex has exactly one influence (E103) and
    the skin binds only canonical bones (I021/E100).
    """
    groups = {
        name: mesh_obj.vertex_groups.new(name=name) for name in bones.bone_names()
    }
    for v in mesh_obj.data.vertices:
        owner = nearest_bind_bone((v.co.x, v.co.y, v.co.z), bind)
        groups[owner].add([v.index], 1.0, "REPLACE")

    mod = mesh_obj.modifiers.new("armature", "ARMATURE")
    mod.object = armature
    mesh_obj.parent = armature


def _rigdata_snapshot(
    context, meshes, armature
) -> dict:  # pragma: no cover - needs bpy
    """Plain-dict snapshot of the E-rule (E100–E105) inputs for an armor_model piece."""
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
    """Write the E-rule RigData snapshot, then export the skinned plate .glb."""
    import json  # noqa: PLC0415

    import bpy  # noqa: PLC0415

    snapshot = _rigdata_snapshot(context, meshes, armature)
    Path(rigdata_json).parent.mkdir(parents=True, exist_ok=True)
    Path(rigdata_json).write_text(json.dumps(snapshot, indent=2), encoding="utf-8")
    print(f"[restyle_armor] wrote RigData snapshot {rigdata_json}")

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
    print(f"[restyle_armor] wrote {out_path} ({len(meshes)} mesh)")


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - needs bpy
    import bpy  # noqa: PLC0415

    args = parse_args(
        generate_rig.argv_after_ddash(argv if argv is not None else sys.argv)
    )
    generate_rig.enforce_blender_pin(args.allow_unpinned_blender, tag="restyle_armor")

    cfg = REGION_BIND[args.region]

    context = bpy.context
    context.scene.unit_settings.system = "METRIC"
    context.scene.unit_settings.scale_length = 1.0

    generate_rig.reset_scene()
    src = _import_and_join(args.in_glb)

    # Prefer the REAL body surface (geo_<region>_lod0) so the plate sits just
    # outside the body with no void; fall back to the nominal bone-group box if the
    # body geoset is unavailable (e.g. an unsmudged LFS checkout).
    region_lo, region_hi = body_region_box(args.body_glb, cfg.region) or region_box(
        cfg.region
    )
    mesh_min, mesh_max = _obj_bbox(src)
    scale, offset = fit_region_transform(
        mesh_min,
        mesh_max,
        region_lo,
        region_hi,
        inflate=cfg.inflate,
        up_extend=cfg.up_extend,
        up_axis=cfg.uv_axis,
        floor_z=0.0 if cfg.floor else None,
    )
    _apply_fit(src, scale, offset)

    # Repair the raw sculpt (weld export-split seams, cap open boundary holes,
    # fix normals) BEFORE decimating so the collapse works a watertight surface —
    # closes the #599 chest-centre topology hole deterministically.
    _heal_mesh(src)

    _decimate_to(src, min(TARGET_TRIS, MAX_TRIS))
    src.name = ARMOR_MESH_NAME
    src.data.name = ARMOR_MESH_NAME

    # Dye-mask UV MUST be normalized against the mesh's ACTUAL post-fit bounds — the
    # inflated + up-extended envelope it occupies — NOT the pre-fit region box. The
    # fit expands the target by inflate (all axes) and up_extend (top of the up
    # axis), so the outer margin and the shoulder-bridge geometry lie OUTSIDE the
    # nominal region; normalizing v against the nominal box sent >50% of verts
    # (concentrated in the bridge) outside [0,1] where they clamp, degenerating the
    # mask's height banding to flat colour over that geometry. Using the real bounds
    # maps the whole mesh — bridge included — across the mask's full 0..1 range.
    fit_min, fit_max = _obj_bbox(src)
    _apply_cylindrical_uv(src, fit_min, fit_max, cfg.uv_axis)

    # Replace Meshy's baked-texture material with the flat light-steel dye base so
    # the plate tints true and the multi-MB source texture never ships.
    _apply_dye_base_material(src, args.region)

    armature = generate_rig.build_armature(args.profile)
    _single_influence_skin(src, armature, cfg.bind)

    rigdata_json = args.rigdata_json or (args.out + ".rigdata.json")
    _gate_export(context, [src], armature, args.out, rigdata_json)


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
