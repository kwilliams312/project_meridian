"""Offline clip retarget: external humanoid clip -> canonical Meridian rig.

Story #914 (W2a of the character-animation epic #906). This is the NET-NEW
offline half of the "Path B" pipeline: a Blender step that bakes a humanoid
animation authored on a DELIBERATELY NON-CANONICAL source armature onto the
canonical :mod:`bones` ``SkeletonProfileHumanoid`` rig, then exports it as a
runtime-loadable animation ``.glb``. The runtime half (load + re-key onto the
live chibi skeleton) lives in ``client/project/characters/locomotion_clips.gd``.

WHY A DEDICATED STEP (and why it does NOT touch the existing exporters).
``generate_rig.py`` and ``tools/meshy/convert_rig.py`` both hardcode
``export_animations=False`` on purpose -- they emit MESH / SKELETON assets, no
motion. This tool is the ONLY place that exports animation data
(``export_animations=True``), so flipping those exporters is never needed (and
would wrongly bloat the mesh/skeleton glbs with baked poses).

THE FIXTURE IS DELIBERATELY NON-CANONICAL (the whole point of W2a's test).
The source armature is built from the SAME canonical bone table but is made to
DIFFER from the target in the two ways a real external clip (e.g. Mixamo) does:

  1. **Bone names are ``mixamorig:``-prefixed** -- the retarget must remap them
     to the canonical ``SkeletonProfileHumanoid`` names, so a no-op path leaves
     ``mixamorig:`` names in the output and fails the verify's name assertion.
  2. **Rest pose is an A-pose** -- the arm chains hang ~40 deg below the
     canonical T-pose. The retarget reproduces the source's WORLD poses on the
     T-pose target (Blender back-solves each canonical bone's local rotation
     from the target rest), so a no-op path that merely copied the source's
     LOCAL rotations onto the T-pose rig would mis-place every arm and fail the
     verify's golden-pose assertion.

GOLDEN LANDMARK (per clip). Every clip poses ``LeftUpperArm`` STRAIGHT UP at
every frame -- an unambiguous, rest-sensitive landmark. On the canonical rig the
left upper arm rests horizontal (-X); reaching "up" (+Y) requires a ~90 deg
local rotation that is DIFFERENT from the source's A-pose-relative local
rotation. The verify checks the upper-arm world direction, which only lands
"up" when the A-pose -> T-pose rest was actually reconciled.

PROVENANCE. The fixture is authored ORIGINAL data -- generated procedurally from
the canonical bone table, no third-party clip, no Mixamo/Adobe asset or license.
``source_tier: original`` (CONTRIBUTING.md), stamped on the runtime library, no
IF-8 sidecar (same reasoning as the W1a placeholder: asset.schema.yaml has no
animation-clip class yet; that + the real Mixamo license are the human-gated
W3-prov story). Real Mixamo clips are Wave 3, NOT this story.

Regenerate the committed fixture with::

    /path/to/blender --background --python tools/blender/meridian_rig/retarget_clip.py \
        -- --profile chibi \
           --out client/project/characters/anim/meridian_locomotion_retarget.glb

Pure (bpy-free) helpers are module-level so the test suite covers them with
``bpy`` absent; the armature build + retarget + export only run under Blender in
:func:`main`.
"""
from __future__ import annotations

import argparse
import math
import sys
from collections.abc import Mapping
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import blender_pin  # noqa: E402
import bones  # noqa: E402

# Repo-root-relative default output: a staged, runtime-loaded animation glb next
# to the locomotion system that consumes it. The dir carries a .gdignore so
# Godot's editor importer never runs on it (the raw bytes are read at runtime via
# FileAccess + GLTFDocument, exactly like the chibi body glb -- no committed
# .import artifact, consistent with the whole staged-source content pipeline).
DEFAULT_OUT = "client/project/characters/anim/meridian_locomotion_retarget.glb"

# The source armature's non-canonical bone-name prefix (see module docstring #1).
SOURCE_PREFIX = "mixamorig:"

