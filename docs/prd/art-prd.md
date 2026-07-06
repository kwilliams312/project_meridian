# Art Track PRD — Project Meridian

**Track:** Art
**Version:** 0.6 — 2026-07-06 (v0.6: A-15 RESOLVED / D-31 — hand-authored source + IF-8 sidecars live pack-local at `content/<ns>/assets/**`; §3.3/§4.3 clarify that `/client/art` (`res://art/...`) is the *imported* `mcc` output, not a source location. v0.5: reviewed against Baseline v0.6 / D-29 (OPS-05 telemetry) — no art deliverables. v0.4: reviewed against Baseline v0.5 / D-28 (macOS client) — no art-scope change: budgets stay GPU-tier-based and the **GTX 1060 bench remains the authoritative min-spec gate** (D-28 rule 2); the M1 Mac is a second Low-tier reference operated by the Client track. v0.3: reviewed against Baseline v0.4 — sharded-realm changes carry no Art deliverables; §3.3 amended per the Art SAD design call: provenance records live inside the IF-8 sidecar (`meridian/asset@1`), not a separate tree; terrain-decision date aligned to the A-09 M0-exit gate. v0.2: engine pivot UE5 → Godot 4.6 per baseline v0.3; no-Nanite LOD budgets, sourcing tiers revised, glTF pipeline)
**Baseline:** [Game Design Baseline v0.6](../00-GAME-DESIGN-BASELINE.md) (binding). All feature IDs, milestone names (M0–M4), and technical decisions (TD-01..TD-12) referenced here are defined there and are not redefined in this document.
**Owner:** Art track lead (TBD)
**Reviewers:** Client track (rendering budgets), Tools track (TLS-02 kit contracts), Server track (consulted only)

---

## 1. Overview & Art Direction

### 1.1 Direction statement (TD-10)

Project Meridian's look is **stylized-realistic "painterly PBR"**: physically based materials and modern real-time lighting (Godot 4.6 Forward+ on D3D12, TD-02), but with hand-authored, painterly surface detail, pushed silhouettes, and deliberate color scripting. The target is *better fidelity than WoW, explicitly not photorealism*, readable at MMO camera distance (10–30 m third-person pullback), and performant on GTX 1060-class hardware (TD-03) with no hardware ray tracing ever assumed (TD-02).

The engine pivot does not change the direction — it changes where fidelity comes from. Without a Nanite equivalent, geometric density is earned through disciplined LOD chains, aggressive instancing, and kit design (§2), not raw polygon spend. Painterly PBR is, if anything, a better fit: painted value structure carries detail that scan-dense geometry no longer can.

### 1.2 Pillars

1. **Silhouette first.** Every character, creature, weapon, and landmark must be identifiable from its silhouette alone at 25 m. Gameplay-relevant reads (enemy class, gear rarity, cast state) never depend on fine texture detail.
2. **Painterly PBR, not photo scan.** We use the full PBR pipeline (base color / roughness / metallic / normal / AO), but base color carries hand-painted value structure — gradients, edge highlights, saturated cores — instead of albedo-neutral scan data. Scanned/AI/photoreal source material is always repainted (§3.4).
3. **Color is a script.** Each zone has an authored palette (3 dominants + 2 accents) documented in the art bible. Player/enemy/interactive colors are reserved: rarity colors (ITM-03), hostile red-orange, interactive gold shimmer never appear as ambient environment dominants.
4. **Exaggerate proportion, restrain noise.** Heroic proportions (see 1.3), oversized weapons and landmarks, but low-frequency surface detail. If a texture reads as noise at 15 m, it is too busy.
5. **Runs on real hardware.** Every art decision is validated on the min-spec profile (TD-03). Beauty that costs the 1060 its 30 FPS is rejected, full stop.

### 1.3 "Better than WoW but not photoreal" — concretely

| Dimension | WoW (reference floor) | Meridian target | Photoreal (explicitly avoided) |
|---|---|---|---|
| Character proportions | ~5.5-head heroic, hands/shoulders heavily exaggerated | **6.5–7-head heroic**; hands/feet/shoulders +15–25% over realistic; faces stylized with simplified planes | 7.5–8-head realistic anatomy |
| Materials | Hand-painted diffuse only, no PBR | **Full PBR with painted base color**; real metal/cloth/leather response; painted micro-detail instead of scan noise | Scan-based albedo, photo textures |
| Saturation | Very high, often clashing | **High but scripted**: zone palettes with reserved gameplay hues; saturated cores, desaturated shadows | Neutral, physically measured |
| Silhouette | Extreme (shoulder pads > head) | **Strong but wearable**: gear silhouette tiers scale with rarity (ITM-03) without full absurdity | Realistic tailoring |
| Geometry density | Very low poly, painted-in detail | **Budgeted modern density** (40–60k-tri heroes, LOD-chained kits) with sculpted forms replacing painted-in shading; density from instancing + kit reuse, never unbounded meshes | Unbounded scan meshes |
| Lighting | Baked vertex/simple lightmaps | **SDFGI on High tier / baked lightmaps + reflection probes on Low** (TD-02); day/night (WLD-02) with authored color grading per time-of-day | Hardware RT, path-traced looks |

Reference points (mood, not to copy): *World of Warcraft* (readability floor), *Ashes of Creation* pre-alpha stylization (upper bound to stay under), *Sea of Thieves* (painterly water/materials), *Fable (2023 reveal)* and *Kena: Bridge of Spirits* (painterly PBR characters), *Riot's Arcane* (color scripting), *Guild Wars 2* (zone palette discipline).

### 1.4 M0 deliverable: the Art Bible

The art bible is the M0 exit artifact for this track (Baseline §3, M0 "Art" bullet). Contents:

- Direction statement, pillars, and the comparison table above, with painted keyframes: 2 zone mood paintings, 1 character lineup, 1 gear-rarity lineup.
- Proportion sheets: human male/female base bodies (CHR-01), creature archetype scale chart vs. player.
- Master material standards and texel-density chart (§2.3).
- Zone-01 color script.
- The style-test set: **1 finished character (art.char.human.male.base) + 1 environment kit (Zone-01 starter kit, ~15 pieces)** pushed through the full Blender→glTF→Godot pipeline (§4), proving the pipeline per the M0 milestone definition.

