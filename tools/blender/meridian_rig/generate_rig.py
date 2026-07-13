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

# The bones table + version-pin guard are pure Python; keep them importable
# whether or not bpy exists.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import blender_pin  # noqa: E402
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


def yup_to_blender(p: tuple[float, float, float]) -> tuple[float, float, float]:
    """Map a table/glTF Y-up point to Blender's Z-up space: (x, y, z) -> (x, -z, y).

    The bones table (and glTF) is Y-up; Blender edit-bone space is Z-up. The
    exporter's ``export_yup=True`` conversion is the exact inverse, so table
    coordinates round-trip unchanged into the exported .glb (asserted by
    ``test_rig_glb_rest_transforms_match_table``).
    """
    x, y, z = p
    return (x, -z, y)


def build_armature(profile: str):  # pragma: no cover - requires bpy/Blender
    """Build a fresh armature object holding every bone for ``profile``.

    All 63 bones (56 profile + 7 sockets) come from :func:`bones.for_profile`,
    which validates the profile name and returns that profile's rest table --
    the socket mounts ride the profile too, so a stockier profile's sockets sit
    on that profile's hands/chest/hips. Edit-bone head/tail are the table's Y-up
    coords converted to Blender Z-up; parents are wired by name.
    """
    import bpy  # noqa: PLC0415 - Blender-only import

    all_specs = list(bones.for_profile(profile))

    armature_data = bpy.data.armatures.new(profile)
    armature_obj = bpy.data.objects.new(profile, armature_data)
    bpy.context.collection.objects.link(armature_obj)

    bpy.context.view_layer.objects.active = armature_obj
    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = armature_data.edit_bones

    created = {}
    for spec in all_specs:
        eb = edit_bones.new(spec.name)
        eb.head = yup_to_blender(spec.head_m)
        eb.tail = yup_to_blender(spec.tail_m)
        created[spec.name] = eb
    for spec in all_specs:
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


def enforce_blender_pin(
    allow_unpinned: bool, tag: str = "generate_rig"
) -> None:  # pragma: no cover - requires bpy
    """Hard-error unless the running Blender matches ``blender_pin.PINNED_VERSION``.

    Called by every bpy entry point in this repo (this module, plus
    ``generate_blockout.py``, ``tools/meshy/convert_rig.py``, and
    ``tests/fixtures/meshy/build_fixture.py``, each of which imports this
    module) right after arg parsing — so an unpinned Blender never silently
    produces a non-byte-identical export (spec ④ §9). Only the ``import bpy``
    needed to read ``bpy.app.version`` distinguishes this from the pure,
    unit-tested :func:`blender_pin.check_pin` comparison it wraps.
    """
    import bpy  # noqa: PLC0415 - Blender-only import

    error = blender_pin.check_pin(bpy.app.version, blender_pin.PINNED_VERSION)
    if error is None:
        return
    if allow_unpinned:
        print(f"[{tag}] WARNING (--allow-unpinned-blender): {error}", file=sys.stderr)
        return
    print(f"[{tag}] refused: {error}", file=sys.stderr)
    raise SystemExit(2)


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - requires bpy
    """Blender entry point: build the armature and write the .glb."""
    args = parse_args(argv_after_ddash(argv if argv is not None else sys.argv))
    enforce_blender_pin(args.allow_unpinned_blender)
    reset_scene()
    build_armature(args.profile)
    export_glb(args.out)
    print(f"[generate_rig] wrote {args.out} for profile {args.profile!r}")


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
