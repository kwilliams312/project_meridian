"""Build the mini Meshy-style rigged .glb fixture (spec ④ §7.3 conversion gate).

Run under Blender::

    /path/to/blender --background --factory-startup -noaudio \
        --python tests/fixtures/meshy/build_fixture.py -- \
        --out tests/fixtures/meshy/meshy_rig_input.glb

Produces a tiny humanoid-arm rig using **Meshy-style bone names** (the stripped
Mixamo naming the seed `tools/meshy/bone_map.yaml` assumes) plus one twist helper
(`LeftForeArmTwist`) that is NOT in the map — so `convert_rig.py` exercises both
the rename path and the merge-into-nearest-mapped-ancestor path. Bones are placed
at the canonical rest positions (from `meridian_rig.bones`) so binding the
converted mesh to the canonical armature is geometrically correct without a
geometric re-pose (see convert_rig.py's re-pose limitation note).

This is a throwaway geometric fixture (small boxes per bone) — no franchise or
extracted assets (clean-room clean); it lives under tests/, not /content/, so it
carries no IF-8 sidecar.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent.parent.parent
sys.path.insert(0, str(_REPO / "tools" / "blender" / "meridian_rig"))

import bones as bones_mod  # noqa: E402
import generate_rig  # noqa: E402

# Meshy(name) → (head_m, tail_m, parent) at canonical rest (Y-up metres). Heads
# and tails are pulled from the canonical table so the fixture aligns to it.
_CANON = {b.name: b for b in bones_mod.ALL_BONES}


def _seg(meshy_name: str, canonical: str, parent: str | None):
    b = _CANON[canonical]
    return (meshy_name, b.head_m, b.tail_m, parent)


# A left-arm chain + spine stub in Meshy naming, plus a twist helper.
_RIG = [
    _seg("Hips", "Hips", None),
    _seg("Spine", "Spine", "Hips"),
    _seg("Spine1", "Chest", "Spine"),
    _seg("LeftArm", "LeftUpperArm", "Spine1"),
    _seg("LeftForeArm", "LeftLowerArm", "LeftArm"),
    _seg("LeftHand", "LeftHand", "LeftForeArm"),
]
# Twist helper: absent from the map, descends from LeftForeArm (name prefix) →
# convert_rig merges its weights into LeftLowerArm. Its box's vertices carry a
# MULTI-influence split (below) so the merge exercises numeric accumulation on
# vertices already weighted to the merge target's source bone — the two-pass /
# weight-conservation path from the PR #523 review (items 1+2).
_TWIST = (
    "LeftForeArmTwist",
    (-0.55, 1.42, 0.0),
    (-0.60, 1.42, 0.0),
    "LeftForeArm",
)
# vertex weights for the twist box: 0.6 helper + 0.4 parent → must emerge from
# conversion as exactly 1.0 on LeftLowerArm (asserted numerically in
# tests/test_meshy.py::test_converted_fixture_conserves_weight_mass).
_TWIST_SPLIT = {"LeftForeArmTwist": 0.6, "LeftForeArm": 0.4}


def _cube_verts(center, half=0.03):
    cx, cy, cz = center
    return [
        (cx + dx * half, cy + dy * half, cz + dz * half)
        for dx in (-1, 1)
        for dy in (-1, 1)
        for dz in (-1, 1)
    ]


_CUBE_FACES = [
    (0, 1, 3, 2),
    (4, 6, 7, 5),
    (0, 2, 6, 4),
    (1, 5, 7, 3),
    (0, 4, 5, 1),
    (2, 3, 7, 6),
]


def main(argv):
    import bpy  # noqa: PLC0415

    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    args = parser.parse_args(generate_rig.argv_after_ddash(argv))

    # Data-API purge (not ops select+delete): the factory startup scene can
    # hold hidden objects that ops-selection never reaches (see convert_rig.py).
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)

    segments = [(n, h, t, p) for (n, h, t, p) in _RIG] + [_TWIST]

    # --- Meshy armature (Meshy bone names) ---
    arm_data = bpy.data.armatures.new("meshy_rig")
    arm_obj = bpy.data.objects.new("meshy_rig", arm_data)
    bpy.context.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="EDIT")
    ebs = {}
    for name, head, tail, _parent in segments:
        eb = arm_data.edit_bones.new(name)
        eb.head = generate_rig.yup_to_blender(head)
        eb.tail = generate_rig.yup_to_blender(tail)
        ebs[name] = eb
    for name, _h, _t, parent in segments:
        if parent is not None:
            ebs[name].parent = ebs[parent]
    bpy.ops.object.mode_set(mode="OBJECT")

    # --- Skinned mesh: one small box per bone, assigned to that bone's group ---
    verts, faces, vert_bone = [], [], []
    for name, head, _t, _p in segments:
        base = len(verts)
        verts.extend(_cube_verts(generate_rig.yup_to_blender(head)))
        faces.extend(tuple(base + i for i in f) for f in _CUBE_FACES)
        vert_bone.extend([name] * 8)

    mesh = bpy.data.meshes.new("body")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    mesh_obj = bpy.data.objects.new("body", mesh)
    bpy.context.collection.objects.link(mesh_obj)

    for name, *_ in segments:
        mesh_obj.vertex_groups.new(name=name)
    for vi, bone in enumerate(vert_bone):
        if bone == "LeftForeArmTwist":
            for group_name, weight in _TWIST_SPLIT.items():
                mesh_obj.vertex_groups[group_name].add([vi], weight, "REPLACE")
        else:
            mesh_obj.vertex_groups[bone].add([vi], 1.0, "REPLACE")

    mesh_obj.parent = arm_obj
    mod = mesh_obj.modifiers.new(name="Armature", type="ARMATURE")
    mod.object = arm_obj

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(out),
        export_format="GLB",
        export_yup=True,
        export_animations=False,
        export_skins=True,
        use_selection=False,
        use_visible=False,
    )
    print(f"[build_fixture] wrote {out}")


if __name__ == "__main__":
    main(sys.argv)