Location: `/docs/art-bible/` (markdown + images in Git LFS per TD-12).

---

## 2. Technical Art Constraints

All budgets are per TD-03: **min-spec = GTX 1060 6GB / 16GB RAM @ 1080p Low / 30 FPS; recommended = RTX 3070 @ 1440p High / 60 FPS**, Godot 4.6 Forward+ on D3D12 (TD-02), no hardware RT. **There is no Nanite equivalent: every asset class ships an explicit LOD chain**, and the Low-tier draw-call ceiling — not triangle count — is the binding constraint on kit and scene design. Budgets are LOD0 unless stated; enforcement is via the asset review flow (§4.4) and the min-spec perf gate (§8.1).

### 2.1 Geometry budgets per asset class

LOD percentages are triangle counts relative to LOD0. Switch distances are starting points, tuned per zone in the min-spec bench (§8.1) via Godot visibility ranges (with fade).

| Asset class | LOD0 tris | LOD chain | Bones / notes |
|---|---|---|---|
| Player character body (CHR-01) | 45–60k | LOD0-3 at 100 / 50 / 20 / 7% (≈ 30k / 12k / 4k) | ≤ 120 bones incl. face; one shared skeleton per race/sex |
| Armor set piece (worn, ITM-01) | 3–8k per slot | follows body LOD chain | Skinned to body skeleton; full 8-slot outfit ≤ 40k added tris |
| Weapon (ITM-01) | 6–12k | LOD0-2 at 100 / 30 / 8% | Static mesh, attached to bone attachment; 2-hander/legendary up to 15k |
| Humanoid mob (NPC-01) | 20–35k | LOD0-3 at 100 / 50 / 20 / 8% | ≤ 90 bones |
| Large creature/boss (NPC-01, GRP-02) | 60–90k | LOD0-3 at 100 / 40 / 15 / 8% | ≤ 150 bones; max 2 on screen assumed |
| Critter/ambient creature | 5–10k | LOD0-2 at 100 / 40 / 15% | ≤ 40 bones |
| Prop, small (ECO-02 nodes, clutter) | 0.5–3k | LOD0-2 at 100 / 50 / 20% | Importer-generated LODs acceptable; MultiMesh-batched when repeated |
| Environment kit piece (WLD-01, TLS-02) | 2–10k (arch module); ≤ 20k for large set pieces | **LOD0-3 at 100 / 50 / 25 / 10%**, hand-authored; borders vertex-snapped at every LOD | Sculpt detail baked to normals; no unbounded source meshes in-engine |
| Hero landmark / POI piece (WLD-03) | 40–80k | LOD0-3 at 100 / 40 / 15 / 5% + far-distance proxy/imposter | One per POI vista, not clustered |
| Foliage: tree | 10–20k | LOD0-2 at 100 / 40 / 15% + billboard imposter at LOD3 | MultiMesh-instanced; shader wind |
| Foliage: grass/ground cover | ≤ 500 per clump | 1 LOD + cull at 40 m (Low) / 80 m (High) | MultiMesh only, never individual nodes |

### 2.2 LOD, instancing & culling policy (no Nanite)

- **Every static and skeletal asset ships its LOD chain per 2.1.** Hand-authored LODs for kit pieces, heroes, creatures, foliage, and landmarks; Godot importer-generated LODs are acceptable only for small props and are checked at review. LOD3 of a kit piece must still respect the kit seam contract (§6.1).
- **Instancing is the density engine.** Foliage, clutter, and repeated props render via `MultiMeshInstance3D` (placed through TLS-02 scatter tools); kit families share materials so the renderer batches them. "Better than WoW" density comes from many cheap instances of well-made pieces, not from heavy meshes.
- **Culling is authored, not free.** Kit pieces that plausibly block sightlines (walls, cliffs, large buildings) ship occluder geometry for Godot's raster occlusion culling; interiors get manual room-scale occluders. Draw-distance and visibility-range tuning per zone is an art deliverable, not an afterthought.
- **Honest density expectation:** a Zone-01 vista on Low is budgeted at ≤ 3.5M rendered triangles and ≤ 2,500 draw calls after culling (§2.5). This is denser than WoW by a wide margin but well short of a Nanite scene; dressing plans and the beautiful-corner benchmark (§6.2) are built to this number, not to UE5 screenshots.
- Shadows: directional cascaded shadow maps, tuned per scalability tier; Low uses reduced cascade count and shadow distance — kit and foliage LODs must not pop visibly inside the Low shadow range.

### 2.3 Texture & texel-density budgets

Source textures authored at 2× target and mipped on import (Godot VRAM compression, BC-class). **Godot has no engine-managed streaming pool comparable to UE virtual texturing — these are resident-VRAM budgets, enforced at authoring time and verified on the bench machine.**

| Asset class | Base color / ORM / Normal | Total VRAM budget |
|---|---|---|
| Player character body | 2048² set (4096² source) | ≤ 24 MB |
| Armor set (8 slots) | shared 2048² + 1024² trims | ≤ 32 MB |
| Weapon | 1024² (2048² for legendary) | ≤ 6 MB |
| Mob | 2048² | ≤ 24 MB |
| Boss | 4096² allowed | ≤ 64 MB |
| Prop | 512²–1024² or trim-sheet | ≤ 4 MB |
| Environment kit | trim sheets + tiling 2048² materials; ≤ 12 unique material sets per kit | ≤ 224 MB per kit resident |
| Terrain (WLD-01) | tiling layer materials, 2048² per layer, ≤ 8 layers blended ≤ 4 per chunk | ≤ 256 MB per zone |