# The four locomotion clips, exported under these exact names so they arrive at
# runtime as the AnimationLibrary keys the W1a AnimationTree already references
# ("<LIBRARY_NAME>/<clip>"). Kept in lockstep with locomotion.gd CLIP_* consts.
CLIP_IDLE = "idle"
CLIP_WALK = "walk"
CLIP_RUN = "run"
CLIP_JUMP = "jump"
CLIP_NAMES = (CLIP_IDLE, CLIP_WALK, CLIP_RUN, CLIP_JUMP)

# The canonical bone the golden landmark poses (straight up) in every clip.
GOLDEN_BONE = "LeftUpperArm"

# Bones the fixture animates for locomotion read (legs swing, spine breathes) --
# a subset chosen to satisfy the existing W1a pose checks (walk swings
# LeftUpperLeg, idle moves the Spine) after retarget.
_LEG_L = "LeftUpperLeg"
_LEG_R = "RightUpperLeg"
_SHIN_L = "LeftLowerLeg"
_SHIN_R = "RightLowerLeg"
_SPINE = "Spine"


def argv_after_ddash(argv: list[str]) -> list[str]:
    """Return the CLI args following Blender's ``--`` separator (``[]`` if none)."""
    if "--" in argv:
        return argv[argv.index("--") + 1:]
    return []


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the tool's post-``--`` arguments."""
    parser = argparse.ArgumentParser(
        prog="retarget_clip.py",
        description="Retarget a non-canonical humanoid clip onto the Meridian rig.",
    )
    parser.add_argument(
        "--profile",
        default="chibi",
        choices=list(bones.VALID_PROFILES),
        help="target proportion profile to bake onto (default: chibi)",
    )
    parser.add_argument(
        "--out",
        default=DEFAULT_OUT,
        help="output animation .glb path (default: the staged locomotion fixture)",
    )
    parser.add_argument(
        "--allow-unpinned-blender",
        action="store_true",
        help=(
            "DEVELOPMENT ONLY: proceed even though the running Blender does not "
            "match blender_pin.PINNED_VERSION."
        ),
    )
    return parser.parse_args(argv)


def source_name(canonical: str) -> str:
    """The W2a fixture's source bone name for a canonical bone (adds the prefix).

    Only the authored ``mixamorig:`` fixture (:func:`build_source_armature`) is
    named by this rule; a real Meshy source is named by the reconciled
    :file:`tools/meshy/bone_map.yaml` table instead (see :func:`load_meshy_map`).
    """
    return SOURCE_PREFIX + canonical


def canonical_name(source: str, name_map: Mapping[str, str]) -> str | None:
    """Map a source bone name to its canonical name via ``name_map``.

    The retarget's generalized name-remap step (story #918). ``name_map`` is a
    source→canonical table — either the W2a authored fixture's ``mixamorig:`` map
    (:func:`fixture_source_map`) or a real Meshy rig's reconciled map
    (:func:`load_meshy_map`). Returns ``None`` for a source bone with no
    canonical target (an unmapped tip/helper, e.g. Meshy's ``head_end`` /
    ``headfront``) so the caller drops its track rather than keying a bone the
    canonical skeleton does not have. A no-op path that skipped this remap leaves
    the source names in the output track paths -- exactly what the W2a verify's
    name assertion catches.
    """
    return name_map.get(source)


def fixture_source_map(profile: str = "ardent_male") -> dict[str, str]:
    """The W2a authored fixture's source→canonical map: ``mixamorig:X`` → ``X``.

    The fixture is the ``mixamorig:`` map CASE of the generalized retarget: every
    canonical bone keyed by its prefixed source name. Bone NAMES are identical
    across profiles (only rest transforms differ), so the profile only affects
    which bones exist, never their names.
    """
    return {source_name(bone): bone for bone in bones.bone_names(profile)}


