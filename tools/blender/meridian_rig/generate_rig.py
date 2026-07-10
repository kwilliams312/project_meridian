"""Deterministic reference-rig generator (spec ④ §2).

Builds the Meridian humanoid armature from the canonical :mod:`bones` table and
exports it as a single skeleton-only glTF binary (``.glb``). The committed
artifact is regenerated with::

    /path/to/blender --background --python tools/blender/meridian_rig/generate_rig.py \
        -- --profile ardent_male --out content/core/assets/art/char/sk_ardent_male_skeleton.glb

Everything that does not need Blender (argument parsing, path shaping) lives in
importable module-level functions so the test suite covers it with ``bpy``
absent. The armature build + glTF export only run inside :func:`main`, which is
reached solely under Blender.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

# The bones table is pure Python; keep it importable whether or not bpy exists.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bones  # noqa: E402

# Repo-root-relative default output path for the committed reference rig.
DEFAULT_OUT = "content/core/assets/art/char/sk_ardent_male_skeleton.glb"


def argv_after_ddash(argv: list[str]) -> list[str]:
    """Return the CLI args following Blender's ``--`` separator (``[]`` if none)."""
    if "--" in argv:
        return argv[argv.index("--") + 1:]
    return []


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the generator's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="generate_rig.py",
        description="Generate the Meridian reference rig .glb from the bones table.",
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
        help="output .glb path (default: the committed reference-rig path)",
    )
    return parser.parse_args(argv)


def build_armature(profile: str):  # pragma: no cover - requires bpy/Blender
    """Build a fresh armature object holding every bone for ``profile``.

    Uses the full :data:`bones.ALL_BONES` table (56 profile bones + 7 sockets);
    ``profile`` is validated via :func:`bones.for_profile`. Edit-bone head/tail
    come straight from each ``BoneSpec``; parents are wired by name.
    """
    import bpy  # noqa: PLC0415 - Blender-only import

    bones.for_profile(profile)  # validates the profile name (raises on unknown)

    armature_data = bpy.data.armatures.new("ardent_male")
    armature_obj = bpy.data.objects.new("ardent_male", armature_data)
    bpy.context.collection.objects.link(armature_obj)

    bpy.context.view_layer.objects.active = armature_obj
    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = armature_data.edit_bones

    created = {}
    for spec in bones.ALL_BONES:
        eb = edit_bones.new(spec.name)
        eb.head = spec.head_m
        eb.tail = spec.tail_m
        created[spec.name] = eb
    for spec in bones.ALL_BONES:
        if spec.parent is not None:
            created[spec.name].parent = created[spec.parent]

    bpy.ops.object.mode_set(mode="OBJECT")
    return armature_obj


def export_glb(out_path: str) -> None:  # pragma: no cover - requires bpy/Blender
    """Export the current scene to ``out_path`` as a skeleton-only .glb.

    Y-up (glTF-native; the exporter handles Blender's Z-up conversion), no
    meshes, no animations. Bone node names/hierarchy survive the round-trip and
    are asserted by the structural tests.
    """
    import bpy  # noqa: PLC0415 - Blender-only import

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=out_path,
        export_format="GLB",
        export_yup=True,
        export_animations=False,
        export_skins=True,
        use_visible=False,
        use_selection=False,
    )


def reset_scene() -> None:  # pragma: no cover - requires bpy/Blender
    """Delete every object so the export contains only our armature."""
    import bpy  # noqa: PLC0415 - Blender-only import

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - requires bpy
    """Blender entry point: build the armature and write the .glb."""
    args = parse_args(argv_after_ddash(argv if argv is not None else sys.argv))
    reset_scene()
    build_armature(args.profile)
    export_glb(args.out)
    print(f"[generate_rig] wrote {args.out} for profile {args.profile!r}")


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
