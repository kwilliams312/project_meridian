# Forge — EditorPlugin skeleton (#134)

**Forge** is the in-Godot terrain/world-building **EditorPlugin** (Tools SAD §5) —
distinct from the Avalonia **Codex** data tool. This directory is the **M0 de-risk
skeleton** that proves the plugin architecture end-to-end with the smallest real
slice: **one dock, one gizmoed custom Node3D, and one call into the `forge_core`
GDExtension** (SAD §8 M0 exit; PRD R3).

It is a **dedicated, isolated** Godot project so enabling/building Forge cannot
touch the game client (`client/project`). At M1, per SAD §5 ("ships inside the
game's Godot project"), the `meridian_forge` addon folds into the client project;
this standalone host keeps the M0 skeleton isolated.

## Layout

```
client/forge/project/                 # dedicated Forge editor-tool Godot project
  project.godot                       # meridian_forge plugin enabled
  forge_core.gdextension              # -> bin/ (gitignored build artifacts)
  forge_verify.gd                     # headless bridge verification (SceneTree)
  addons/meridian_forge/
    plugin.cfg / forge_plugin.gd      # EditorPlugin: registers dock + type + gizmo
    docks/zone_dock.gd                # the ONE dock — calls into forge_core
    nodes/forge_zone_marker.gd        # the ONE custom Node3D
    gizmos/forge_zone_marker_gizmo.gd # the ONE 3D gizmo (128 m bounds wireframe)
client/gdextension/forge_core/        # the forge_core GDExtension (C++20)
  src/                                # ForgeCore shim + engine-free cores
  test/                               # engine-free unit tests
```

`forge_core` links **only** godot-cpp (the client's pinned submodule) — no client
runtime deps — and packages as a macOS `.framework` per the #263 fix.

## Build `forge_core`

```bash
cd client/gdextension/forge_core
cmake -B build -DGODOTCPP_TARGET=editor
cmake --build build -j
# -> client/forge/project/bin/libforge_core.macos.editor.framework
```

(`git submodule update --init --recursive` first if `client/godot-cpp` is empty.)

## Engine-free unit tests (no Godot)

```bash
cd client/gdextension/forge_core
cmake -B build-tests -DFORGE_CORE_TESTS=ON && cmake --build build-tests -j
ctest --test-dir build-tests --output-on-failure   # forge-terrain-stub, forge-version
```

## Enable / verify in the editor

Open `client/forge/project` in the pinned Godot 4.7 editor. The plugin is already
enabled, so you should see the **Forge** dock (top-left), and adding a
**ForgeZoneMarker** node shows its 128 m zone-bounds wireframe gizmo. The dock's
"Call forge_core" button re-reads `ForgeCore.version()` + the terrain seam.

### Headless bridge check (macOS: seed windowed first, #283/#290)

The macOS `--headless` import aborts inside MoltenVK, so seed `.godot/` once with a
windowed import, then run the headless verifier:

```bash
GODOT=/Applications/Godot.app/Contents/MacOS/Godot
P=client/forge/project
"$GODOT" --rendering-driver metal --editor --quit --path "$P"   # one-time seed
"$GODOT" --headless --path "$P" --script res://forge_verify.gd  # asserts the bridge; exits 0/1
```

The dock UI and the viewport gizmo are **visual** — confirm those in the editor.
The GDExtension load, `ForgeCore` registration/call, and the terrain-alignment
seam are covered headlessly by `forge_verify.gd`.