def load_meshy_map(version: str = "meshy-5") -> dict[str, str]:
    """The reconciled Meshy source→canonical map from ``tools/meshy/bone_map.yaml``.

    Lazily adds the repo ``tools/`` dir to ``sys.path`` and reads the table via
    the pure ``meshy.mapping`` loader (PyYAML-backed) — so the mixamorig fixture
    path keeps zero extra dependencies while a Meshy retarget gets the real map.
    """
    tools_dir = Path(__file__).resolve().parents[2]
    if str(tools_dir) not in sys.path:
        sys.path.insert(0, str(tools_dir))
    from meshy import mapping  # noqa: PLC0415 - lazy: only the Meshy path needs PyYAML

    return dict(mapping.load_map(version).bones)


def invert_map(name_map: Mapping[str, str]) -> dict[str, str]:
    """Invert a source→canonical map to canonical→source.

    The retarget drives from the target's animated CANONICAL bones, so it needs
    the reverse lookup to read each one's source pose bone. The forward map is
    injective (each source bone has one canonical target), so the inverse is
    well-defined over the mapped bones.
    """
    return {canonical: source for source, canonical in name_map.items()}


def arm_subtree(profile: str = "ardent_male") -> list[str]:
    """Canonical names of every bone at/under both UpperArms (the A-posed chains).

    These are the bones the source rig hangs into an A-pose so its rest differs
    from the canonical T-pose. Pure (table-only) so it is unit-testable.
    """
    hierarchy = bones.hierarchy(profile)
    roots = {"LeftUpperArm", "RightUpperArm"}
    out: list[str] = []
    for name in hierarchy:
        cur: str | None = name
        while cur is not None:
            if cur in roots:
                out.append(name)
                break
            cur = hierarchy.get(cur)
    return out


def yup_to_blender(p: tuple[float, float, float]) -> tuple[float, float, float]:
    """Map a table/glTF Y-up point to Blender's Z-up space: (x, y, z) -> (x, -z, y).

    Identical to generate_rig.py: the exporter's ``export_yup=True`` is the exact
    inverse, so table coordinates round-trip unchanged into the .glb.
    """
    x, y, z = p
    return (x, -z, y)


# --- Blender-only region (requires bpy) ---------------------------------------


def _rotate_point_about(  # pragma: no cover - trivial math wrapper, bpy-free logic
    point, pivot, axis, angle: float
):
    """Rotate ``point`` about ``pivot`` around ``axis`` by ``angle`` radians."""
    from mathutils import Matrix  # noqa: PLC0415

    rot = Matrix.Rotation(angle, 4, axis)
    return (rot @ (point - pivot)) + pivot


def build_source_armature(profile: str):  # pragma: no cover - requires bpy
    """Build the NON-CANONICAL source armature: mixamorig: names + A-pose arms.

    Same 63-bone table as the target, but every bone renamed with the
    ``mixamorig:`` prefix and both arm chains rotated ~40 deg down about their
    shoulder so the rest pose is an A-pose that DIFFERS from the canonical
    T-pose. This is what makes a no-op retarget fail the verify.
    """
    import bpy  # noqa: PLC0415
    from mathutils import Vector  # noqa: PLC0415

    specs = list(bones.for_profile(profile))
    arm_data = bpy.data.armatures.new("mixamo_source")
    arm_obj = bpy.data.objects.new("mixamo_source", arm_data)
    bpy.context.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="EDIT")
    ebs = arm_data.edit_bones

    created = {}
    for spec in specs:
        eb = ebs.new(source_name(spec.name))
        eb.head = yup_to_blender(spec.head_m)
        eb.tail = yup_to_blender(spec.tail_m)
        created[spec.name] = eb
    for spec in specs:
        if spec.parent is not None:
            created[spec.name].parent = created[spec.parent]

    # A-pose: rotate each arm subtree about its shoulder head. Left arm rotates
    # one way, right the mirror, so the arms drop into a plausible A. The exact
    # angle is irrelevant to correctness -- only that the rest clearly differs
    # from the T-pose so a no-op retarget is caught.
    subtree = arm_subtree(profile)
    for root_bone, sign in (("LeftUpperArm", +1.0), ("RightUpperArm", -1.0)):
        pivot = created[root_bone].head.copy()
        angle = sign * math.radians(40.0)
        for name in subtree:
            if not name.startswith(("Left", "Right")):
                continue
            # Only rotate the side this root owns.
            if root_bone.startswith("Left") and not name.startswith("Left"):
                continue
            if root_bone.startswith("Right") and not name.startswith("Right"):
                continue
            eb = created[name]
            eb.head = _rotate_point_about(eb.head, pivot, Vector((0, 1, 0)), angle)
            eb.tail = _rotate_point_about(eb.tail, pivot, Vector((0, 1, 0)), angle)

    bpy.ops.object.mode_set(mode="OBJECT")
    return arm_obj


