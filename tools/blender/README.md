# `meridian_export` — Blender art pipeline addon

The only sanctioned glTF export path (wraps the stock exporter). For the selected
object(s) it exports a `.glb` to the pack-local asset path **and** writes a matching
`<name>.asset.yaml` IF-8 sidecar carrying the provenance + budget fields CI enforces.
Enforces at export time what CI re-checks: naming, scale/transform, kit pivots, the
50 cm grid, and the per-class budget caps (Art SAD §2.1, §4; §6.1).

## Structure (v0, issue #137)

```
meridian_export/
  __init__.py    bpy addon: bl_info, register/unregister, operator + Meridian sidebar panel.
                 Thin bpy shell — introspects the object/armature, writes the .glb +
                 .asset.yaml, delegates ALL logic to sidecar.py / rig_checks.py.
  sidecar.py     PURE Python (no bpy import). Builds the IF-8 sidecar dict, computes the
                 budget block from the mesh, and runs the naming/scale/pivot/grid/budget
                 convention checks. Unit-testable without Blender.
  rig_checks.py  PURE Python (no bpy import). E-rule band: rig/geoset conformance checks
                 for skeletal export classes (`character_model`, `armor_model`). Reads
                 the canonical bone table (`tools/blender/meridian_rig/bones.py`) and the
                 geoset region vocabulary (`schema/content/skeleton.defs.yaml`) — no
                 second copy of either enum. Unit-testable without Blender.
  budgets.json   Shared per-class budget ceilings + naming prefixes — single source of
                 truth with tools/validate_content.py (Art SAD §2.1 AR2, no drift).
```

`tests/test_meridian_export.py` mocks `bpy` at import and validates the emitted
sidecar against the real `schema/content/asset.schema.yaml` and the #142 lints
(`validate_content.py` L021/L022 provenance, L070–072 budget).

## E-rules — rig/geoset conformance (spec ④ §3/§4)

For `character_model` and `armor_model` exports, `rig_checks.check_rig()` runs
before the `.glb`/sidecar are written; any error **blocks the export** (unlike the
naming/scale/pivot checks above, which only warn).

| Rule | Checks | Applies to |
|------|--------|------------|
| E100 | Every exported bone name is in the canonical `skeleton.defs.yaml` bone set (63 names: 56 profile + 7 sockets) | `character_model`, `armor_model` |
| E101 | All 7 socket bones (`socket_main_hand`, `socket_off_hand`, `socket_shield`, `socket_back`, `socket_ranged`, `socket_hip_l`, `socket_hip_r`) are present | `character_model` only (gear binds a subset of canonical bones, never the socket mounts) |
| E102 | Body meshes are named `geo_<region>_lod<N>`; all 8 geoset regions are covered at `lod0` | `character_model` with meshes (a skeleton-only export — the committed rig asset — carries no meshes and passes) |
| E103 | ≤4 skin-weight influences per vertex, weights normalized (sum to 1.0) | `character_model`, `armor_model` |
| E104 | `geo_*`-named meshes must name a known geoset region; `armor_model` meshes must NOT be `geo_*`-named at all (gear binds whole pieces via item@2 `worn.hides`, it doesn't carry geoset names) | `character_model`, `armor_model` |

Geoset regions (8, from `schema/content/skeleton.defs.yaml` `$defs.geosetRegion`):
`head`, `hands`, `forearms`, `torso`, `waist`, `hips_legs`, `lower_legs`, `feet`.

**Repo-checkout requirement:** the E-rule vocabulary (bone table from
`tools/blender/meridian_rig/bones.py`, geoset regions from
`schema/content/skeleton.defs.yaml`) is loaded lazily on the first skeletal
export, not at addon import — so the addon installs and non-skeletal exports
work anywhere, but exporting `character_model`/`armor_model` requires running
Blender from a Project Meridian repo checkout (otherwise the export aborts with
an error saying exactly that).

## Sidecar fields emitted

`schema`, `id`, `class`, `source` (pack-root-relative), `license`, `provenance`
(`source_tier` default `original`, `authors`, and tier-conditional `origin_url` /
`attribution` / `license_verified_on` / `ai`), a mesh-derived `budget` block
(`lod0_tris`, `texture_max_px`, `material_sets` for kit pieces), `import_hints`,
and `restyle_status`.

## Install / use

Zip `meridian_export/` and install via Blender → Preferences → Add-ons → Install.
Panel appears in **View3D → Sidebar → Meridian**. Fill the export form (asset id,
class, pack asset dir, source path, provenance) and Export. Pinned to Blender LTS.

**Contract:** [Art SAD §2.1](../../docs/sad/art-sad.md) · schema
[asset.schema.yaml](../../schema/content/asset.schema.yaml).
