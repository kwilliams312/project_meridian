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

# --- Import the pure module whether loaded as a Blender addon (package) or via a
# direct path insertion. Blender registers the package, so relative import works;
# the try/except keeps the module importable standalone for tooling.
try:  # pragma: no cover - exercised inside Blender
    from . import sidecar as _sidecar
except ImportError:  # pragma: no cover - standalone fallback
    import sidecar as _sidecar

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