def build_target_armature(profile: str):  # pragma: no cover - requires bpy
    """Build the canonical target armature (T-pose, SkeletonProfileHumanoid names).

    Byte-identical construction to generate_rig.py's armature: the retarget bakes
    onto THIS rest, so the exported clip's rest matches the chibi rig the game
    ships (and the runtime re-key rebases any residual rest delta anyway).
    """
    import bpy  # noqa: PLC0415

    specs = list(bones.for_profile(profile))
    arm_data = bpy.data.armatures.new(profile)
    arm_obj = bpy.data.objects.new(profile, arm_data)
    bpy.context.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="EDIT")
    ebs = arm_data.edit_bones
    created = {}
    for spec in specs:
        eb = ebs.new(spec.name)
        eb.head = yup_to_blender(spec.head_m)
        eb.tail = yup_to_blender(spec.tail_m)
        created[spec.name] = eb
    for spec in specs:
        if spec.parent is not None:
            created[spec.name].parent = created[spec.parent]
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm_obj


def _orient_bone_up(pose_bone):  # pragma: no cover - requires bpy
    """Pose ``pose_bone`` so its bone axis points world +Z (Blender up).

    export_yup then maps +Z -> glTF/Godot +Y, so the bone reads "straight up" in
    the game. Sets the pose bone's world matrix; Blender back-solves the local
    basis from whatever rest the bone has -- so this same call yields DIFFERENT
    local rotations on the A-pose source vs. the T-pose target, which is exactly
    the rest reconciliation the golden landmark tests.
    """
    from mathutils import Vector  # noqa: PLC0415

    rest_dir = (pose_bone.bone.tail_local - pose_bone.bone.head_local).normalized()
    delta = rest_dir.rotation_difference(Vector((0.0, 0.0, 1.0)))
    rest_rot = pose_bone.bone.matrix_local.to_3x3()
    new_rot = (delta.to_matrix() @ rest_rot).to_4x4()
    new_rot.translation = pose_bone.matrix.translation
    pose_bone.matrix = new_rot


def _swing(pose_bone, axis, angle: float):  # pragma: no cover - requires bpy
    """Set a local rotation of ``angle`` rad about local ``axis`` (rest-relative)."""
    from mathutils import Quaternion  # noqa: PLC0415

    pose_bone.rotation_mode = "QUATERNION"
    pose_bone.rotation_quaternion = Quaternion(axis, angle)