Texel density: 512 px/m environment standard, 1024 px/m first-person-visible gear, 256 px/m distant/roof/underside. Zone total resident texture target: **≤ 2.0 GB on Low** (the 1060's 6 GB must also hold meshes, render targets, and lightmaps — baked-GI zones carry a lightmap budget of ≤ 192 MB per zone, owned jointly with Client track).

### 2.4 Material / shader budget

- **Master-material library, not bespoke shaders.** ≤ 12 master materials game-wide (Character, Hair, Eye, EnvOpaque, EnvTrimsheet, Foliage, Terrain, Water, Glass, VFX-Translucent, UI, Decal). In Godot terms: `StandardMaterial3D`-based presets where the standard pipeline suffices (EnvOpaque, Prop cases), and a small library of custom **`.gdshader` spatial shaders** for the rest. All assets use per-asset parameter overrides of a master — no one-off shaders merged, ever.
- Complexity discipline: masters are budgeted and profiled per scalability tier on the bench machine; Foliage carries shader wind (vertex displacement) and must stay cheap enough for MultiMesh fields on Low. New shader features require a min-spec capture before merge.
- Translucency is the min-spec killer: VFX overdraw budget per screen ≤ 4× at 1080p (§2.5); no lit/shaded translucency on Low — VFX-Translucent is unshaded there.
- **SDFGI on High tier; Low ships baked lightmaps (`LightmapGI`) + reflection probes only** (TD-02). All zones must be lit acceptably under both paths, checked at review (§8.2). Static env meshes therefore need clean lightmap UV2 (validated at import, §4.2). SDFGI's known light-leak behavior around thin walls is an authored-around constraint: kit wall pieces meet minimum-thickness rules documented in the art bible (see risk R7, §10).

### 2.5 Draw-call & crowd discipline (50+ player scenes, IT-M1)

IT-M1 requires 50+ concurrent players; battlegrounds (PVP-02) put 20+ in one screen. **With no Nanite, the ≤ 2,500 draw-call ceiling on Low is the binding constraint on kit design** — it is cheaper to break a budget with 400 unbatched unique props than with any triangle count. Constraints:

- Scene budget on Low: ≤ 2,500 draw calls, ≤ 3.5M rendered tris after culling.
- Characters: modular gear merges to ≤ 10 surfaces per character; **crowd LOD rules** — beyond 30 m characters drop to LOD2 and skeletal/animation update rate halves (custom anim-tick throttling built with Client track; Godot has no built-in animation budget allocator), beyond 60 m LOD3 + no cloth/secondary physics.
- Kit pieces instance-friendly by construction: shared material instances across a kit, MultiMesh-safe, no per-piece unique materials (this is also a TLS-02 Forge contract, §6).
- Particle systems: `GPUParticles3D` only (CPU particles reserved for UI/menu scenes); per-spell caps in §5 (VFX); global on-screen particle budget enforced by scalability tiers.

---

## 3. Asset Sourcing & Provenance

Per TD-09: original art is CC-BY 4.0; third-party assets must be **CC0 or CC-BY only**; **engine-locked marketplace content (Quixel Megascans, Fab, Unreal Marketplace, Unity Asset Store) is disallowed** — those licenses bind the assets to their engine and are incompatible with an open, redistributable Godot stack. AI-generated assets are allowed **with provenance recorded per asset**. Every asset in the repo has a provenance record — no record, no merge (enforced in review, §4.4, and content CI where automatable).

### 3.1 Source tiers

The former "free UE-compatible" tier (Quixel/Epic marketplace/Fab) is **deleted**: its content is licensed *for use in Unreal Engine projects only* and cannot ship in a Godot client or be relicensed CC-BY. The CC-library tier expands to take its place — with the honest caveat that it does not match Megascans' coverage or scan quality, so **more restyle and original work is assumed than the UE plan carried** (risk R3/R8, §10).

| Tier | Source | Use |
|---|---|---|
| A | Original work (Blender/Substance/Krita) | Characters, gear, hero assets, anything brand-defining |
| B | AI-generated + mandatory cleanup pass (§3.2) | Concept art, texture bases, prop/creature starting points, skybox mattes; **also the primary gap-filler where the CC0 libraries lack a needed surface/prop family** |
| C | CC0/CC-BY libraries: **PolyHaven** (CC0 textures/HDRIs/models), **ambientCG** (CC0 PBR materials), **Sketchfab CC0-filtered**, **OpenGameArt** (CC0/CC-BY filtered), **Kenney** (CC0 props/UI) | Environment fill, surfaces, decals, HDRIs, props — always restyled (§3.4); CC-BY attribution auto-generated into CREDITS from provenance records |

Everything sourced under tier C is engine-agnostic and redistributable, so the entire shipped asset set can honor TD-09 with no per-asset license carve-outs — a real improvement over the old `ue-only` contamination problem, paid for in throughput.

### 3.2 AI-generated asset workflow

- **Approved uses:** concept/mood paintings, texture base layers, trim-sheet ideation, prop/creature blockout meshes (image-to-3D as *starting geometry only*), matte skyboxes, and CC0-library gap-filling (tileable surface generation where PolyHaven/ambientCG have no equivalent).
- **Recommended tools:** image — models/services whose terms grant full commercial rights and don't restrict open-source redistribution (verify at time of use; record tool+version per asset); 3D — image-to-3D generators under the same terms test. Tools trained on known-infringing sets or with output-ownership restrictions are disallowed.
- **Mandatory cleanup pass (no raw AI output ships):** retopologize to budget (§2.1) including the full LOD chain, re-UV (incl. lightmap UV2 for statics), rebake, and **repaint base color to the painterly standard** (§1.2). Meshes: fix symmetry/silhouette, sculpt pass for intentional forms. Textures: repaint at minimum 40% of surface area — AI output is reference/underpainting, not the deliverable.
- **Quality bar:** an AI-assisted asset must be indistinguishable in style review (§7.2) from Tier-A work. Reviewers are told the tier only *after* the style verdict.
- **Prompt hygiene:** no prompts invoking "Blizzard", "World of Warcraft", artist names, or franchise terms; prompts are stored in the provenance record and are auditable.

### 3.3 Provenance record format (TD-09)

**The provenance record is a block inside the asset's IF-8 sidecar** (`meridian/asset@1`, [asset.schema.yaml](../../schema/content/asset.schema.yaml)) — one file per asset carrying source path, import hints, and provenance together, so nothing can drift between two YAML trees (design call in the Art SAD §3; supersedes this PRD's earlier `/content/assets/provenance/` sketch). The sidecar and its hand-authored source file live **pack-local at `content/<ns>/assets/**`** (A-15 / D-31, Sync Decisions §7.1), and `source:` is pack-root-relative:

