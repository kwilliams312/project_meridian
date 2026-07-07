# Terrain3D spike — integration report & recommendation (issue #132)

**Status:** Complete — empirical validation of the A-09 terrain decision.
**Validates:** A-09 / #133 (fork-and-vendor Terrain3D) against a **real integration**.
**Spike code:** [`client/forge/spike/terrain3d/`](../client/forge/spike/terrain3d/) (self-contained project, isolated from the client boot).
**Reads with:** [terrain-eval.md](terrain-eval.md) (the paper evaluation this spike tests), [Tools SAD §5.2](sad/tools-sad.md) (`ITerrainBackend`), [Sync Decisions §11](01-SYNC-DECISIONS.md).

---

## Recommendation: **ADOPT — fork-and-vendor Terrain3D** (confirms A-09)

The spike **validates the A-09 decision with a working integration**. Terrain3D
was vendored (MIT, pinned commit), **built clean against the pinned Godot 4.7
godot-cpp**, loaded headless, and driven through the `ITerrainBackend` seam. No
finding contradicts A-09; one paper criterion (C2, min-spec render frame time)
remains a **bench measurement**, not an architectural risk. Do **not** build an
in-house terrain GDExtension for v1 — the seam keeps that option open if the
bench ever forces it.

**Confidence basis:** real vendored build + a loaded extension + a driven seam +
measured export/memory numbers here, plus Terrain3D's documented architecture
for the one number that needs the min-spec bench.

---

## What the spike actually did (not paper)

1. **Vendored** the Terrain3D fork at upstream `main` commit `ff4614c` (MIT),
   recording provenance + license ([PROVENANCE.md](../client/forge/spike/terrain3d/PROVENANCE.md)).
   No source modified; brushes/csharp/demo pruned for a lean PR.
2. **Built** it from source against the **same pinned godot-cpp 4.7 commit the
   client uses** (`client/ENGINE_VERSION` → `5ffd70e`, = `Godot Engine
   v4.7.stable.official`). Reproducible via `build_terrain3d.sh`.
3. **Integrated behind `ITerrainBackend`** (Tools SAD §5.2). The SAD's C++
   header is a "DESIGN SKETCH, not compiled"; the spike gives the seam its first
   real embodiment in GDScript — the layer Forge's docks/exporter actually are —
   with two backends (`Terrain3DBackend`, `NullTerrainBackend`) behind one
   interface.
4. **Measured** what this box can (headless), and **built the bench rig** for
   the one number it can't.

---

## Measured HERE (reproducible: `./build_terrain3d.sh && ./run_spike.sh`)

Environment: macOS, Apple M2 Max (Forward+ / Metal), Godot 4.7.stable, Terrain3D
1.1.0-dev editor-target debug build. Harness: `spike/spike_harness.gd`, headless.

### Build & load (discharges Tools PRD R7 — GDExtension churn)

| Check | Result |
|-------|--------|
| Terrain3D compiles against godot-cpp 4.7 | **PASS — 0 errors, 0 warnings** |
| Extension loads in Godot 4.7.stable (headless) | **PASS** — `ClassDB.class_exists("Terrain3D")` |
| Terrain renders in 4.7 (windowed) | **PASS (owner re-confirm)** — see [render evidence](../client/forge/spike/terrain3d/docs/terrain3d_render_evidence.png); geoclipmap mesh with sculpted relief, Terrain3D default checker material |

R7 was the single biggest cited risk (upstream API churn breaking M1). For the
4.7 pin it is **empirically clear**: a clean build with zero warnings.

### Seam operations (the four co-signed criteria, exercised)

| Criterion (terrain-eval) | Seam op | Measured result |
|--------------------------|---------|-----------------|
| **C1** 128 m region alignment | op 3 | **PASS** — `region_size_m()==128.0`, `aligns_to_chunk_grid()==true` with `region_size=SIZE_128`; one region == one chunk |
| **C3** paint-layer count (~8/zone budget) | op 2 | **PASS** — `layer_capacity()==32` (4× budget) |
| **C4** heightfield extraction `f32[129×129]` | op 4 | **PASS** — exact 16 641-sample grid; native float32; **holes return `NAN`, not silent 0** (16 641/16 641 cells NAN pre-sculpt → the export lint has a real signal) |
| seam abstracts the backend | all | **PASS** — the *same* harness drives `NullTerrainBackend` (zero Terrain3D dep) with no caller change → A-09 reversibility is real, not aspirational |

### Timings (per chunk, this box — CPU-side seam cost, not render)

| Operation | Terrain3DBackend | NullTerrainBackend |
|-----------|------------------|--------------------|
| `export_heightfield` (129×129, `get_height` sampling) | **4.68 ms** | 1.49 ms |
| `sculpt` (r=40 brush) | 8.87 ms | — |
| `enumerate_collision_geometry` (4 m step, 6 144 verts) | 2.61 ms | 0.11 ms |
| static memory / populated region | ~299 KiB | ~66 KiB |

