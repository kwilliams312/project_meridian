"""Headless-Blender Meshy→canonical rig conversion (spec ④ §7.3).

Run under Blender (normally spawned by ``python -m meshy convert-rig``)::

    /path/to/blender --background --factory-startup -noaudio \
        --python tools/meshy/convert_rig.py -- \
        --in raw_meshy.glb --out canonical.glb --plan-json plan.json

Separation of concerns: the **plan is resolved in system Python** (``mapping.py``
needs PyYAML, which Blender's bundled Python lacks) and handed to this script as
JSON — so everything here depends only on ``bpy`` + the stdlib + the pure
``generate_rig``/``bones`` modules. The JSON is ``{"renames": {...}, "merges":
{...}}`` (Meshy bone → canonical bone).

Pipeline (spec ④ §7.3):
  1. Import the Meshy rigged ``.glb`` (mesh + Meshy armature + vertex groups).
  2. **Rename** mapped vertex groups to their canonical bone names, then
     **merge** each helper/twist group's weights into its mapped ancestor's
     canonical group and delete the helper group.
  3. Delete the imported Meshy armature and **re-bind** the mesh to a fresh
     canonical armature built by ``generate_rig.build_armature`` — the same
     source (``bones.py``) as the reference rig, so the output can only ever
     bind canonical bones (the I021 objective gate).
  4. **Limit to ≤4 influences + normalize**, then re-export as a ``.glb``.

Only argument/plan parsing is pure/importable; everything touching ``bpy`` lives
in functions marked ``# pragma: no cover`` (CI never runs Blender — spec ④ §10).

Re-pose limitation (DONE_WITH_CONCERNS, story #506; tracked in issue #524): the
seed map is UNVERIFIED, so Meshy's true rest orientation is unknown. This
converter binds the mesh (at its imported rest) to the canonical armature's
rest; that is correct when the Meshy rest already matches the canonical T-pose
(as the committed fixture is authored). A geometric per-bone re-pose (rotation
deltas from the canonical rest) must be added and validated with the #524 map
verification — until then convert-rig refuses unverified maps unless
--allow-unverified-map is passed.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Pure sibling for `bones`/`generate_rig` (canonical armature). Mirrors
# generate_rig.py's own sys.path shaping; no PyYAML dependency is pulled in.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "blender" / "meridian_rig"))


def argv_after_ddash(argv: list[str]) -> list[str]:
    """Return the CLI args following Blender's ``--`` separator (``[]`` if none)."""
    if "--" in argv:
        return argv[argv.index("--") + 1 :]
    return []


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse convert_rig's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="convert_rig.py",
        description="Convert a Meshy auto-rig onto the canonical skeleton.",
    )
    parser.add_argument("--in", dest="input", required=True, help="input Meshy .glb")
    parser.add_argument("--out", required=True, help="output canonical-rig .glb")
    parser.add_argument(
        "--plan-json",
        required=True,
        help="JSON file: {'renames': {meshy: canonical}, 'merges': {helper: canonical}}",
    )
    parser.add_argument(
        "--allow-unpinned-blender",
        action="store_true",
        help=(
            "DEVELOPMENT ONLY: proceed even though the running Blender does "
            "not match blender_pin.PINNED_VERSION. Without it, an unpinned "
            "Blender is refused so the converted rig is never silently "
            "non-byte-identical to what the pinned build would produce."
        ),
    )
    return parser.parse_args(argv)


