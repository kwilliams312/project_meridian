"""meridian_export — the sanctioned Blender glTF + IF-8 sidecar export addon.

The only sanctioned export path (wraps the stock glTF exporter). For the selected
object(s) it exports a ``.glb`` to the pack-local asset path and writes a matching
``<name>.asset.yaml`` IF-8 sidecar carrying the provenance + budget fields the CI
validators enforce (Art SAD §2.1, §4; schema/content/asset.schema.yaml).

All non-``bpy`` logic lives in :mod:`sidecar` (pure Python, unit-tested without
Blender). This file is the thin ``bpy`` shell: UI, object introspection, and the
two file writes.
"""

from __future__ import annotations

bl_info = {
    "name": "meridian_export",
    "author": "Project Meridian art track",
    "version": (0, 1, 0),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > Meridian",
    "description": "Export selected mesh as glTF + IF-8 .asset.yaml sidecar (pipeline-compliant).",
    "category": "Import-Export",
}

# --- Import the pure modules whether loaded as a Blender addon (package) or via a
# direct path insertion. Blender registers the package, so relative import works;
# the try/except keeps the modules importable standalone for tooling.
try:  # pragma: no cover - exercised inside Blender
    from . import rig_checks as _rig_checks
    from . import sidecar as _sidecar
except ImportError:  # pragma: no cover - standalone fallback
    import rig_checks as _rig_checks
    import sidecar as _sidecar

# Skeletal export classes that require the E-rule rig/geoset conformance checks
# (spec ④ §3/§4) before an export is allowed to proceed.
_SKELETAL_CLASSES = ("character_model", "armor_model")

try:  # pragma: no cover - bpy only exists inside Blender
    import bpy
    from bpy.props import EnumProperty, StringProperty
    from bpy.types import Operator, Panel
except ImportError:  # pragma: no cover - allows `import meridian_export` w/o Blender
    bpy = None  # type: ignore[assignment]
    Operator = object  # type: ignore[assignment,misc]
    Panel = object  # type: ignore[assignment,misc]


# --- bpy introspection helpers (pure functions of a bpy object; not unit-tested
# without Blender, kept minimal — the testable logic is in sidecar.py). ---------


def _tri_count(obj) -> int:  # pragma: no cover - needs bpy
    """Triangulated face count of a mesh object (LOD0)."""
    mesh = obj.data
    return sum(max(len(p.vertices) - 2, 0) for p in mesh.polygons)


def _texture_max_px(obj) -> int | None:  # pragma: no cover - needs bpy
    """Largest image dimension across the object's material image textures."""
    dims: list[int] = []
    for slot in obj.material_slots:
        mat = slot.material
        if mat is None or not mat.use_nodes:
            continue
        for node in mat.node_tree.nodes:
            image = getattr(node, "image", None)
            if image is not None and image.size[0]:
                dims.append(max(int(image.size[0]), int(image.size[1])))
    return max(dims) if dims else None


def _has_negative_scale(obj) -> bool:  # pragma: no cover - needs bpy
    sx, sy, sz = obj.scale
    return (sx * sy * sz) < 0


def _mesh_info(obj) -> "_sidecar.MeshInfo":  # pragma: no cover - needs bpy
    """Extract a pure MeshInfo snapshot from a bpy object."""
    return _sidecar.MeshInfo(
        name=obj.name,
        tri_count=_tri_count(obj),
        texture_max_px=_texture_max_px(obj),
        material_set_count=len(obj.material_slots) or None,
        scale=tuple(obj.scale),
        transform_applied=all(abs(s - 1.0) < 1e-4 for s in obj.scale),
        has_negative_scale=_has_negative_scale(obj),
    )


# --- rig/geoset introspection helpers (pure functions of bpy objects; not unit-
# tested without Blender — the testable logic is in rig_checks.py). -------------


def _find_armature(obj):  # pragma: no cover - needs bpy
    """Find the armature driving `obj` via its Armature modifier, else None."""
    for mod in obj.modifiers:
        if mod.type == "ARMATURE" and mod.object is not None:
            return mod.object
    if obj.type == "ARMATURE":
        return obj
    return None