```yaml
# content/<ns>/assets/art/env_zone01_rock_cliff03.asset.yaml (meridian/asset@1)
schema: meridian/asset@1
id: core:art.env.zone01.rock.cliff03
class: prop
source: assets/art/env/zone01/rock_cliff03.glb   # pack-root-relative; hand-authored source
license: CC0-1.0                # SPDX; CI allowlist = CC0-1.0, CC-BY-4.0
provenance:
  source_tier: cc0              # original | ai | cc0 | cc_by
  origin_url: "https://..."     # license pinned to URL + date
  license_verified_on: 2026-09-14
  authors: ["contributor-github-handle"]
  transform_notes: "restyled to painterly standard; recolored to Zone-01 palette; LOD0-3 authored"
restyle_status: done            # tiers ai/cc0/cc_by cannot merge as pending
reviewed_by: ["art-lead-handle", "deputy-handle"]
```

The content compiler (TLS-01) tooling enforces this via TLS-07 lints L020–L022: every asset ID referenced by content must have a sidecar; any record whose license is not CC0/CC-BY **fails CI outright** — there is no longer a tolerated engine-locked tier to tag. AI-tier assets additionally require the `provenance.ai` block (tool + auditable prompts file), schema-enforced.

### 3.4 Restyling pipeline — "kitbash to style"

Every Tier B/C asset passes through a fixed restyle checklist before review: (1) rebase materials onto our master materials; (2) repaint/regrade base color into the zone palette with painted value structure; (3) normal-map detail reduction (kill scan micro-noise below the 15 m readability threshold); (4) silhouette pass — exaggerate 1–2 forms; (5) trim/decal pass with Meridian motif sheets (authored at M0) so mixed-source assets share ornamentation DNA; (6) **LOD-chain authoring to §2.1** — CC0 sources rarely ship game-ready LODs, so this is a standing line item in restyle effort estimates. Target: a screenshot containing Tier A + restyled Tier C assets side by side shows no visible seam in style review.

### 3.5 Disallowed

- Any Blizzard/WoW asset, extracted data, or file format — and **lookalikes**: no recreations of identifiable WoW gear sets, creature designs, zone layouts, logos, or UI trade dress (Baseline Pillar 5).
- **Engine-locked marketplace content:** Quixel Megascans (any tier), Fab, Unreal Marketplace, Epic sample content, Unity Asset Store — regardless of price, per TD-09. Existing style-test material sourced from these during pre-pivot exploration is purged and re-sourced.
- GPL/NC/ND/SA-viral–licensed art (CC-BY-SA is **not** accepted — share-alike conflicts with the mixed CC-BY pipeline).
- "Free" rips from Sketchfab/OGA with unverifiable authorship; assets whose license can't be pinned to a URL + date.
- AI tools that claim ownership of output or forbid commercial/open-source use.

---

## 4. Pipeline

### 4.1 DCC standard

- **Blender (LTS release, currently 4.x)** is the open-source default DCC — free, contributor-accessible, first-class glTF export. Sculpting in Blender; ZBrush accepted for contributors who own it (deliver decimated + retopo).
- Texturing: **Substance 3D Painter** recommended (industry standard; personal licenses common) with **ArmorPaint/Blender+Krita** as the fully-open alternative — a shared Painter smart-material + Blender node-group library encodes the painterly standard (M0 deliverable).
- Rigging/animation: Blender + our published rig files; in-engine polish via Godot's `AnimationTree`/retarget stack (with Client track).

### 4.2 Blender → glTF 2.0 → Godot import conventions

- **glTF 2.0 (`.glb`) is the interchange format** — it is Godot's first-class import path and an open standard, matching the open-stack mandate. FBX is not used.
- Units: 1 Blender unit = 1 m (glTF and Godot agree, no import scale factor). Godot is -Z forward, Y up; the export preset handles axis conversion. All meshes: transforms applied, pivot per kit rules (§6.1).
- Export: 1 `.glb` per asset; naming prefixes retained from the original convention: `sm_` (static mesh), `sk_` (skeletal), `t_` (texture), `m_`/`mi_` (master/variant material), `a_` (anim), `fx_` (particle system scene). LODs as suffixed meshes (`_lod0..3`) picked up by the importer.
- Skeletal: one skeleton per race/sex, shared by all gear; blend shapes (morph targets) for customization sliders (CHR-01), carried through glTF.
- **Godot import-settings presets are a pipeline deliverable (M0):** versioned import presets per asset class (compression mode, mipmaps, LOD import behavior, lightmap UV2 unwrap for statics, physics/occluder generation flags) committed to the repo so every contributor's import is identical.
- Every import goes through an **import validator (Godot editor plugin, built with Tools track):** checks naming, scale, LOD chain presence per §2.1, master-material parentage, lightmap UV2 where needed, occluder presence for wall-class kit pieces, and provenance record existence. Fails loudly.

### 4.3 Asset IDs, folders, Git LFS

- Asset IDs per Baseline §5.3: `art.<category>.<zone|race|set>.<name>.<variant>` — e.g. `art.char.human.male.base`, `art.env.zone01.kit.wall_stone_a`, `art.vfx.cmb.fireball.impact`, `art.ui.hud.frame.party`. Content files reference IDs, never file paths.
- **Two distinct trees, not to be confused (A-15 / D-31):**
  1. **Hand-authored source lives pack-local** at `content/<ns>/assets/**` — the `.glb`/`.png` shipped source and its IF-8 sidecar, one subtree per pack for `.mcpack` self-containment (TLS-08). This is what artists commit and what the IF-8 `source:` field points at.
  2. **`/client/art/` (`res://art/...`) is the *imported* Godot resource output** — the `.scn`/`.ctex`/etc. that `mcc` generates from the source into the `.pck` (Tools SAD §2.7), laid out by asset ID (`res://art/env/zone01/kit/...`). It is a build artifact, **not** a source location; artists never author there.