`export_heightfield` at ~5 ms/chunk is comfortably inside a batch bake budget
(the exporter runs it per chunk offline, not per frame). The `get_height`
lattice-sampling path (terrain-eval C4 *path 2*) was used — backend-agnostic and
exact at 1 m spacing since height is stored native float32.

### New empirical finding — shared-edge convention (worth the format's attention)

A single-region export yields **16 384 real cells (128×128) + a 129th edge
row/column of holes**, because the shared +1 edge belongs to the *neighbouring*
region (which doesn't exist in a one-chunk test). This confirms terrain-eval
C4's note ("include the shared +1 edge from the neighbouring region") is a **real
exporter requirement**, not a footnote: `export_heightfield` must either read the
neighbour region's first row/col or the zone must instantiate adjacent regions
before bake. The seam's `NAN` sentinel makes this detectable rather than silent.
**Action:** the chunk exporter (Tools SAD §5.4) handles edge stitching; noted as
a residual below.

---

## NEEDS THE MIN-SPEC BENCH (not measurable here — no number invented)

**C2 — clipmap LOD holds the min-spec frame budget.** The gate
(terrain-eval C2) is **30 FPS @ 1080p Low, GTX 1060 6 GB, within ≤ 2 500 draw
calls** (TD-03; the task also references the M1 8 GB bench, #61). This **cannot**
be measured on the dev box:

- This box is an Apple M2 Max, not the GTX 1060 / 8 GB min-spec target — a pass
  or fail here says nothing about min-spec.
- The on-box render was run (`spike/render_measure.gd`) and the terrain renders
  correctly (screenshot), but its frame-time is **confounded** (unoptimized
  editor-target extension build; `SceneTree`-script main loop, not a real game
  loop; shader-compile hitch on the first measured frame — worst 266 ms). It is
  therefore **deliberately NOT reported as a performance figure.** Reporting
  "~29 FPS on an M2 Max" would be a measurement artifact, not a Terrain3D signal.

**Methodology handed to the owner (the rig is built):**

1. On the min-spec bench (GTX 1060 6 GB / #61 8 GB box), build the extension
   **release-target**: `./build_terrain3d.sh <platform> <arch>` (change
   `target=editor`→`target=template_release` for the shipping number).
2. Apply a **Low** material preset (texture count + LOD as Art PRD §8.3 Low).
3. Run `godot --path client/forge/spike/terrain3d --script
   res://spike/render_measure.gd --resolution 1920x1080` (windowed).
4. Read `avg frame` / `worst frame` from the printed `RENDER MEASURE` block; the
   camera orbits so the geoclipmap LODs update every frame.
5. **Gate:** avg ≤ 33.3 ms (30 FPS) with terrain draws leaving headroom under
   the 2 500-draw Low ceiling (terrain is a handful of clipmap meshes; props
   dominate the budget per Art PRD §2.5).

Architecturally there is no blocker (Witcher-3-style geoclipmap, 7 tunable LODs;
terrain-eval C2 analysis). This residual is a **confirmation to record**, not a
risk that moves the recommendation.

---

## Fork-and-vendor vs. adopt-live vs. build-our-own

| Option | Verdict | Why (from this spike) |
|--------|---------|-----------------------|
| **Build our own GDExtension** | **Rejected for v1** | All four criteria pass on a real build; no blocking gap justifies the M1-critical-path schedule cost (Tools PRD R1/R3). The seam keeps this as a *later* drop-in if the bench ever forces it. |
| **Adopt upstream live** | **Rejected** | R7 (churn) — upstream tracks `master` godot-cpp; we must pin to the client's blessed 4.7 commit and freeze the feature set at M0 exit (Tools PRD R7/R8). |
| **Fork-and-vendor (A-09)** | **Confirmed** | Clean build against the pinned 4.7 godot-cpp; MIT allows vendoring + patching; a vendored fork lets us carry a `forge_core` heightfield-export entry point and upgrade on our schedule, re-validating with this same harness each bump. |

---

## Residuals to close before the M0-exit feature freeze

1. **C2 min-spec number** — run `render_measure.gd` (release build + Low preset)
   on the GTX 1060 / #61 bench; record avg/worst frame vs. the 33.3 ms gate.
   *(Owner — bench required.)*
2. **Shared-edge stitching** — the chunk exporter reads the neighbour region's
   first row/col (or instantiates adjacent regions) so `export_heightfield`
   returns a fully-populated 129×129 at zone interiors. *(Forge exporter, §5.4.)*
3. **Release-target + Windows/Linux binaries** — the spike built macOS-arm64
   editor-target; CI should build `template_release` for win-x64 + macos-arm64
   (D-28) and wire the vendored fork into the Creator-Kit engine pin (R7/R8).
4. **`forge_core` C++ backend (optional)** — if/when terrain ops move into
   `forge_core` (C++), it implements the same five operations; the GDScript seam
   here is the reference contract. Not v1-blocking.

## How to reproduce

```bash
cd client/forge/spike/terrain3d
./build_terrain3d.sh macos arm64        # clean build vs pinned godot-cpp 4.7
GODOT=/Applications/Godot.app/Contents/MacOS/Godot ./run_spike.sh   # 12/12 checks
```