def _skin_influence_stats(
    mesh_objs,
) -> tuple[int, bool, dict[str, int], list[str]]:  # pragma: no cover - needs bpy
    """Max vertex-group influence count and normalization, overall AND per mesh.

    The per-mesh breakdown feeds E103's mesh-identified error message (#526,
    T3 review minor); the overall aggregate is kept for the additive
    (backward-compatible) RigData fields.
    """
    max_influences = 0
    normalized = True
    mesh_max_influences: dict[str, int] = {}
    unnormalized_meshes: list[str] = []
    for mesh_obj in mesh_objs:
        mesh_max = 0
        mesh_normalized = True
        for vertex in mesh_obj.data.vertices:
            groups = [g for g in vertex.groups if g.weight > 0.0]
            mesh_max = max(mesh_max, len(groups))
            if groups and abs(sum(g.weight for g in groups) - 1.0) > 1e-3:
                mesh_normalized = False
        mesh_max_influences[mesh_obj.name] = mesh_max
        if not mesh_normalized:
            unnormalized_meshes.append(mesh_obj.name)
        max_influences = max(max_influences, mesh_max)
        normalized = normalized and mesh_normalized
    return max_influences, normalized, mesh_max_influences, unnormalized_meshes


def _transforms_applied(obj) -> bool:  # pragma: no cover - needs bpy
    """No residual object-level transform: location/rotation/scale at identity.

    Feeds E105 (#526, spec ④ §4's dropped blocking promise). Mirrors the
    scale-only check `_mesh_info.transform_applied` already does for the
    non-blocking SCALE warning, extended to location + rotation as well —
    "apply all transforms" means all three, not just scale.
    """
    loc_ok = all(abs(v) < 1e-4 for v in obj.location)
    rot_ok = all(abs(v) < 1e-4 for v in obj.rotation_euler)
    scale_ok = all(abs(v - 1.0) < 1e-4 for v in obj.scale)
    return loc_ok and rot_ok and scale_ok


def _unit_scale_ok(context) -> bool:  # pragma: no cover - needs bpy
    """Scene unit settings resolve to 1 Blender unit = 1 m (E105, spec ④ §4)."""
    unit = context.scene.unit_settings
    return unit.system == "METRIC" and abs(unit.scale_length - 1.0) < 1e-4


def _build_rig_data(
    context, obj, asset_class: str
) -> "_rig_checks.RigData":  # pragma: no cover - needs bpy
    """Build a RigData snapshot for a skeletal export (character_model/armor_model)."""
    armature = _find_armature(obj)
    bone_names = list(armature.data.bones.keys()) if armature is not None else []
    socket_names = [n for n in bone_names if n.startswith("socket_")]
    mesh_objs = [o for o in context.selected_objects if o.type == "MESH"]
    max_influences, normalized, mesh_max_influences, unnormalized_meshes = (
        _skin_influence_stats(mesh_objs)
    )
    transform_objs = list(mesh_objs)
    if armature is not None:
        transform_objs.append(armature)
    object_transforms = [
        _rig_checks.ObjectTransformState(
            name=o.name, transforms_applied=_transforms_applied(o)
        )
        for o in transform_objs
    ]
    return _rig_checks.RigData(
        asset_class=asset_class,
        bone_names=bone_names,
        socket_names=socket_names,
        mesh_names=[o.name for o in mesh_objs],
        max_influences=max_influences,
        weights_normalized=normalized,
        mesh_max_influences=mesh_max_influences,
        unnormalized_meshes=unnormalized_meshes,
        object_transforms=object_transforms,
        unit_scale_ok=_unit_scale_ok(context),
    )


# --- Operator ------------------------------------------------------------------


