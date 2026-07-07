# `/client/import-presets` — Godot import preset templates (Art SAD §2.3)

Versioned, per-asset-class Godot import settings. These are the **canonical import
policy**: how Godot imports a repo `.glb` / `.png` / `.wav` into an engine resource,
fixed so every contributor's editor (and headless CI) produces the *same* resource
from the same source (Art SAD §8.1 determinism; PRD §4.2 "Godot import-settings
presets are a pipeline deliverable (M0)").

They serve two jobs at once:

1. **Document the policy** — one file per class, plain text, diffable, versioned. A
   preset bump triggers a full headless reimport in CI (Art SAD §2.3 / §8.1).
2. **Drive the validator** — `tools/validate_imports.py` reads these templates and the
   asset sidecars (IF-8 `class` + `import_hints`, `schema/content/asset.schema.yaml`)
   and hard-fails any asset whose settings are off-preset (Art SAD §4.2 model, mirrors
   the #142 hard-fail posture). If a Godot `.import` file exists for a source, it is
   validated field-by-field against the class template too.

## Files

| File | IF-8 classes | Applies to |
|------|--------------|------------|
| `presets.json` | — | The machine-readable preset set the validator loads (single source of truth). |
| `character.import.tmpl` | `character_model`, `armor_model`, `creature_model` | `sk_char_*`, `sk_npc_*`, skinned meshes |
| `env-kit.import.tmpl` | `kit_piece`, `hero_landmark` | `sm_env_*_kit_*`, hand-authored LOD chains |
| `weapon.import.tmpl` | `weapon_model` | `sm_*` weapon meshes, bone-attached |
| `prop.import.tmpl` | `prop` | `sm_prop_*`, small clutter |
| `foliage.import.tmpl` | `foliage` | `sm_fol_*`, MultiMesh fields |
| `texture-std.import.tmpl` | `texture_set`, `icon` | `t_*` base color / ORM / normal, UI icons |
| `vfx-texture.import.tmpl` | `vfx` | `t_fx_*`, particle/flipbook textures |
| `audio.import.tmpl` | `sfx`, `ui_sound`, `music_stem`, `music_stinger`, `ambience_bed`, `ambience_emitter` | `.wav` / `.ogg` sources |

The `.import.tmpl` files are Godot `.import`-shaped templates (the `[params]` block a
`ResourceImporterScene` / `ResourceImporterTexture` / `ResourceImporterWAV` produces),
carried as documentation of the exact settings the plugin stamps (`meridian_import`,
Art SAD §2.3). `presets.json` is the normalized form the Python validator consumes so it
never has to parse `.import` INI to know the policy.

## Grounding (min-spec, arm64/Metal + D3D12)

Every setting traces to the Art SAD / PRD:

- **Textures VRAM-compressed** (`compress/mode=2`, VRAM Compressed → BC on D3D12 /
  ASTC-or-BC on arm64 Metal): the 1060 has **6 GB** and Godot has **no engine-managed
  texture streaming** — these are *resident-VRAM* budgets (PRD §2.3). Uncompressed or
  lossless-imported textures blow the budget on min-spec, so VRAM compression is
  mandatory, not optional.
- **Mipmaps on** for world textures (PRD §2.3 "mipped on import"); **off** for UI
  icons (screen-space, never minified) and VFX flipbooks (Art SAD §2.3 vfx-texture).
- **sRGB vs linear per suffix** (`_bc` = sRGB base color, `_orm` / `_n` = linear data)
  — Art SAD §2.3 vfx-texture / texture-std; wrong color space double-corrects and
  breaks PBR response.
- **Size caps per class** (PRD §2.3 table): the import must not upscale past the class
  cap; source is authored at 2× and the *imported* max stays at the class ceiling.
- **Model LOD import** — importer LOD generation **off** for hand-authored classes
  (character, env-kit, foliage, hero landmark) so authored chains are not double-LODed;
  **allowed** for `prop` (PRD §2.1 / §2.2, Art SAD §2.3). Skeleton + BoneMap retained
  for skinned classes; lightmap UV2 unwrap for statics that carry it; occluder + static
  collision node conversion for kit pieces (Art SAD §2.3, PRD §2.4/§6.1).
- **Audio** — `.wav` kept as PCM for short SFX/UI (low latency, Music SAD encode tier);
  loopable streams may be Ogg Vorbis. Force-mono for positional 3D sources.

The numeric caps here are the *import* ceilings; per-asset triangle/VRAM **budgets**
live in `tools/blender/meridian_export/budgets.json` and are enforced separately by
`validate_content.py` L070-072 (#142). This validator does **not** duplicate those —
it checks import *settings* conformance only.