def load_plan(path: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Read the resolved conversion plan JSON → (renames, merges)."""
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    return dict(doc.get("renames") or {}), dict(doc.get("merges") or {})


def merge_vertex_group(
    mesh_obj, src_name: str, dst_name: str
) -> None:  # pragma: no cover
    """Fold ``src_name`` group's per-vertex weights into ``dst_name``, drop src.

    Creates ``dst_name`` if absent (a merge target whose canonical bone had no
    directly-mapped Meshy source). No-op when the mesh lacks ``src_name``.

    Two-pass by design (PR #523 review, items 1+2):

    - **Pass 1 is read-only.** ``dst.add()`` can grow the very ``vert.groups``
      collection being iterated (bpy iterator-invalidation hazard on vertices
      not yet in ``dst``), so all (vertex, merged-weight) pairs are collected
      before any mutation.
    - **The merged weight is computed numerically** (``src + dst``) and written
      once with ``"REPLACE"`` — never ``"ADD"``, whose in-Blender accumulation
      clamps at 1.0 and can shift the vertex's relative weight distribution
      before normalize runs. glTF imports arrive with per-vertex weight sums
      ≤ 1.0 (WEIGHTS_0 is normalized), so ``src + dst`` cannot exceed 1.0 for
      any glb input; if a non-glb source ever violates that, the clamp below
      warns loudly instead of silently distorting (tracked with #524).
    """
    vgs = mesh_obj.vertex_groups
    src = vgs.get(src_name)
    if src is None:
        return
    dst = vgs.get(dst_name) or vgs.new(name=dst_name)
    src_index, dst_index = src.index, dst.index

    merged: list[tuple[int, float]] = []
    for vert in mesh_obj.data.vertices:
        src_w = dst_w = 0.0
        for g in vert.groups:
            if g.group == src_index:
                src_w = g.weight
            elif g.group == dst_index:
                dst_w = g.weight
        if src_w > 0.0:
            merged.append((vert.index, src_w + dst_w))

    for vert_index, weight in merged:
        if weight > 1.0 + 1e-6:
            print(
                f"[convert_rig] WARNING: merged weight {weight:.4f} > 1.0 on "
                f"vertex {vert_index} ({src_name} + {dst_name}) — clamping; "
                f"input weights were not normalized",
                file=sys.stderr,
            )
        dst.add([vert_index], min(weight, 1.0), "REPLACE")
    vgs.remove(src)


def apply_plan_to_vertex_groups(  # pragma: no cover
    mesh_obj, renames: dict[str, str], merges: dict[str, str]
) -> None:
    """Rename mapped groups to canonical names, then merge helper groups in.

    Renames run first so every merge target (a canonical bone name) already
    exists as a group before helper weights are folded into it.
    """
    for meshy_name, canonical in renames.items():
        vg = mesh_obj.vertex_groups.get(meshy_name)
        if vg is not None:
            vg.name = canonical
    for helper_name, canonical_target in merges.items():
        merge_vertex_group(mesh_obj, helper_name, canonical_target)


def _scene_objects_by_type(bpy, kind: str):  # pragma: no cover
    return [o for o in bpy.context.scene.objects if o.type == kind]


def main(argv: list[str] | None = None) -> int:  # pragma: no cover - requires bpy
    import bpy  # noqa: PLC0415 - Blender-only import

    import generate_rig  # noqa: PLC0415 - Blender-only (builds canonical armature)

    args = parse_args(argv_after_ddash(argv if argv is not None else sys.argv))
    generate_rig.enforce_blender_pin(args.allow_unpinned_blender, tag="convert_rig")
    in_glb = Path(args.input)
    out_glb = Path(args.out)
    renames, merges = load_plan(Path(args.plan_json))

    # --- 1. Fresh scene, import the Meshy rig. Purge via the data API, not
    # ops.select_all+delete: the factory startup scene can hold HIDDEN objects
    # (Blender 5.0 ships a hidden Icosphere) that ops-selection never reaches —
    # they would then surface in the imported-mesh list and break conversion. ---
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.ops.import_scene.gltf(filepath=str(in_glb))

    meshes = _scene_objects_by_type(bpy, "MESH")
    if not meshes:
        print(f"[convert_rig] ERROR: no mesh in {in_glb}", file=sys.stderr)
        return 1
    if len(meshes) > 1:
        # Every mesh is converted (PR #523 review, item 3) — but multi-mesh
        # Meshy output is unexpected for a single rigged character, so say so.
        print(
            f"[convert_rig] note: {len(meshes)} meshes in {in_glb}; converting all",
            file=sys.stderr,
        )

    # --- 2. Rename + merge vertex groups onto canonical bone names (ALL meshes —
    # a second mesh silently keeping Meshy groups would defeat the I021 gate). ---
    for mesh_obj in meshes:
        apply_plan_to_vertex_groups(mesh_obj, renames, merges)

    # --- 3. Drop the Meshy armature; re-bind every mesh to one fresh canonical
    # armature. ---
    for arm in _scene_objects_by_type(bpy, "ARMATURE"):
        bpy.data.objects.remove(arm, do_unlink=True)
    canonical_arm = generate_rig.build_armature("ardent_male")
    for mesh_obj in meshes:
        for mod in list(mesh_obj.modifiers):
            if mod.type == "ARMATURE":
                mesh_obj.modifiers.remove(mod)
        mesh_obj.parent = canonical_arm
        arm_mod = mesh_obj.modifiers.new(name="Armature", type="ARMATURE")
        arm_mod.object = canonical_arm

    # --- 4. Limit ≤4 influences + normalize (per mesh), then export. ---
    for mesh_obj in meshes:
        if not mesh_obj.vertex_groups:
            # An unskinned mesh (no groups) cannot pass limit/normalize's poll;
            # it simply rides along unweighted rather than crashing the pass.
            print(
                f"[convert_rig] WARNING: mesh '{mesh_obj.name}' has no vertex "
                f"groups (unskinned) — bound to the armature without weights",
                file=sys.stderr,
            )
            continue
        bpy.ops.object.select_all(action="DESELECT")
        mesh_obj.select_set(True)
        bpy.context.view_layer.objects.active = mesh_obj
        bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.vertex_group_limit_total(limit=4)
        bpy.ops.object.vertex_group_normalize_all()

    out_glb.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(out_glb),
        export_format="GLB",
        export_yup=True,
        export_animations=False,
        export_skins=True,
        use_selection=False,
        use_visible=False,
    )
    print(
        f"[convert_rig] wrote {out_glb} ({len(renames)} renamed, {len(merges)} merged)"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    sys.exit(main())
