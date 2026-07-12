# SPDX-License-Identifier: Apache-2.0
"""Re-cut an already-restyled body .glb's geoset partition in place (issue #587).

The ⑤/S4 restyle (:mod:`restyle_body`) cuts the body into the eight
``geo_<region>_lod<N>`` geosets by a nearest-anchor Voronoi partition. Issue #587
fixed the anchor table (``generate_blockout.region_bone_groups`` gained a per-arm
upper-arm anchor) so the WHOLE arm (shoulder joint -> wrist) now belongs to
``forearms`` instead of leaving the upper arm inside ``torso`` — where a
torso-hiding chest plate erased it and orphaned the forearm.

Regenerating the committed body from its raw Meshy sculpt is impossible
deterministically (the sculpt is a non-deterministic text-to-3D output that is
not committed — Art SAD; ``ardent_male_base.asset.yaml`` origin_url). So instead
of re-running the full restyle, this tool re-applies ONLY the geoset cut to the
committed, restyled ``sk_ardent_male_base.glb``:

* load the committed body (its 8x4 geoset meshes + the 63-bone armature),
* per LOD level, join the region meshes back into that level's full surface and
  re-partition its faces with the CURRENT :func:`restyle_body.region_anchors`
  (the #587 anchor table) — reusing the exact partition helpers the restyle uses,
* re-export with the restyle's glTF settings.

It never moves a vertex or touches a skin weight: every vertex position and its
<=4-influence weight set is carried through the join/partition unchanged, so the
body SHAPE and skinning are byte-for-byte the same — only which geoset mesh each
face belongs to changes. The result is exactly what the restyle would have
produced for this surface under the #587 anchors, and re-running the tool on its
own output is idempotent (the faces are already in their nearest region).

Deterministic: byte-reproducible only under the pinned Blender
(:data:`blender_pin.PINNED_VERSION`), enforced at startup like every other bpy
entry point in this repo.

Regenerate the committed body with::

    /path/to/blender --background --factory-startup -noaudio \
        --python tools/blender/meridian_rig/recut_body_geoset.py --

(defaults re-cut the committed path in place; pass --in/--out to override.)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_blockout  # noqa: E402
import generate_rig  # noqa: E402
import restyle_body  # noqa: E402

DEFAULT_PATH = "content/core/assets/art/char/sk_ardent_male_base.glb"

# Geoset object names as they import: geo_<region>_lod<N> (glTF may append a
# ``.00N`` uniquifier if a name ever collides on import — tolerate it).
_GEO_OBJ_RE = re.compile(r"^geo_([a-z0-9_]+)_lod(\d+)(?:\.\d+)?$")


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the re-cut tool's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="recut_body_geoset.py",
        description="Re-cut a restyled body .glb's geoset partition in place (#587).",
    )
    parser.add_argument(
        "--in",
        dest="in_glb",
        default=DEFAULT_PATH,
        help="committed body .glb to re-cut",
    )
    parser.add_argument(
        "--out", default=None, help="output .glb (default: same as --in, in place)"
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
def _collect_geosets(scene):  # pragma: no cover - needs bpy
    """Return ``{lod: {region: mesh_obj}}`` for the imported geoset meshes."""
    by_lod: dict[int, dict[str, object]] = {}
    for obj in scene.objects:
        if obj.type != "MESH":
            continue
        m = _GEO_OBJ_RE.match(obj.name)
        if m is None:
            continue  # stray non-geoset object (e.g. a startup-file leftover)
        region, lod = m.group(1), int(m.group(2))
        by_lod.setdefault(lod, {})[region] = obj
    return by_lod


def _join_region_meshes(region_objs):  # pragma: no cover - needs bpy
    """Join one LOD level's region meshes into a single full-surface object."""
    import bpy  # noqa: PLC0415

    # Deterministic join order (by current name) so the reassembled surface — and
    # thus the re-export — is reproducible.
    region_objs = sorted(region_objs, key=lambda o: o.name)
    bpy.ops.object.select_all(action="DESELECT")
    for o in region_objs:
        o.select_set(True)
    active = region_objs[0]
    bpy.context.view_layer.objects.active = active
    if len(region_objs) > 1:
        bpy.ops.object.join()
    joined = bpy.context.view_layer.objects.active
    joined.name = "_recut_surface"
    joined.data.name = "_recut_surface"
    return joined