def author_source_action(  # pragma: no cover - requires bpy
    source_obj, clip: str
):
    """Author one clip's motion on the SOURCE armature, return its Action.

    Legs swing fore/aft (walk/run) or the spine breathes (idle) or the legs tuck
    (jump) -- enough for the W1a pose checks to keep passing after retarget --
    while ``LeftUpperArm`` is held STRAIGHT UP at every frame (the golden
    landmark). All motion is authored relative to the SOURCE (A-pose) rest; the
    retarget reconciles it onto the T-pose target.
    """
    import bpy  # noqa: PLC0415
    from mathutils import Vector  # noqa: PLC0415

    source_obj.animation_data_create()
    # Distinct name + NO fake user: the source actions are scratch, consumed by
    # the retarget and removed before export so only the 4 canonical target
    # actions survive into the glb (otherwise the exporter emits both, doubling
    # every clip as idle/idle_001, ...).
    action = bpy.data.actions.new("src_" + clip)
    source_obj.animation_data.action = action
    scene = bpy.context.scene
    fps = scene.render.fps

    # Per-clip shape: (length_seconds, leg_amp, spine_amp, jump_tuck).
    shape = {
        CLIP_IDLE: (2.0, 0.0, 0.06, 0.0),
        CLIP_WALK: (1.0, 0.40, 0.0, 0.0),
        CLIP_RUN: (0.6, 0.75, 0.0, 0.0),
        CLIP_JUMP: (0.6, 0.0, 0.0, 0.55),
    }[clip]
    length, leg_amp, spine_amp, jump_tuck = shape
    n = max(2, int(round(length * fps)))

    golden = source_obj.pose.bones[source_name(GOLDEN_BONE)]
    leg_l = source_obj.pose.bones[source_name(_LEG_L)]
    leg_r = source_obj.pose.bones[source_name(_LEG_R)]
    shin_l = source_obj.pose.bones[source_name(_SHIN_L)]
    shin_r = source_obj.pose.bones[source_name(_SHIN_R)]
    spine = source_obj.pose.bones[source_name(_SPINE)]
    for pb in (golden, leg_l, leg_r, shin_l, shin_r, spine):
        pb.rotation_mode = "QUATERNION"

    for i in range(n + 1):
        frame = i + 1
        scene.frame_set(frame)
        phase = float(i) / float(n)

        # Golden landmark: LeftUpperArm straight up, every frame.
        bpy.context.view_layer.update()
        _orient_bone_up(golden)

        # Locomotion motion (rest-relative local swings).
        if leg_amp > 0.0:
            _swing(leg_l, Vector((1, 0, 0)), math.sin(phase * math.tau) * leg_amp)
            _swing(leg_r, Vector((1, 0, 0)), math.sin((phase + 0.5) * math.tau) * leg_amp)
            _swing(shin_l, Vector((1, 0, 0)), math.sin((phase + 0.25) * math.tau) * leg_amp * 0.5)
            _swing(shin_r, Vector((1, 0, 0)), math.sin((phase + 0.75) * math.tau) * leg_amp * 0.5)
        if spine_amp > 0.0:
            _swing(spine, Vector((1, 0, 0)), math.sin(phase * math.tau) * spine_amp)
        if jump_tuck > 0.0:
            _swing(leg_l, Vector((1, 0, 0)), jump_tuck)
            _swing(leg_r, Vector((1, 0, 0)), jump_tuck)

        for pb in (golden, leg_l, leg_r, shin_l, shin_r, spine):
            pb.keyframe_insert(data_path="rotation_quaternion", frame=frame)

    return action


def retarget_action(  # pragma: no cover - requires bpy
    source_obj, target_obj, clip: str, source_map: Mapping[str, str]
):
    """Bake the source's WORLD poses for ``clip`` onto the canonical target.

    For every frame, read each animated source bone's WORLD matrix and assign it
    to the corresponding canonical target bone. The source→canonical
    ``source_map`` (mixamorig fixture OR reconciled Meshy table) is inverted so
    each canonical animated bone finds its source pose bone -- the SAME map drives
    both source shapes. Blender back-solves the target's local rotation from the
    T-pose rest -- reproducing identical world motion on the differently-rested
    rig. Bones are processed parent-first so a child's world assignment sees its
    parent already posed. Returns the target Action.
    """
    import bpy  # noqa: PLC0415

    target_obj.animation_data_create()
    action = bpy.data.actions.new(clip)
    action.use_fake_user = True
    target_obj.animation_data.action = action
    scene = bpy.context.scene

    # Only the bones the source actually animates get baked (rotation-only, no
    # root translation -- the fixture is in-place, like the W1a placeholder).
    animated = [GOLDEN_BONE, _LEG_L, _LEG_R, _SHIN_L, _SHIN_R, _SPINE]
    # Parent-first order (by hierarchy depth) so world assignment is stable.
    hierarchy = bones.hierarchy()

    def depth(name: str) -> int:
        d, cur = 0, hierarchy.get(name)
        while cur is not None:
            d += 1
            cur = hierarchy.get(cur)
        return d

    animated.sort(key=depth)

    # canonical→source so each animated canonical bone reads its source pose bone.
    canon_to_source = invert_map(source_map)

    src_action = source_obj.animation_data.action
    frame_start = int(src_action.frame_range[0])
    frame_end = int(src_action.frame_range[1])

    for frame in range(frame_start, frame_end + 1):
        scene.frame_set(frame)
        bpy.context.view_layer.update()
        world = {
            name: source_obj.pose.bones[canon_to_source[name]].matrix.copy()
            for name in animated
        }
        for name in animated:
            tgt = target_obj.pose.bones[name]
            tgt.rotation_mode = "QUATERNION"
            tgt.matrix = world[name]
            bpy.context.view_layer.update()
            tgt.keyframe_insert(data_path="rotation_quaternion", frame=frame)

    return action


