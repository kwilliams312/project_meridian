# Terrain3D — vendoring provenance & clean-room record (issue #132)

This directory vendors a **fork** of [Terrain3D](https://github.com/TokisanGames/Terrain3D)
per the A-09 decision (fork-and-vendor; `docs/terrain-eval.md`, Sync Decisions §11).
This file records exactly what was taken, from where, and under what license, for
clean-room hygiene (Baseline: clean-room, Apache-2.0 project; MIT deps are
compatible when attributed).

## Upstream source

| Field | Value |
|-------|-------|
| Project | Terrain3D — high-performance editable terrain for Godot 4 |
| Upstream repo | https://github.com/TokisanGames/Terrain3D |
| Branch | `main` |
| Pinned commit | `ff4614c168e64a5dadf2e4822ac103d8a4fc6742` |
| Commit date | 2026-05-26 |
| Addon version | 1.1.0-dev (`addons/terrain_3d/plugin.cfg`) |
| License | **MIT** — `terrain3d_src/LICENSE.txt`, © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors |
| Authors | `terrain3d_src/AUTHORS.md` |

MIT is permissive and compatible with the project's Apache-2.0 licensing
(attribution retained: `LICENSE.txt` + `AUTHORS.md` are vendored verbatim).

## What was vendored (and what was pruned)

Vendored (the minimum to build + load + drive the seam):
- `terrain3d_src/src/`        — full C++ GDExtension source (48 files), verbatim.
- `terrain3d_src/doc/doc_classes/` — class docs XML (SConstruct embeds these).
- `terrain3d_src/SConstruct`  — upstream build script, verbatim.
- `terrain3d_src/LICENSE.txt`, `AUTHORS.md` — license + attribution.
- `addons/terrain_3d/`        — the GDScript editor plugin + `terrain.gdextension`,
                                MINUS the pruned dirs below.

Pruned to keep the spike PR lean (re-fetch from upstream commit above for full use):
- `project/addons/terrain_3d/brushes/` (~11 MB brush alpha textures — editor sculpt UI only).
- `project/addons/terrain_3d/csharp/`  (C# bindings — project is GDScript/C++).
- `project/demo/`                       (~24 MB demo assets — not needed for the seam).
- `godot-cpp` submodule                 (we build against the project's PINNED godot-cpp
                                          4.7 commit instead — see below).

No upstream source file was modified. This is a clean vendored drop; any
Meridian-specific patches (e.g. a `forge_core` heightfield-export entry point)
would land as tracked diffs on top and be recorded here.

## Build provenance (the R7 test)

The binary is built from `terrain3d_src/` against the **same pinned godot-cpp
commit the client uses** (`client/ENGINE_VERSION` → `GODOT_CPP_COMMIT =
5ffd70e34d0ab87009a9f0ffa3361bc8f4b09731`, which synced godot-cpp to
`Godot Engine v4.7.stable.official`). Reproduce with `./build_terrain3d.sh`.

**Result (measured here):** clean compile — **0 errors, 0 warnings** — and the
extension loads in Godot 4.7.stable. This directly discharges the Tools PRD R7
concern (GDExtension/API churn across Godot minors) for the 4.7 pin.

The compiled binary (`addons/terrain_3d/bin/`) is a build artifact and is
**gitignored** (same convention as `client/project/bin/libmeridian.*`). Rebuild
it with `build_terrain3d.sh` before running the spike.