- Mapping ID→resource path lives in a generated manifest consumed by the content compiler (TLS-01); the imported resource layout mirrors the ID.
- Git LFS (TD-12) for all binaries: `.glb .gltf .blend .png .tga .psd .exr .wav`. Godot text resources (`.tscn`, `.tres`, `.gdshader`, `.import`) stay plain-text in Git — diffable and mergeable, a pivot win. Shipped source `.glb`/`.png` and sidecars live pack-local under `content/<ns>/assets/**`; working `.blend`/`.spp` DCC sources live in a never-packed `<ns>-art-source/` tree. LFS locking enabled for `.blend` rig/skeleton sources and other binary shared files.

### 4.4 Review & approval flow

1. Contributor opens PR: asset(s) + provenance record + validator screenshot + in-Godot screenshot on the standard review map (day + night lighting).
2. **Automated gate:** import validator + provenance lint in CI.
3. **Style gate:** art lead or deputized style reviewer scores against the style checklist (§8.2). One revision round expected.
4. **Perf gate:** budgets (§2) checked in-engine — LOD chain verified at each switch distance; min-spec profile capture for anything category-new (§8.1).
5. Merge; asset ID registered in the manifest; nightly test-realm build picks it up (Baseline §6).

---

## 5. Asset Categories & Scope by Milestone

Counts are planning estimates (±30%), sized for a small core team + contributors. "Set" = modeled, textured, rigged/implemented, in-engine, approved — **including the full LOD chain**, which the no-Nanite pivot adds to every static asset's definition of done.

### M0 — Foundation (months 0–4) — prove the pipeline

| Deliverable | Feature IDs | Estimate |
|---|---|---|
| Art bible + style tests (§1.4) | TD-10, all | 1 bible, 4 keyframes, 2 style-test scenes |
| Human base bodies (male/female), 1 face set, placeholder customization | CHR-01 (M0 stub) | 2 bodies, 1 shared skeleton, 4 blend-shape sliders |
| Pipeline-proof character: `art.char.human.male.base` fully through Blender→glTF→Godot | CHR-01 | 1 |
| Locomotion stub set (idle/walk/run/jump) on shared skeleton | CHR-02 (M0 basic) | ~12 anims |
| Zone-01 starter environment kit (full LOD0-3 chains) + 1 terrain material | WLD-01 (prep) | ~15 kit pieces, 1 terrain material (4 layers) |
| Empty-test-map dressing for IT-M0 | — | reuse of the above |
| Motif/trim sheets + master material library v1 (`.gdshader` masters + StandardMaterial3D presets) | §2.4, §3.4 | 12 masters, 3 trim sheets |
| Godot import-settings presets per asset class (§4.2) | pipeline | 1 preset set, versioned |

**M0 total: ~35 assets + bible.**

### M1 — Greybox Vertical Slice (months 4–9) — readable, ugly on purpose

Greybox discipline: Zone-01 is built in the zone editor (TLS-02) from **greybox kits with final pivots/dimensions** (§6.2); only the gameplay-critical art is finished.

| Deliverable | Feature IDs | Estimate |
|---|---|---|
| 1 playable race final (2 sexes), customization set v1 (4 hair, 4 face, 3 skin palettes) | CHR-01 | 2 finished characters + ~11 variant parts |
| Full locomotion set (walk/run/jump/swim/turn/strafe) | CHR-02 | ~40 anims |
| Combat animation sets: melee class + caster class (GCD attacks, casts, hits, blocks) | CMB-01 | ~50 anims |
| Death/resurrect/ghost-form treatment + corpse-run readability (ghost material, graveyard props) | CMB-03 | ~8 anims + 1 material + 4 props |
| Buff/debuff VFX starter set + aura icons | CMB-04 (M1 basic) | 10 VFX, 24 icons |
| Spell/ability VFX for 2 classes (cast loop, projectile, impact per ability) | CMB-01 | ~24 GPUParticles3D systems (cap: ≤ 200 GPU particles/spell, ≤ 2 translucent layers) |
| Mobs for Zone-01: 6 creature families × leader/normal variants | NPC-01 | 8 rigged creatures, ~15 anims each |
| NPC townsfolk/vendor/trainer bodies (reuse player rig + costume sets) | NPC-01 | 6 costume sets |
| Zone-01 greybox kit (complete coverage: town, wilds, camps, cave) + terrain material set | WLD-01, TLS-02 | ~80 greybox pieces, 8 terrain layers |
| POI landmarks (greybox + 1 finished vista), world-map art for Zone-01 | WLD-03 | 5 greybox + 1 final, 1 map painting |
| Weapons & armor: 3 weapon families × 3 tiers; 2 armor sets/class × rarity tint language v1 | ITM-01 | ~15 weapons, 4 armor sets |
| Loot/container/vendor props | ITM-01, ECO-01 | ~12 props |
| Core HUD art: unit frames, action bars, bags, minimap/map frame, quest tracker skin | UI-01, QST-01 | 1 HUD kit (~60 sprites/9-slices), 40 ability icons |
| SFX-support visuals (hit sparks, footstep decals) | AUD-01 (support) | ~8 |

**M1 total: ~120 modeled/VFX/UI assets + ~150 animations.** IT-M1 contribution: everything above running with 50+ players on min-spec (§8.3).

### M2 — Systems Depth (months 9–15) — the beautiful corner

| Deliverable | Feature IDs | Estimate |
|---|---|---|
| **Zone-01 full art pass** — the "beautiful corner": final kits (replace greybox 1:1 by pivot contract §6.2), foliage set (MultiMesh-scattered), water, skybox set | WLD-01 | ~150 final kit pieces, 20 foliage, 3 skies |
| Zone-02 greybox kit (biome-differentiated) | WLD-01 | ~60 greybox pieces |
| Day/night lighting rigs (SDFGI High / lightmap-baked Low, both validated) + weather VFX (rain, fog, wind-reactive foliage) + time-of-day grading | WLD-02 | 4 lighting scenarios, 5 weather systems |
| Talent-tier visual feedback VFX (procs, stances) | CMB-04 | ~15 VFX |
| Itemization rarity language v2: color/glow/silhouette tiering across common→epic; stat-proc VFX | ITM-03 | rarity spec + ~10 glow/trim treatments, 6 proc VFX |
| Dungeon-01 kit (interior modular set, occluder-authored), 2 boss creatures, dungeon props | GRP-02 | ~70 kit pieces, 2 bosses, ~20 props |
| Crafting & gathering: 2 profession stations, gathering-node families (ore/herb, 3 tiers each), profession props/anims | ECO-02 | 2 stations, 6 node sets, ~10 props, 6 anims |
| Auction house / mail / bank NPC & bank-interior dressing | ECO-03/04/05 (support) | ~8 props |
| Ambient visual emitters (fireflies, dust, falls) paired with AUD-03 beds | AUD-03 | ~10 emitter systems |
| Group/party UI art, dungeon map art | GRP-01/02, UI-01 | ~20 sprites |