def _label_faces_by_region(
    joined, anchors, region_index
):  # pragma: no cover - needs bpy
    """Tag every face of ``joined`` with its new nearest-anchor region index."""
    import bmesh  # noqa: PLC0415

    me = joined.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.faces.ensure_lookup_table()
    layer = bm.faces.layers.int.new("region_idx")
    for f in bm.faces:
        c = f.calc_center_median()
        region = restyle_body.nearest_region((c.x, c.y, c.z), anchors)
        f[layer] = region_index[region]
    bm.to_mesh(me)
    bm.free()


def _split_into_region_meshes(
    joined, lod, regions, region_index, armature
):  # pragma: no cover
    """Split the labelled surface into one ``geo_<region>_lod<lod>`` mesh each."""
    import bpy  # noqa: PLC0415

    out = []
    for region in regions:
        idx = region_index[region]
        bpy.ops.object.select_all(action="DESELECT")
        joined.select_set(True)
        bpy.context.view_layer.objects.active = joined
        bpy.ops.object.duplicate()
        dup = bpy.context.view_layer.objects.active
        kept = restyle_body._keep_region_faces(dup, idx)
        if kept == 0:
            # Every region carries faces at every committed LOD; an empty region
            # after re-partition means the anchor table regressed — fail loud
            # rather than silently ship a body missing a geoset (I022).
            raise SystemExit(
                f"recut: region {region!r} captured no faces at lod{lod} — "
                f"the geoset partition would be incomplete"
            )
        name = restyle_body.mesh_name(region, lod)
        dup.name = name
        dup.data.name = name
        # Drop the temporary region_idx layer so it never reaches the export.
        if "region_idx" in dup.data.attributes:
            dup.data.attributes.remove(dup.data.attributes["region_idx"])
        _ensure_armature_bind(dup, armature)
        out.append(dup)
    bpy.data.objects.remove(joined, do_unlink=True)
    return out


def _ensure_armature_bind(mesh_obj, armature):  # pragma: no cover - needs bpy
    """Guarantee the mesh keeps its armature modifier + parent after duplication."""
    mesh_obj.parent = armature
    has_arm = any(mod.type == "ARMATURE" for mod in mesh_obj.modifiers)
    if not has_arm:
        mod = mesh_obj.modifiers.new("armature", "ARMATURE")
        mod.object = armature
    else:
        for mod in mesh_obj.modifiers:
            if mod.type == "ARMATURE":
                mod.object = armature


def _export(all_meshes, armature, out_path):  # pragma: no cover - needs bpy
    """Export the re-cut meshes + armature with the restyle's glTF settings."""
    import bpy  # noqa: PLC0415

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    for m in all_meshes:
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
    print(f"[recut] wrote {out_path} ({len(all_meshes)} geoset meshes)")


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - Blender-only
    import bpy  # noqa: PLC0415

    args = parse_args(
        generate_rig.argv_after_ddash(argv if argv is not None else sys.argv)
    )
    generate_rig.enforce_blender_pin(
        args.allow_unpinned_blender, tag="recut_body_geoset"
    )
    out_path = args.out or args.in_glb

    context = bpy.context
    context.scene.unit_settings.system = "METRIC"
    context.scene.unit_settings.scale_length = 1.0

    generate_rig.reset_scene()
    bpy.ops.import_scene.gltf(filepath=args.in_glb)

    # Free the canonical geo_<region>_lod<N> mesh-DATA names before we rebuild the
    # geosets: joining consumes the source objects but leaves their mesh datablocks
    # as orphans still holding those names, which would push the freshly-cut meshes
    # to ``geo_..._lodN.001`` on export and break I022's exact-name check.
    for obj in context.scene.objects:
        if obj.type == "MESH" and _GEO_OBJ_RE.match(obj.name):
            obj.data.name = "_src_" + obj.data.name

    armatures = [o for o in context.scene.objects if o.type == "ARMATURE"]
    if len(armatures) != 1:
        raise SystemExit(
            f"recut: expected exactly one armature, found {len(armatures)}"
        )
    armature = armatures[0]

    anchors = restyle_body.region_anchors()
    regions = list(generate_blockout.region_bone_groups().keys())
    region_index = {r: i for i, r in enumerate(regions)}

    by_lod = _collect_geosets(context.scene)
    if not by_lod:
        raise SystemExit("recut: imported .glb carried no geo_<region>_lod<N> meshes")

    all_meshes = []
    for lod in sorted(by_lod):
        region_objs = list(by_lod[lod].values())
        joined = _join_region_meshes(region_objs)
        _label_faces_by_region(joined, anchors, region_index)
        all_meshes.extend(
            _split_into_region_meshes(joined, lod, regions, region_index, armature)
        )

    _export(all_meshes, armature, out_path)


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
