# Terrain3D Evaluation — A-09 Terrain Spike

**Status:** Complete — recommendation for the M0-exit terrain gate (Baseline §3 / Tools PRD §3.1, §9; SAD §5.2).
**Action item:** A-09 (Tools, due M0) — *evaluate Terrain3D (MIT): adopt / fork-and-vendor vs. build our own terrain GDExtension*.
**Discharges:** #133 (evaluation + decision) and #131 (co-signed evaluation criteria, below).
**Subject:** [Terrain3D](https://github.com/TokisanGames/Terrain3D) — a high-performance, editable terrain system for Godot 4. MIT-licensed, GDExtension-based, actively maintained by TokisanGames.
**Version referenced:** Terrain3D 1.0.x / 1.1.0 docs (readthedocs `stable` + `latest`), GitHub `main`.

---

## 0. Co-signed evaluation criteria (discharges #131)

The project owner co-signed these four criteria (Tools PRD §3.1, R1; open question #4) before the spike started. Art co-signs (2) and (3) (paint-layer/material needs, Art PRD §2.3); Client co-signs (2) and (4) (min-spec perf + server heightfield consumption). Terrain3D is scored against each; a **blocking gap on any one** flips the recommendation away from adopt/fork.

| # | Criterion | Bar | Co-signed by |
|---|-----------|-----|--------------|
| **C1** | **128 m region alignment** | Terrain3D's region/tile storage must align to Meridian's **128 m** chunk grid (Tools SAD §3.2, chunk size 128 m × 128 m; §5.2 "region alignment query (must tile on the 128 m grid)"). | Tools, Client |
| **C2** | **Clipmap LOD quality on min-spec** | The clipmap-mesh LOD must hold **30 FPS @ 1080p Low on a GTX 1060 6 GB** (TD-03) inside the ≤ 2,500 draw-call ceiling (Art PRD §2.5, §8.3). | Tools, Art, Client |
| **C3** | **Paint-layer count** | Must meet a zone's terrain material budget: **~8 texture layers per zone, ≤ 4 blended per chunk** (Art PRD §2.3). | Tools, Art |
| **C4** | **Heightfield extraction fidelity** | Must export the heightfield out of the plugin as the server's **129×129 @ 1 m float32 per chunk** (Tools SAD §3.2, §5.2 `export_heightfield(chunk) → f32[129×129]`) for movement validation (OPS-03) + navmesh bake input. | Tools, Client, Server |

**Method / limitation note.** Findings below come from Terrain3D's public documentation (readthedocs API + docs) and GitHub README, cited per finding. **Web research was available.** No hands-on min-spec benchmark was run in this spike — C2's frame-rate bar is confirmed against Terrain3D's *design* (documented LOD architecture and shipped-title precedent), not against a Meridian scene on the 1060 bench. That empirical confirmation is the M0 EditorPlugin-skeleton spike's job (Tools PRD §9, SAD §6 M0 column) and is listed as a residual to close before the feature-freeze; it does not change the recommendation because C2 has no architectural blocker.

---

## C1 — 128 m region alignment

**Findings.**
- Terrain3D is **region-based**. Regions are the unit of storage; the world is a sparse grid of fixed-size regions, and unused regions cost nothing (the "supports 64×64 m up to 65.5×65.5 km" range comes from tiling regions, not one giant map). — [Introduction](https://terrain3d.readthedocs.io/en/stable/docs/introduction.html), [README](https://github.com/TokisanGames/Terrain3D)
- `region_size` is a fixed enum with these exact values: `SIZE_64` (64 m), **`SIZE_128` (128 m)**, `SIZE_256` (256 m, default), `SIZE_512`, `SIZE_1024`, `SIZE_2048`. Region size is measured in metres and simultaneously in vertices/pixels on the map images (1 m/vertex at the default vertex spacing). — [class Terrain3D API](https://terrain3d.readthedocs.io/en/stable/api/class_terrain3d.html)
- Regions are addressed on an integer grid via `Vector2i region_location`; `get_region(region_location)` / `get_regionp(global_position)` resolve a region by grid index or world position. Region origins therefore fall on exact `region_size` multiples from the world origin. — [class Terrain3DData](https://terrain3d.readthedocs.io/en/stable/api/class_terrain3ddata.html)

**Analysis.** `SIZE_128` makes a Terrain3D region **exactly one Meridian chunk** (128 m × 128 m at 1 m/vertex → a 129×129 vertex grid per region including the shared edge, which is precisely the C4 heightfield shape). Region origins land on 128 m multiples, so with the zone origin snapped to the grid (SAD §3.2: "terrain bounds min-corner snapped to the grid"), Terrain3D regions and Meridian chunks are the *same* partition — no fractional straddling, no per-chunk stitching. The `ITerrainBackend` "region alignment query (must tile on the 128 m grid)" (SAD §5.2) is answered by pinning `region_size = SIZE_128`.

**Verdict: PASS.** Native 128 m region option is a direct, exact match. Not a blocker.

---

## C2 — Clipmap LOD quality on min-spec

**Findings.**
- Terrain3D uses a **Geomorphing Geometric Clipmap Mesh Terrain, "as used in *The Witcher 3*"**: several flat meshes, higher vertex density toward the camera and lower farther out, blended together in a circular pattern; the meshes follow the camera while the height data stays fixed in place. This gives automatic LOD. — [Introduction](https://terrain3d.readthedocs.io/en/stable/docs/introduction.html), [README](https://github.com/TokisanGames/Terrain3D)
- LOD count is configurable: `mesh_lods` **defaults to 7** ("the number of lods generated in the mesh"). `mesh_size` defaults to 48 ("Lod0 has 4*mesh_size + 2 quads per side"), so the near-field mesh density is tunable. — [class Terrain3D API](https://terrain3d.readthedocs.io/en/stable/api/class_terrain3d.html)
- The README positions Terrain3D as a **high-performance** system and it is the terrain used by shipping/commercial Godot 4 projects (TokisanGames' own title). The clipmap approach is GPU-cheap by construction: a small fixed set of meshes regardless of world size, height sampled from textures in the vertex shader.
- The terrain can be baked to static meshes via `bake_mesh()` (LOD 0–8, filterable) if a fully static proxy is ever wanted. — [class Terrain3D API](https://terrain3d.readthedocs.io/en/stable/api/class_terrain3d.html)

**Analysis.** The clipmap architecture is the *right* one for the min-spec target: it renders a bounded, LOD-graded mesh set that does not scale with terrain extent, and it is the same technique a AAA title (Witcher 3) shipped on far weaker hardware than a GTX 1060. Draw-call cost is a handful of meshes for the whole terrain — negligible against the ≤ 2,500-draw-call Low ceiling, which Art PRD §2.5 already notes is dominated by *props*, not terrain. `mesh_lods`/`mesh_size` give direct knobs to trade near-field tessellation for frame time on the 1060, and the SAD's per-chunk proxy-LOD ring scheme (§3.2 "beyond `far_ring`: unloaded, terrain clipmap only") already assumes exactly this — terrain is the cheap always-resident layer.

**Residual (not a blocker).** The 30 FPS/1080p-Low number is confirmed against architecture + shipped-title precedent, not a measured Meridian scene. Closing it is the M0 EditorPlugin-skeleton spike's empirical task on the GTX 1060 bench (Art PRD §8.3 bench script). No architectural risk was found that would make this fail; the tunables above are the mitigation if near-field cost needs trimming.

**Verdict: PASS (with an empirical residual to close at M0).** The LOD system is architecturally suited to min-spec and tunable. Not a blocker.

---

## C3 — Paint-layer count

**Findings.**
- Terrain3D supports **up to 32 texture layers** ("32 textures"). — [Introduction](https://terrain3d.readthedocs.io/en/stable/docs/introduction.html), [README](https://github.com/TokisanGames/Terrain3D)
- Per-vertex texturing is stored in a **Control map** (one of the three map types alongside Height and Color), which encodes base/overlay texture indices and a blend value — i.e. a small number of layers blend at any given point, with the full palette drawn from the 32-texture set. — [Importing & Exporting Data](https://terrain3d.readthedocs.io/en/stable/docs/import_export.html)

**Analysis.** The Art PRD §2.3 budget is **~8 layers per zone, ≤ 4 blended per chunk** (and the M1 greybox terrain set is 8 layers, Art PRD §8.2). Terrain3D's 32-texture ceiling is **4× the per-zone budget** — comfortable headroom, not a constraint. The "≤ 4 blended per chunk" budget is an authoring-side discipline Meridian enforces (via the `paint layer ↔ art.* terrain-set binding` in `ITerrainBackend`, SAD §5.2); Terrain3D's control-map blending does not force more than the budgeted number of layers to blend at a point.

**Verdict: PASS.** 32 layers vs. an ~8-layer budget — large headroom. Not a blocker.

---

## C4 — Heightfield extraction fidelity

**Findings.**
- Terrain3D has three map types that can be imported/exported: **Height (32-bit floats)**, Control, and Color. — [Importing & Exporting Data](https://terrain3d.readthedocs.io/en/stable/docs/import_export.html)
- Height export supports `exr` (32-bit float, recommended), `r16`/raw (16-bit uint), and png/jpg/webp (lossy, not recommended for height). The editor exporter reports image size and min/max heights on export. — [Importing & Exporting Data](https://terrain3d.readthedocs.io/en/stable/docs/import_export.html)
- **Programmatic API** (the path Forge/`forge_core` will actually use — not the editor exporter dialog):
  - `float get_height(global_position: Vector3) const` — interpolated height at a world position; returns `NAN` for holes / outside regions. — [class Terrain3DData](https://terrain3d.readthedocs.io/en/stable/api/class_terrain3ddata.html)
  - `Array[Image] get_height_maps()` and `Array[Image] get_maps(map_type)` — direct references to the per-region height-map `Image`s. — [class Terrain3DData](https://terrain3d.readthedocs.io/en/stable/api/class_terrain3ddata.html)
  - `Terrain3DRegion get_region(region_location)` / `get_regionp(global_position)` — fetch a region and read its height map `Image`, which the docs explicitly say you can "save it externally yourself." — [Importing & Exporting Data](https://terrain3d.readthedocs.io/en/stable/docs/import_export.html), [class Terrain3DData](https://terrain3d.readthedocs.io/en/stable/api/class_terrain3ddata.html)
  - `Vector3 get_normal(global_position)` and `get_mesh_vertex(lod, filter, global_position)` for derived surface data. — [class Terrain3D API](https://terrain3d.readthedocs.io/en/stable/api/class_terrain3d.html)

**Analysis.** The server needs **129×129 float32 @ 1 m per chunk** (SAD §3.2, §5.2). Two independent paths satisfy this exactly:
1. **Region-image path (preferred, exact).** With `region_size = SIZE_128`, a region's height map *is* a 128-per-side float image; reading the region `Image` (via `get_region`/`get_height_maps`) and including the shared +1 edge row/column from the neighbouring region yields the exact **129×129 float32** grid with no resampling — fidelity is bit-exact to what the terrain stores.
2. **Sampled path (robust, backend-agnostic).** Iterate the 129×129 lattice at 1 m spacing calling `get_height(global_position)`; produces the same array through the same API any backend can expose. Slightly more expensive, but backend-independent and the natural default for the `ITerrainBackend.export_heightfield` contract.

Either path produces `f32[129×129]`; float32 is Terrain3D's *native* height storage, so there is no precision loss. The `NAN`-for-holes contract also gives Forge a clean signal to reject un-sculpted/hole cells at export (a lint, not a silent zero).

**Verdict: PASS.** Native float32 height, region-image and per-position APIs both yield the exact 129×129 grid. Not a blocker.

---

## Scorecard

| Criterion | Bar | Terrain3D | Verdict |
|-----------|-----|-----------|---------|
| **C1** 128 m region alignment | region tiles on 128 m grid | `region_size = SIZE_128` native; region == chunk exactly | **PASS** |
| **C2** Clipmap LOD on min-spec | 30 FPS @ 1080p Low, GTX 1060, ≤ 2,500 draws | Witcher-3-style geoclipmap, 7 LODs, tunable; terrain draws are negligible | **PASS** (empirical residual) |
| **C3** Paint-layer count | ~8/zone, ≤ 4 blended/chunk | 32 textures — 4× headroom | **PASS** |
| **C4** Heightfield extraction | `f32[129×129]` per chunk | native float32 height; region-image or `get_height` sampling → exact grid | **PASS** |

**No criterion is a blocker.** All four pass; C2 carries an empirical residual (a bench measurement), not an architectural gap.

---

## Recommendation: **Fork-and-vendor Terrain3D**

Adopt Terrain3D, but **vendor a pinned fork** (a Meridian-owned copy at a fixed upstream commit) rather than depend on upstream releases live. Rationale:

1. **All four co-signed criteria pass** with margin (C1 exact, C3 4× headroom, C4 native float32, C2 architecturally sound). There is no blocking gap that would justify building our own terrain GDExtension from scratch — which Tools PRD R1/R3 flags as a direct schedule risk on the M1 critical path.
2. **Fork-and-vendor (not adopt-as-is)** because Tools PRD R1 and R7 (GDExtension/API churn across Godot minors) require that **upstream churn can't break M1**: the Creator Kit pins a blessed Godot version (Tools PRD R7), and terrain must be pinned to match. A vendored fork lets us (a) freeze the terrain feature set at M0 exit (PRD R1), (b) carry any small Meridian-specific patches — e.g. a `forge_core`-friendly heightfield-export entry point — without waiting on upstream, and (c) upgrade deliberately on our schedule, re-validating against these four criteria each bump. MIT licensing makes vendoring and patching unencumbered (consistent with D-17's open-source stance and the Creator Kit's redistributability, Tools PRD §7).
3. **The `ITerrainBackend` seam (SAD §5.2) keeps the decision reversible.** Forge's docks, chunk exporter, and navmesh bake never touch Terrain3D's API directly — they go through `ITerrainBackend`. If a future Godot version or an unforeseen min-spec finding ever makes Terrain3D untenable, an in-house GDExtension is a drop-in second implementation of the same interface, with no caller changes. The evaluation's PASS verdicts justify adopting *now*; the seam insures the *later*.

**Residuals to close before the M0-exit feature freeze:**
- **C2 empirical:** a Terrain3D scene on the GTX 1060 bench at 1080p Low holding 30 FPS within the draw-call ceiling (folds into the M0 EditorPlugin-skeleton spike + Art PRD §8.3 bench script).
- **Vendoring mechanics:** pin the upstream commit, record the Godot version it targets, and wire the fork into the Creator-Kit engine pin (Tools PRD R7/R8).
- **Export entry point:** confirm the region-image vs. `get_height`-sampling path for `export_heightfield` against the vendored build (C4 path 1 vs. 2) and land it behind `ITerrainBackend`.