class MERIDIAN_OT_export_asset(Operator):  # type: ignore[misc]
    """Export the active object as glTF + IF-8 sidecar."""

    bl_idname = "meridian.export_asset"
    bl_label = "Export Meridian Asset"
    bl_options = {"REGISTER"}

    if bpy is not None:  # pragma: no cover - property registration needs bpy
        asset_id: StringProperty(  # type: ignore[valid-type]
            name="Asset ID",
            description="Namespaced asset id, e.g. core:art.env.zone01.kit.wall_stone_a",
            default="",
        )
        asset_class: EnumProperty(  # type: ignore[valid-type]
            name="Class",
            items=[
                ("kit_piece", "Kit Piece", ""),
                ("prop", "Prop", ""),
                ("character_model", "Character", ""),
                ("weapon_model", "Weapon", ""),
                ("armor_model", "Armor", ""),
                ("creature_model", "Creature", ""),
                ("foliage", "Foliage", ""),
                ("hero_landmark", "Hero Landmark", ""),
            ],
            default="kit_piece",
        )
        output_dir: StringProperty(  # type: ignore[valid-type]
            name="Pack asset dir",
            description="Pack-local output directory, e.g. .../content/core/assets/art/env/zone01/kit/wall_stone_a",
            subtype="DIR_PATH",
            default="",
        )
        source_rel: StringProperty(  # type: ignore[valid-type]
            name="Source (pack-relative)",
            description="Pack-root-relative source path, e.g. assets/art/env/zone01/kit/wall_stone_a.glb",
            default="",
        )
        source_tier: EnumProperty(  # type: ignore[valid-type]
            name="Source tier",
            items=[
                ("original", "Original", ""),
                ("ai", "AI-assisted", ""),
                ("cc0", "CC0", ""),
                ("cc_by", "CC-BY", ""),
            ],
            default="original",
        )
        authors: StringProperty(  # type: ignore[valid-type]
            name="Authors",
            description="Comma-separated author handles",
            default="",
        )
        asset_license: EnumProperty(  # type: ignore[valid-type]
            name="License",
            items=[("CC-BY-4.0", "CC-BY-4.0", ""), ("CC0-1.0", "CC0-1.0", "")],
            default="CC-BY-4.0",
        )

    def execute(self, context):  # pragma: no cover - needs bpy runtime
        import os

        obj = context.active_object
        if obj is None or obj.type != "MESH":
            self.report({"ERROR"}, "Select a mesh object to export.")
            return {"CANCELLED"}

        id_err = _sidecar.validate_id(self.asset_id)
        if id_err:
            self.report({"ERROR"}, id_err)
            return {"CANCELLED"}
        source_err = _sidecar.validate_source(self.source_rel)
        if source_err:
            self.report({"ERROR"}, source_err)
            return {"CANCELLED"}

        # E-rule rig/geoset conformance (spec ④ §3/§4): skeletal classes must pass
        # every check before export proceeds — these are blocking, unlike the
        # naming/scale/pivot warnings below.
        if self.asset_class in _SKELETAL_CLASSES:
            rig_data = _build_rig_data(context, obj, self.asset_class)
            rig_errors = _rig_checks.check_rig(rig_data)
            if rig_errors:
                for err in rig_errors:
                    self.report({"ERROR"}, err)
                return {"CANCELLED"}

        budgets = _sidecar.load_budgets()
        mesh = _mesh_info(obj)
        budget = _sidecar.compute_budget(mesh, self.asset_class, budgets)
        warnings = _sidecar.collect_warnings(mesh, self.asset_class, budget, budgets)
        for w in warnings:
            self.report({"WARNING"}, f"[{w.code}] {w.message}")

        prov = _sidecar.ProvenanceInput(
            source_tier=self.source_tier,
            authors=[a.strip() for a in self.authors.split(",") if a.strip()],
            license=self.asset_license,
        )
        doc = _sidecar.build_sidecar(
            asset_id=self.asset_id,
            asset_class=self.asset_class,
            source=self.source_rel,
            mesh=mesh,
            provenance=prov,
            import_hints={"lod_policy": "authored"},
            budgets=budgets,
        )

        os.makedirs(self.output_dir, exist_ok=True)
        stem = self.asset_id.rsplit(".", 1)[-1]
        glb_path = os.path.join(self.output_dir, f"{stem}.glb")
        yaml_path = os.path.join(self.output_dir, f"{stem}.asset.yaml")

        # glTF export via the stock exporter, with the axis/unit conventions baked
        # in so contributors cannot get them wrong (Art SAD §2.1).
        bpy.ops.export_scene.gltf(
            filepath=glb_path,
            export_format="GLB",
            use_selection=True,
            export_yup=True,
            export_apply=True,
        )

        import yaml  # bundled with Blender's Python or the tooling venv

        with open(yaml_path, "w", encoding="utf-8") as fh:
            fh.write(
                "# IF-8 sidecar (meridian/asset@1) — generated by meridian_export "
                f"v{_sidecar.ADDON_VERSION}.\n"
            )
            yaml.safe_dump(doc, fh, sort_keys=False, default_flow_style=False)

        self.report({"INFO"}, f"Exported {glb_path} + {yaml_path}")
        return {"FINISHED"}


# --- Panel ---------------------------------------------------------------------


class MERIDIAN_PT_export_panel(Panel):  # type: ignore[misc]
    """Sidebar panel: Meridian asset export."""

    bl_label = "Meridian Export"
    bl_idname = "MERIDIAN_PT_export_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Meridian"

    def draw(self, context):  # pragma: no cover - needs bpy
        layout = self.layout
        layout.operator(MERIDIAN_OT_export_asset.bl_idname, icon="EXPORT")
        # Expose the operator props inline so the artist fills a form, not YAML.
        col = layout.column()
        col.label(text="Fill in the export form, then Export.")


_CLASSES = (MERIDIAN_OT_export_asset, MERIDIAN_PT_export_panel)


def register() -> None:  # pragma: no cover - needs bpy
    for cls in _CLASSES:
        bpy.utils.register_class(cls)


def unregister() -> None:  # pragma: no cover - needs bpy
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":  # pragma: no cover
    register()
