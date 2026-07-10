# Blender tooling

## `meridian_rig` — reference rig table + generator (spec ④ §2)

```
meridian_rig/
  bones.py         PURE Python (no bpy). Canonical 63-bone table (56 Godot
                   SkeletonProfileHumanoid bones + 7 Meridian gear sockets).
  generate_rig.py  Deterministic rig generator. Builds the armature from
                   bones.py and exports the skeleton-only .glb. Arg parsing /
                   path helpers are pure module-level functions (pytest covers
                   them without Blender); bpy is only touched inside main().
```

**Blender version pin:** `Blender 5.0.0` (build date 2025-11-18, hash
`a37564c4df7a`) — the exact `--version` output of the binary used to generate
the committed rig. Regenerate with the same version to keep the artifact
byte-identical (the export is deterministic: same table + same Blender ⇒ same
SHA-256).

**Regeneration command** (from the repo root; adjust the binary path to your
install — on macOS the binary is not on PATH):

```bash
/Applications/Blender.app/Contents/MacOS/Blender --background --factory-startup -noaudio \
  --python tools/blender/meridian_rig/generate_rig.py -- \
  --profile ardent_male --out content/core/assets/art/char/sk_ardent_male_skeleton.glb
```

The committed artifact (`content/core/assets/art/char/sk_ardent_male_skeleton.glb`,
Git LFS) is validated structurally by `tests/test_meridian_rig.py` — bone
names + hierarchy are read back with pygltflib, so CI needs no Blender. On
checkouts without LFS smudge (CI), those tests skip via a pointer-file guard.

## `meridian_export` — Blender art pipeline addon

The only sanctioned glTF export path (wraps the stock exporter). For the selected
object(s) it exports a `.glb` to the pack-local asset path **and** writes a matching
`<name>.asset.yaml` IF-8 sidecar carrying the provenance + budget fields CI enforces.
Enforces at export time what CI re-checks: naming, scale/transform, kit pivots, the
50 cm grid, and the per-class budget caps (Art SAD §2.1, §4; §6.1).

## Structure (v0, issue #137)

```
meridian_export/
  __init__.py    bpy addon: bl_info, register/unregister, operator + Meridian sidebar panel.
                 Thin bpy shell — introspects the object, writes the .glb + .asset.yaml,
                 delegates ALL logic to sidecar.py.
  sidecar.py     PURE Python (no bpy import). Builds the IF-8 sidecar dict, computes the
                 budget block from the mesh, and runs the naming/scale/pivot/grid/budget
                 convention checks. Unit-testable without Blender.
  budgets.json   Shared per-class budget ceilings + naming prefixes — single source of
                 truth with tools/validate_content.py (Art SAD §2.1 AR2, no drift).
```

`tests/test_meridian_export.py` mocks `bpy` at import and validates the emitted
sidecar against the real `schema/content/asset.schema.yaml` and the #142 lints
(`validate_content.py` L021/L022 provenance, L070–072 budget).

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