def export_glb(out_path: str):  # pragma: no cover - requires bpy
    """Export every baked target action as a single animation .glb (skeleton + clips).

    ``export_animation_mode='ACTIONS'`` emits one glTF animation per fake-user
    action (idle/walk/run/jump). Skeleton-only otherwise (no mesh). This is the
    ONE exporter in the repo that sets ``export_animations=True``.
    """
    import bpy  # noqa: PLC0415

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=out_path,
        export_format="GLB",
        export_yup=True,
        export_animations=True,
        export_animation_mode="ACTIONS",
        export_bake_animation=True,
        export_skins=True,
        use_visible=False,
        use_selection=False,
    )


def enforce_blender_pin(  # pragma: no cover - requires bpy
    allow_unpinned: bool, tag: str = "retarget_clip"
) -> None:
    """Hard-error unless the running Blender matches the pin (spec 4 section 9)."""
    import bpy  # noqa: PLC0415

    error = blender_pin.check_pin(bpy.app.version, blender_pin.PINNED_VERSION)
    if error is None:
        return
    if allow_unpinned:
        print(f"[{tag}] WARNING (--allow-unpinned-blender): {error}", file=sys.stderr)
        return
    print(f"[{tag}] refused: {error}", file=sys.stderr)
    raise SystemExit(2)


def reset_scene() -> None:  # pragma: no cover - requires bpy
    """Delete every object so the export contains only our target armature+clips."""
    import bpy  # noqa: PLC0415

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def main(argv: list[str] | None = None) -> None:  # pragma: no cover - requires bpy
    """Blender entry point: build both rigs, author + retarget the clips, export."""
    import bpy  # noqa: PLC0415

    args = parse_args(argv_after_ddash(argv if argv is not None else sys.argv))
    enforce_blender_pin(args.allow_unpinned_blender)
    reset_scene()

    source = build_source_armature(args.profile)
    target = build_target_armature(args.profile)

    # The W2a fixture is the mixamorig: map CASE of the generalized retarget; a
    # real Meshy source would pass load_meshy_map() here instead.
    source_map = fixture_source_map(args.profile)

    for clip in CLIP_NAMES:
        # Author on the source, then bake onto the target. author_source_action
        # assigns a fresh active action each clip; retarget reads that active
        # action, so the two armatures stay independent per clip.
        author_source_action(source, clip)
        retarget_action(source, target, clip, source_map)

    # Drop the source armature AND its scratch actions so ONLY the canonical
    # target + its 4 baked actions export (ACTIONS mode would otherwise emit
    # every fake-user/leftover action, doubling each clip).
    bpy.data.objects.remove(source, do_unlink=True)
    for act in list(bpy.data.actions):
        if act.name.startswith("src_"):
            bpy.data.actions.remove(act)
    export_glb(args.out)
    print(f"[retarget_clip] wrote {args.out} for profile {args.profile!r} "
          f"({len(CLIP_NAMES)} clips: {', '.join(CLIP_NAMES)})")


if __name__ == "__main__":  # pragma: no cover - Blender-only entry
    main()