**M2 total: ~330 assets.** IT-M2 contribution: Dungeon-01 fully art-passed and 5-player-clear performant; economy props authored via editors where applicable (§8.3).

### M3 — Alpha World (months 15–24) — breadth

| Deliverable | Feature IDs | Estimate |
|---|---|---|
| Zone-02 art pass; Zone-03 + Zone-04 greybox→art (leaning on restyled Tier B/C at scale, §3.4) | WLD-01, WLD-03 | ~350 kit/foliage pieces, 12 POIs, 3 map paintings |
| Race 2 (2 sexes) + customization; classes 3–4 combat anim/VFX sets | CHR-01, CMB-01 | 2 characters, ~90 anims, ~30 VFX |
| Mob families for Zones 02–04 + Dungeon-02 kit & bosses | NPC-01, GRP-02 | ~20 creatures, ~60 kit pieces, 2 bosses |
| Ground mounts: 2 mount creatures (rig + mounted locomotion with Client track) | CHR-05 | 2 mounts, ~12 anims |
| Battleground set: 10v10 CTF map kit, 2 faction bases, flag/capture VFX, scoreboard UI art | PVP-02 | ~50 kit pieces, 6 VFX, UI set |
| Gear breadth: levels 1–30 across 4 classes, rarity language applied | ITM-01/03 | ~60 weapons, ~16 armor sets |
| Weather/day-night for all 4 zones | WLD-02 | 4× zone lighting/weather tuning |
| UI: guild/LFG/social panels skinned | UI-01 (support SOC-02/GRP-03) | ~30 sprites |
| Contributor kit: style-gate docs, templates, review-map, sample assets for community zone builders | TLS-08 (support) | 1 kit |

**M3 total: ~700 assets.** IT-M3 contribution: 4 zones hold min-spec budgets at 500 CCU server load; community-made zone built from our kits passes the style/perf gates (§8.3).

*(M4 is direction-only per the baseline: asset-scope set at end of M2; expect raid kit, localization-safe UI art, accessibility palette audit.)*

---

## 6. Integration with Tools (Forge-Ready Kits)

Zones are built in the zone editor — **Forge, now a Godot editor plugin suite per TD-08** (TLS-02) — by designers, not artists placing meshes by hand. Environment kits are therefore a **contract with the Tools track**, and that contract is unchanged by the engine pivot:

### 6.1 Kit contract (applies to every kit piece from M0 onward)

- **Grid:** pieces snap on a 50 cm master grid; architectural pieces on 1 m / 2 m / 4 m modules; wall heights 3 m / 4.5 m; door apertures standardized (1.5 × 2.5 m single).
- **Pivots:** consistent per category — floor pieces pivot at back-left-bottom corner; walls at bottom-left edge; props at bottom-center; cliffs/rocks at approximate ground-contact centroid. Documented in the art bible; validated by the import validator (§4.2).
- **Sockets/seams:** kit pieces of a family tile without gaps **at every LOD level** (vertex-snapped borders through LOD3); no z-fighting skirts.
- **Instancing:** shared material instances per kit; MultiMesh-safe; no per-instance unique textures.
- **Occlusion:** wall/cliff/building-class pieces ship occluder geometry (§2.2) so Forge-built scenes cull correctly without a manual pass.
- **Collision:** simple collision shapes authored (not auto-convex) so server-side movement validation (Server track) matches visuals.
- **Metadata:** each piece registered with its asset ID + category tags so TLS-02 palettes/browsers can filter (with Tools track).

### 6.2 Greybox → art-pass workflow (M1 → M2)

1. M1: art ships **greybox kits with final dimensions, pivots, grid, and collision** — flat-shaded, palette-coded by function. Designers build Zone-01 entirely from these in TLS-02.
2. Locked layout: at M1 exit, Zone-01 layout freezes for the art pass.
3. M2: final art pieces replace greybox **1:1 by asset ID** — same pivot, bounds, collision envelope — so the zone editor content (spawns, volumes, patrol paths from NPC-01/TLS-06) is untouched. Deviations require a Tools-track sign-off.
4. "Beautiful corner": Zone-01's art pass is the fidelity benchmark; its dressing density, material count, draw-call/instance profile, and perf captures become the budget template for Zones 02–04 — deliberately authored against the no-Nanite scene budget (§2.2), so later zones inherit an honest target.

---

## 7. Team & Workflow Reality (Open-Source Contributors)

### 7.1 Contributor model

- Small core (art lead + 1–2 senior generalists) owns: art bible, characters, master materials/shaders, kit contracts, and all style-gate reviews. Community contributors own: props, kit variants, foliage, icons, restyle passes on Tier B/C assets, VFX variants, and LOD-chain authoring on accepted LOD0 assets (a well-scoped entry-level task the pivot creates a lot of).
- Task granularity: GitHub issues sized ≤ 1 asset or ≤ 1 variant set, each linking the relevant bible page, budget row (§2), and a template `.blend`.
- Everything a contributor needs is in-repo: rigs, master materials, trim sheets, review map, import presets, validator. The pivot removes the last proprietary dependency on the critical path — Godot itself is MIT, so the entire toolchain from Blender to shipped client is open (Substance remains a recommended-not-required extra, §4.1).

### 7.2 Style-gate review

