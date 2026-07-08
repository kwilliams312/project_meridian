<!-- SPDX-License-Identifier: Apache-2.0 -->
# Meridian client — Godot project

The Godot 4.7 client project (bootstrap skeleton, issue #158). Engine is pinned to
**4.7-stable** (`client/ENGINE_VERSION`).

> **Why this doc exists:** `project.godot` is an **editor-managed** file — Godot
> rewrites it to its canonical form on every editor import (including the one-time
> windowed `.godot` seed in `scripts/dev/run-client.sh`, the #283 workaround), which
> strips any hand-written comments. So the annotations that used to live in
> `project.godot` live here instead, where the editor can't clobber them.

## Boot flow

`run/main_scene` is the **login screen** (`res://scenes/login/login_screen.tscn`).
The client flow is **Boot/Login → Character Select → World**:

- **Login** (#99) — the first screen; on success it hands off to char-select.
  Requires the GDExtension binary (login uses `MeridianLogin`).
- **Character Select** (#110) — pure GDScript; picks/creates a character, then
  Enter World.
- **World** (`res://scenes/world/world.tscn`) — the networked scene: connects to
  worldd, renders the local player + remote entities over the AoI relay (#87).
  Player capsules are colored by class (#328, see
  `scenes/world/player_class_colors.gd`).

## Rendering

Forward+ per TD-02: **D3D12 on Windows, native Metal on macOS Apple Silicon**.
`config/features` carries `"Forward Plus"`, which makes `forward_plus` the default
rendering method (Godot omits the explicit `renderer/rendering_method` key as
redundant — this is expected, not a regression).

`textures/vram_compression/import_etc2_astc=true` is **required for the macOS arm64
export** (#113): Godot refuses an arm64/universal export unless the ETC2/ASTC VRAM
compression import path is enabled (arm64 is treated as a mobile-class texture
target). Benign for the desktop dev boot — it only widens which compressed texture
variants the importer emits.

## GDExtension

The client loads `libmeridian.macos.editor.framework` (editor/run-client variant)
or the `template_debug` variant (exported game). Build it with
`scripts/dev/build.sh --client`. See `scripts/dev/run-client.sh` for the local run
loop and the `.godot` seed.
