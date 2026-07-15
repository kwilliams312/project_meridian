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

## Assembled characters (②, #541)

Players (the local body and each remote) render as **assembled characters** — a
per-race body + worn gear built from the ids the wire carries — instead of the old
class-colored capsule. The one assembly node is
`characters/assembled_character.gd` (`AssembledCharacter`), used by BOTH the world
scene (`scenes/world/world.gd` — `_build_entity_body`) and the char-select preview
(`scenes/charselect/char_select.gd`). It resolves all visuals client-side from the
mounted mcc pack via `MeridianContentDB` (`content/content_db.gd`): only ids travel
(`race`, `sex`, appearance preset ids, `item_template`, `dye_id`) — every model /
catalog / dye lookup is local.

`EntityEnter` gained additive `race`/`sex`/`appearance`/`equipment` fields (②/T1);
the client codec decodes them (`client/net`), the GDExtension relays them into the
entity dict, and `world.gd` assembles when `d.has("appearance")`.

**Capsule fallback (spec §6 — never a crash).** The class-colored capsule
(`_build_capsule_body`, colored via `scenes/world/player_class_colors.gd`, #328)
remains the fallback body whenever assembly can't proceed:

- the frame carries **no appearance** (an NPC/creature, or a pre-#538 server) — the
  original capsule path, untouched;
- `assemble()` returns **false** — no catalog for the race, or an unloadable body
  model;
- individual content misses (unknown preset → catalog entry 1; missing worn model →
  the piece stays hidden and its geoset hides are **not** applied, so a body region
  is never left uncovered; unknown dye → the item's authored colors).

## Theme-driven pack mount (`MERIDIAN_REALM_THEME`, #791)

`MeridianContentDB` mounts the staged pack under `res://meridian/<theme>` and the
whole client resolves visuals from it. Which theme it mounts is selected by
`content/content_db.gd → resolve_pack_dir()`, in precedence order:

1. **`MERIDIAN_PACK_DIR`** — an explicit full pack directory (dev/CI pointing at a
   fresh `mcc emit-pck` build). Wins over everything.
2. **`MERIDIAN_REALM_THEME`** — the realm's content theme namespace →
   `res://meridian/<theme>`. This **mirrors worldd's `MERIDIAN_REALM_THEME`**
   (`server/worldd/world_boot` — the realm's primary `pack_namespace`), so the
   client mounts the same pack the connected realm serves: a chibi realm
   (`theme=chibi`) shows chibi. The value is sanitised to a single `[a-z0-9_]`
   segment (a `/`, `..`, or other unsafe value is rejected and falls back to core,
   so it can never escape `res://meridian/`).
3. **Default → `res://meridian/core`** (unchanged pre-#791 behaviour, back-compat).

The realm also advertises its content hash in the IF-2 `HandshakeOk` (`content_hash`)
for a later **fail-closed content-hash tie** at enter-world
(`MeridianPackMount.set_expected_content_hash` — `world.fbs HandshakeOk.content_hash`
already carries it). Until that handshake-driven verification is wired, the
`MERIDIAN_REALM_THEME` selector is the local-dev bridge for choosing which staged
pack to mount. `MeridianPackMount.mount_and_verify("res://meridian/<theme>")`
already resolves the mounted pack's `content_hash` + `content_version`
(`"<namespace>@<version>"`), so the hash tie is a thin follow-up.

Staged packs live under `client/project/meridian/<theme>/` (the `mcc emit-pck`
artifacts: `pack.manifest.json`, `pack.contents.jsonl`, `pack.data.json`). `core`
and `chibi` both ship. Headless proof (build the GDExtension first, then seed the
global-class cache with one `--import`):

```
scripts/dev/build-client.sh
godot --headless --path client/project --import
godot --headless --path client/project --script res://content/content_db_verify.gd        # core (baseline)
godot --headless --path client/project --script res://content/content_db_theme_verify.gd   # theme mount (#791)
```

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