- Two-reviewer rule for style: art lead + one rotating deputy (deputies earn the role after 5 accepted assets). Checklist-driven (§8.2), verdicts in the PR, blind to source tier (§3.2).
- A public **style-gate gallery** page (accepted vs. revise examples with annotations) is maintained from M1 so contributors self-correct before submitting.
- Throughput assumption: core team ~10 finished assets/month + review capacity ~30 contributor assets/month at M1, scaling with deputies. M3 breadth (~700 assets) is only achievable with the restyle pipeline (§3.4) carrying ≥ 40% of environment volume — and with Megascans gone, restyle sources are thinner and LOD authoring adds per-asset cost, so this is a **higher-severity** tracked risk than in the UE plan (§10, R3/R8).

---

## 8. Testing & Acceptance

### 8.1 In-engine perf validation (per asset class, on min-spec)

- A **min-spec bench machine** (GTX 1060 6GB / 16 GB) runs weekly in-Godot captures of: the review map, Zone-01, and (from M2) Dungeon-01, at 1080p Low. Gate: 30 FPS with 33 ms frame / ≤ 2,500 draw calls / resident VRAM within §2.3. Metrics via Godot's profiler and `RenderingServer` viewport statistics (draw calls, primitives, video memory), captured by a bench script maintained with the Client track.
- Category-new assets (first boss, first weather system, etc.) get an individual bench capture (frame trace + VRAM report) attached to the PR.
- LOD verification: bench script sweeps camera distance bands and screenshots each LOD switch on category-new assets — pop artifacts at Low shadow/draw distances are a perf-gate failure, not a style note.
- Crowd test: automated 50-bot spawn scenario on the test realm (with Server/Client tracks) validates §2.5 budgets before each IT.

### 8.2 Style review checklist (the gate in §4.4/§7.2)

1. Silhouette reads at 25 m (checked at review-map camera preset).
2. Base color has painted value structure; no raw scan/AI albedo noise (15 m noise test).
3. Zone palette compliance; no reserved gameplay hues in ambient use.
4. Proportion sheet compliance (characters/creatures) or grid/pivot contract compliance (kits).
5. Parameter variant of an approved master material; texel density within band.
6. Reads correctly under all 4 lighting scenarios (day/night/overcast/interior) **and** under both GI paths — SDFGI (High) and baked-lightmap (Low); known SDFGI leak-prone setups (thin walls, floating kit pieces) flagged here.
7. Provenance record complete (license CC0/CC-BY verified); tier B/C restyle checklist done, LOD chain present.
8. No Blizzard-lookalike concern (any reviewer can raise; art lead adjudicates).

### 8.3 Integration-test contributions & done-criteria

| IT | Art contributes | Art "done" means |
|---|---|---|
| **IT-M0** | Base character visible/animating in the empty test map; test map dressed with the M0 starter kit; both moved through the full Blender→glTF→Godot pipeline | Two clients see each other's `art.char.human.male.base` move with the locomotion stub set at 30 FPS on min-spec; art bible approved; pipeline + import presets documented for contributors |
| **IT-M1** | All Zone-01 greybox kits, characters, mobs, gear, VFX, and HUD art used in the 10-quest chain | New player completes the chain to level 5 with zero placeholder-checker assets in the critical path; 50+ player town scene holds 30 FPS/1080p Low on the bench machine within the §2.5 draw-call ceiling; every asset authored/placed via TLS-02/data editors where applicable |
| **IT-M2** | Zone-01 full art pass, Dungeon-01 art, crafting/gathering visuals, rarity language v2, weather/day-night | 5-player Dungeon-01 clear at min-spec budget; gather→craft→auction→mail loop has final props/UI art; "beautiful corner" signed off as the game-wide fidelity benchmark with published budget template (incl. instance/draw-call profile) |
| **IT-M3** | 4 art-complete zones, 2 dungeons, battleground set, race 2, mounts, gear breadth, contributor kit | 500-CCU alpha holds min-spec targets in the heaviest zone hub; community-made zone built from released kits passes style + perf gates and loads on an unmodified server; provenance records exist for 100% of shipped assets, all CC0/CC-BY-clean |

---

## 9. Traceability Table

Every feature ID with Art marked ● in the baseline matrix (§4):

| Feature ID | Art deliverable(s) | Milestone |
|---|---|---|
| CHR-01 | Base bodies + pipeline-proof character (M0 stub); race 1 final + customization set (M1); race 2 + customization (M3) | M0 stub / M1, M3 |
| CHR-02 | Locomotion stub (M0); full locomotion set walk/run/jump/swim (M1) | M0 basic / M1 |
| CHR-05 | Ground-mount creatures + mounted locomotion sets | M3 |
| WLD-01 | Zone-01 starter kit + terrain material (M0); Zone-01 greybox kit (M1); Zone-01 art pass + Zone-02 greybox (M2); Zones 02–04 art-complete (M3) | M1 (→M3) |
| WLD-02 | Day/night lighting rigs (dual GI paths), weather VFX, time-of-day grading for Zone-01 (M2); all 4 zones (M3) | M2 |
| WLD-03 | POI landmark meshes + vista, world-map paintings, discovery UI art | M1 (→M3) |
| CMB-01 | Combat anim sets + spell/ability VFX for classes 1–2 (M1), classes 3–4 (M3) | M1 |
| CMB-03 | Death/resurrect anims, ghost-form material, graveyard props | M1 |
| CMB-04 | Buff/debuff VFX + aura icons (M1 basic); talent/proc VFX (M2) | M1 basic / M2 |
| NPC-01 | Zone-01 creature families + townsfolk costumes (M1); Zones 02–04 + dungeon creatures (M3) | M1 |
| ITM-01 | Weapon/armor visuals + loot/vendor props (M1); gear breadth levels 1–30 (M3) | M1 |
| ITM-03 | Rarity visual language v1 (M1) → v2 spec: color/glow/silhouette tiers + proc VFX (M2) | M2 |
| ECO-02 | Profession stations, gathering-node families, crafting props/anims | M2 |
| ECO-05 | Bank interior/NPC dressing (with ECO-03/04 support props) | M2 |
| GRP-02 | Dungeon-01 interior kit + bosses + props (M2); Dungeon-02 (M3) | M2 |
| PVP-02 | Battleground CTF kit, faction bases, capture VFX, scoreboard UI art | M3 |
| UI-01 | Core HUD art kit + ability icons (M1); social/LFG/guild panel skins (M3) | M1 |
| AUD-03 | Ambient visual emitters paired with ambient beds (fireflies, dust, waterfalls) | M2 |
| TLS-02 | Forge-ready kit contract (§6.1) for the Godot-editor-plugin Forge, greybox kits, kit metadata/palette registration | M1 |
| CHR-01/ACC-01 (○ consulted) | Character-select/login screen dressing — consulted with Client track | M0→M1 |

