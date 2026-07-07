# Terrain3D spike (issue #132)

Empirical validation of the **A-09 "fork-and-vendor Terrain3D"** decision
(`docs/terrain-eval.md`, Sync Decisions §11) against a **real integration**
behind the `ITerrainBackend` seam (Tools SAD §5.2).

This is a **self-contained Godot project**, isolated from the game client boot
(`client/project/`). Vendoring/loading Terrain3D here never touches the client.

## Layout

```
project.godot                 self-contained spike project (Godot 4.7, Forward+)
seam/
  i_terrain_backend.gd        ITerrainBackend — the A-09 swap seam (Tools SAD §5.2)
  terrain3d_backend.gd        wraps the vendored Terrain3D fork behind the seam
  null_terrain_backend.gd     analytic backend (no Terrain3D) — proves swappability
spike/
  spike_harness.gd            headless validation + measurement harness (#283)
  spike_scene.tscn/_main.gd   windowed render scene + FPS overlay (visual + bench rig)
terrain3d_src/                vendored Terrain3D C++ fork (buildable). See PROVENANCE.md
addons/terrain_3d/            vendored Terrain3D GDScript addon + terrain.gdextension
                              (bin/ is gitignored — build_terrain3d.sh regenerates it)
build_terrain3d.sh            build the fork against the PINNED godot-cpp 4.7 commit
run_spike.sh                  run the headless harness
PROVENANCE.md                 upstream commit, MIT license, what was vendored/pruned
```

## Reproduce

```bash
# 1. Build the Terrain3D GDExtension against pinned godot-cpp 4.7 (~2-3 min).
./build_terrain3d.sh macos arm64          # or: linux x86_64 / windows x86_64

# 2. Run the headless harness (needs Godot 4.7 = client/ENGINE_VERSION).
GODOT=/Applications/Godot.app/Contents/MacOS/Godot ./run_spike.sh
```

The harness prints PASS/FAIL for each check and the measured numbers. Exit code
is non-zero if any check fails.

## What this spike establishes

See `docs/terrain3d-spike-report.md` for the full write-up (measured-here vs
needs-bench, and the adopt/fork-vs-build recommendation). Short version:
Terrain3D builds clean against Godot 4.7 (0 warnings), loads headless, and the
five `ITerrainBackend` operations work through the seam — with the render
frame-time number on min-spec left to the owner's GTX 1060 bench (#61 / TD-03).