---

## 10. Risks & Open Questions

### Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | **AI-asset legal ambiguity.** Copyrightability and training-data liability for Tier B assets remain unsettled; CC-BY 4.0 relicensing of AI output is legally murky. | Distribution/licensing exposure (TD-09) | Provenance records with prompts + tools per asset (§3.3); mandatory human transformation pass (§3.2); ability to bulk-identify and replace Tier B assets by tier tag; keep Tier B out of brand-defining assets |
| R2 | **Style cohesion across three source tiers.** Mixed AI/CC0/original assets drift apart, producing an "asset-flip" look. | Brand-killing | Restyle checklist as a hard gate (§3.4); blind style review (§3.2); beautiful-corner benchmark (§6.2); master-material lockdown (§2.4) |
| R3 | **Contributor throughput.** M3 needs ~700 assets; volunteer output is spiky, review is the bottleneck, and the pivot raised per-asset cost (LOD chains everywhere, thinner restyle sources). | M3 slip | Deputy reviewer program (§7.2); small task granularity incl. standalone LOD-authoring tasks (§7.1); restyle pipeline for volume; scope valve: Zone-04 biome may reuse Zone-02/03 kit families recolored |
| R4 | **Min-spec vs. fidelity squeeze.** "Better than WoW" on a 1060 with heavy translucent VFX in 50-player fights. | IT-M1/IT-M3 failure | VFX particle/overdraw caps (§2.5, §5); custom crowd anim-tick throttling (§2.5, with Client track); weekly bench captures (§8.1); Low-tier baked-lightmap path tested at every style review (§8.2 item 6) |
| R5 | **Environment density ceiling without Nanite.** The visual bar "better than WoW" was budgeted assuming near-free static geometry; conventional LODs + a 2,500 draw-call Low ceiling could make zones read sparse or trigger visible LOD pop. | Direction credibility; IT-M2 "beautiful corner" underwhelms | Kit design for instanced density (MultiMesh scatter, trim-sheet variety, §2.2); authored occluders + per-zone draw-distance/visibility-range tuning as an art deliverable; imposters/billboards for far detail; beautiful corner built early against the honest §2.2 scene budget so expectations are set at M2, not discovered at M3 |
| R6 | **Greybox pivot-contract breakage** during the M2 art pass forcing zone rebuilds in TLS-02. | Tools/Art rework | 1:1 replacement rule with Tools sign-off for deviations (§6.2); import validator enforces pivots |
| R7 | **SDFGI light-leak quirks on the High tier.** SDFGI leaks light through thin geometry and can shift ambient response as probes update, which can invalidate style-review verdicts made under one GI path. | Review churn; High-tier zones look broken in exactly the showcase configuration | Minimum wall-thickness rule in the kit contract/art bible (§2.4); review checklist item 6 checks both GI paths; leak-prone kit configurations documented in the style-gate gallery; escalation path to Client track for SDFGI cell-size tuning per zone |
| R8 | **CC0 library gaps vs. Megascans quality/coverage.** PolyHaven/ambientCG/Kenney do not match the breadth or scan fidelity the UE plan leaned on for environment fill; some biome surface families simply won't exist off-the-shelf. | More Tier A/B workload than planned; M3 breadth risk compounding R3 | AI-assisted texture generation as the designated gap-filler (§3.2) feeding the restyle pass; painterly restyle reduces dependence on scan fidelity by design (§1.2); per-zone surface-needs audit at greybox time so gaps are known a milestone early; commission/original work reserved for irreplaceable hero surfaces |

### Open questions / gaps found in the baseline

1. **Zone naming beyond Zone-02.** The baseline names Zone-01 and Zone-02 only; M3 says "4 zones art-complete". This PRD assumes Zone-03/Zone-04 follow the same naming; biomes for Zones 02–04 are undefined — needed by early M2 for kit planning (and now also for the per-zone CC0 surface-gap audit, R8).
2. **Dungeon-02 and the battleground map have no canonical names** (Dungeon-01 is named in IT-M2). Requesting IDs in the baseline before M3 kit work starts.
3. **Ground mounts** — resolved since v0.1: baseline v0.2 added CHR-05 (ground mounts, M3, Art ●). Sized here as 2 mount creatures + mounted locomotion (§5 M3); confirm rider-rig approach (attachment vs. merged skeleton) with Client track before M3 starts.
4. **AUD-03 Art ●** is interpreted here as visual ambient emitters paired with audio beds (§5 M2). Confirm with Music track that this split (Music owns beds, Art owns paired visuals) matches their PRD.
5. **CHR-01 customization depth** (slider counts, how much appearance data the server persists) is unspecified; blend-shape budget (§4.2) needs a Client-track ceiling on per-character customization payload.
6. **Character-select/login screen art ownership** — ACC-01 marks Art ○ (consulted); this PRD budgets only dressing support. Confirm Client track owns the screen itself.
7. **UI-02 Lua addon API skinning:** whether community addons may reuse UI-01 art atlases (CC-BY makes it possible; needs an explicit policy line in the baseline or Client PRD).
8. **Baseline has no named art review authority** for the cross-track "definition of done"; this PRD assumes the art lead sign-off (§4.4) satisfies it — should be ratified at cross-track review.
9. **Godot terrain solution is undecided** (heightmap plugin vs. mesh-kit terrain vs. custom, no UE Landscape equivalent ships with the engine). The decision gate is the **A-09 Terrain3D spike at M0 exit** (Tools track owns it; Art co-signs the evaluation criteria per Tools PRD §13 Q4). The terrain material budget (§2.3) and 8-layer assumption feed those criteria — Zone-01 greybox terrain depends on the outcome.
