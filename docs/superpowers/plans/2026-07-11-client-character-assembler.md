# Client Character Assembler (②) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Orchestration:** each Task maps 1:1 to a GitHub story under the ② epic; subagents implement on branches from `origin/dev`, PR into `dev` (NEVER commit to dev), lead verifies (independent rerun + `gh pr checks` — CI green is a merge gate, #465 content-build is the sole allowed exception) and merges.

**Spec:** [2026-07-11-client-character-assembler-design.md](../specs/2026-07-11-client-character-assembler-design.md)

**Goal:** Replace capsules with assembled per-race characters — wire + worldd broadcast of appearance/equipment, client content access from mcc tables, one `AssembledCharacter` node used by world and char-select, proven with a two-client in-world run.

**Architecture:** Additive FlatBuffers fields on `EntityEnter` populated by `worldd`; a `MeridianContentDB` GDScript singleton reading mcc's per-type table blobs + by-id art resources; a scene-composition `AssembledCharacter` node (single `Skeleton3D`, bone-name binding, `BoneAttachment3D` sockets, geoset-name hides, material-param dyes); capsule fallback everywhere content is missing.

**Tech Stack:** FlatBuffers (`schema/net/world.fbs`), C++20 `worldd`, GDScript (Godot 4.7), mcc table blobs (`tools/mcc/FORMAT.md`), runtime glTF loading (`GLTFDocument`), existing `*_verify.gd` headless harness + two-bot E2E stack.

## Global Constraints

- FlatBuffers additive-only: append fields, never renumber. `EntityEnter` currently ends at `name:string` (schema/net/world.fbs:303-323); new fields append after it.
- Only ids travel: `item_template:uint32` (IF-9), `dye_id:uint32`, preset ids u8. All visual lookup client-side.
- Error rules verbatim from spec §6 / contract ① §9: missing body → class-color capsule fallback + one telemetry warning per asset id; unknown preset → catalog entry 1; missing worn model → hide piece + log; unknown dye → authored colors; payload without appearance → today's capsule path untouched.
- One assembly code path: world scene and char-select preview both use `AssembledCharacter` — no forked logic.
- NPC/creature payloads and rendering untouched. Crowd budgets/animations OUT (spec §8).
- M1 sex: wire reserves `sex:uint8 = 0` (0 = male); catalogs resolve `appearance.<race>.male`.
- Client scripts follow existing conventions: tabs, `class_name`, header comments citing issues/specs, `*_verify.gd` for headless checks (see client/project/hud/*.gd exemplars).
- Server work follows worldd patterns (world_dispatch.cpp payload builders, conformance corpus in server/libs/proto/conformance).
- Suites: `uv run pytest -q`, server ctest via `scripts/dev/test.sh [--db]`, and the client boot-smoke/E2E harness where invoked. All green pre-PR; `gh pr checks --watch` pasted.

---

### Task 1: Wire + worldd — EntityEnter appearance/equipment broadcast

**Files:**
- Modify: `schema/net/world.fbs` (append to `EntityEnter` at :323; add `DyeChoice`, `EquippedVisual` tables near `Appearance`)
- Modify: `server/worldd/world_state.cpp/.h` + the EntityEnter payload builder (locate: `probe search "EntityEnter payload build" server/worldd` — the builder that today sets `char_class`/vitals/name)
- Modify: `server/libs/proto/conformance/conformance.cpp` + `golden/` (+ manifest)
- Test: `server/worldd/test/` (extend the world-session/vitals test that asserts EntityEnter fields — find via `grep -rl "EntityEnter" server/worldd/test/`)

**Interfaces:**
- Produces (wire, binding for Task 4):

```fbs
table DyeChoice { channel:uint8; dye_id:uint32; }   // channel: 0=primary 1=secondary 2=accent
table EquippedVisual {
  slot:uint8;             // server's existing equipment-slot id
  item_template:uint32;   // IF-9 world id; slot omitted entirely when empty
  dyes:[DyeChoice];       // empty = authored colors
}
// EntityEnter appended fields:
//   race:uint8 = 0;               // roster id; 0 = unset (non-player entities)
//   sex:uint8 = 0;                // reserved; 0 = male (M1)
//   appearance:Appearance;        // existing table (contract ① T5); absent for NPCs
//   equipment:[EquippedVisual];   // visible slots only; absent for NPCs
```

- Consumes: character row (`race`, `appearance` JSON — parse via the existing `AppearanceRecord` helper from contract ① T5), equipment container + `item_instance.dyes` JSON.

- [ ] **Step 1: Failing worldd test** — extend the existing EntityEnter-shape test: a player entity with a saved appearance record `{v:1,hair:2,face:3,skin:1}` and an equipped item (template id from the test fixture pack, e.g. the boots/pickaxe already used by itm1 tests) with `dyes: {"primary":"core:dye.russet"→id}` produces an EntityEnter whose new fields round-trip those values; an NPC entity produces NO appearance/equipment (absent, not defaults).
- [ ] **Step 2: Verify FAIL** (fields don't exist), then **implement**: fbs append + codegen per schema/net/README.md; builder populates from the character/equipment state (visible slots only — reuse the server's slot-visibility set from the item rules; dye JSON `{channel: dye_content_id}` maps to numeric ids via the world DB dye table… VERIFY: dyes compile to the world DB via mcc (dye@1 is pck-only per #467 — the server has NO dye table). Therefore `item_instance.dyes` stores content ids; the WIRE carries `dye_id:uint32` = the IF-9 numeric id. worldd resolves content-id→numeric via the idmap-backed content store it already loads (db_content_store) — if dyes are absent from the world DB, store the NUMERIC id in `item_instance.dyes` at dye-application time instead; for M1 no dye-application path exists, so worldd sends `dyes` empty and the wire shape is exercised only by the conformance corpus + a worldd unit test with a hand-seeded instance row. Document which resolution you implemented.
- [ ] **Step 3: Conformance corpus** — EntityEnter with appearance+equipment, and the NPC-without case; regenerate goldens per the corpus README; flatc conformance green.
- [ ] **Step 4: GREEN** — affected worldd targets + full `scripts/dev/test.sh --db`; pytest; commit `feat(server): broadcast appearance + equipped visuals in EntityEnter (②/T1)`.

---

### Task 2: MeridianContentDB + catalog-driven char-create pickers (#477)

**Files:**
- Create: `client/project/content/content_db.gd` (autoload singleton `MeridianContentDB`)
- Modify: `client/project/project.godot` (autoload), `client/project/scenes/charselect/char_select.gd` + `character_appearance.gd` (pickers from catalog), `char_select_verify.gd` (extend)
- Possibly modify: `tools/mcc/src/stages/emit_pck.cpp` ONLY IF a needed type is missing from the client tables (see Step 1)
- Test: `client/project/content/content_db_verify.gd` (headless, run via the documented client verify invocation — see how hud_verify.gd / char_select_verify.gd are executed in CI or scripts)

**Interfaces:**
- Consumes: mcc pck artifacts — read `tools/mcc/FORMAT.md` FIRST: per-type table blobs at `res://meridian/<ns>/tables/<type>.bin` and by-id resources at `res://meridian/<ns>/<type>/<rest>.<ext>` (emit_pck.cpp:102-116).
- Produces (binding for Tasks 3/4):

```gdscript
class_name MeridianContentDB  # autoload "ContentDB"
func catalog(race: int, sex: int) -> Dictionary   # {} if missing; keys: body_model, skeleton, presets:{hair:[{id,model}],face:[...],skin:[...]}
func worn(item_template: int) -> Dictionary       # {} if none; keys: models:[{model,mirror}], hides:[], attach:{socket,sheath_socket}, dye_channels:[], race_overrides:{}
func dye_color(dye_id: int) -> Color              # authored-default sentinel Color(0,0,0,0) if unknown
func model_path(asset_id_or_numeric: int) -> String  # res:// path or "" if unknown
```

- [ ] **Step 1: Verify the substrate** — read FORMAT.md + emit_pck.cpp; confirm the item table blob carries `visual.worn`, and that `appearance`/`dye` types are emitted into client tables (they were classified in #467 as pck-only — confirm the table emission actually includes them; if a type is absent from the client tables, add it to emit_pck's table emission following the existing per-type pattern, regenerate mcc golden, and note the scope addition in the PR). Also confirm how the client currently mounts the pack (probe `load_resource_pack` usage under client/ excluding godot-cpp gen; the M0 stand-in may load tables from a build dir — match the boot path build-client.sh produces).
- [ ] **Step 2: Failing verify script** — `content_db_verify.gd`: catalog(1,0) returns the ardent catalog dict with `body_model` set; worn(<pickaxe numeric id>) has `attach.socket == "main_hand"`; dye_color(<russet id>) == the authored color; all misses return the documented empty sentinels.
- [ ] **Step 3: Implement + wire pickers** — parse the table blobs once at boot (lazy per type); charselect hair/face/skin pickers populate from `catalog()` preset lists (ids shown are the stable ints); missing catalog → pickers disabled + "content missing" label (spec §6).
- [ ] **Step 4: GREEN** — verify script passes headless; existing char_select_verify still green; commit `feat(client): MeridianContentDB + catalog-driven char-create pickers (②/T2, closes #477)`.

---

### Task 3: AssembledCharacter node

**Files:**
- Create: `client/project/characters/assembled_character.gd`
- Test: `client/project/characters/assembled_character_verify.gd`

**Interfaces:**
- Consumes: Task 2's `ContentDB` API verbatim; the committed blockout body (`geo_<region>_lod0` meshes, canonical `Skeleton3D`, `socket_*` bones) and pickaxe worn data.
- Produces (binding for Task 4):

```gdscript
class_name AssembledCharacter extends Node3D
func assemble(race: int, sex: int, appearance: Dictionary, equipment: Array) -> bool  # false = caller should use capsule fallback
func set_equipment_slot(slot: int, item_template: int, dyes: Array) -> void
func clear() -> void
signal assembly_failed(reason: String)  # emitted once per failing asset id
```

- [ ] **Step 1: Failing verify script** covering, with the real blockout+pickaxe content: body instanced (8 geoset nodes visible, skeleton has 63 bones); equip pickaxe → `BoneAttachment3D` on `socket_main_hand` with the weapon mesh child; a fixture armor dict with `hides:["feet"]` → `geo_feet_lod0.visible == false`; unequip → restored; unknown preset id → falls back to preset 1 (assert via which mesh/material got applied) without error; `assemble()` returns false on a race with no catalog; `race_overrides` fixture picks the override model path; incremental `set_equipment_slot` idempotent.
- [ ] **Step 2: Implement** — runtime glTF load via `GLTFDocument`/`GLTFState` from the ContentDB `model_path` bytes (the mcc pack ships source `.glb`; confirm extension in Step 1 of Task 2); re-parent skinned meshes onto the body `Skeleton3D` (set `skeleton` NodePath + verify `Skin` resource bone names resolve — bone-name binding is the contract); hides = union over equipped items; dyes = `set_instance_shader_parameter`/material albedo tint on gear surfaces (M1: whole-piece tint); geoset hide/show restore tracked per slot.
- [ ] **Step 3: GREEN headless**, commit `feat(client): AssembledCharacter — per-race body + worn gear assembly (②/T3)`.

---

### Task 4: World + char-select integration, two-client E2E proof

**Files:**
- Modify: `client/project/scenes/world/world.gd` (`_spawn_remote` at :529 and the local-player body path — assemble when EntityEnter carries appearance; capsule fallback per spec §6; nameplate/HUD untouched), the net codec (`client/net/.../codec.*` + gdextension net thread) to decode the new EntityEnter fields into the entity dict `d`
- Modify: `client/project/scenes/charselect/char_select.gd` (+ scene) — preview pane instantiates `AssembledCharacter`, re-assembles on picker change/roster selection
- Test: extend the world-scene verify + the two-bot E2E harness run
- Docs: `client/project/README.md` (assembler + fallback notes)

**Interfaces:**
- Consumes: Task 1 wire fields (exact names above), Task 3 node API verbatim.

- [ ] **Step 1: Codec first** — failing clientnet test (client/net/test/clientnet_test.cpp pattern): an EntityEnter buffer with appearance+equipment decodes into `d["race"]`, `d["appearance"]` (dict), `d["equipment"]` (array of {slot,item_template,dyes}); one WITHOUT them yields absent keys. Implement, GREEN.
- [ ] **Step 2: World wiring** — `_spawn_remote`: if `d.has("appearance")` → `AssembledCharacter.assemble(...)`; on `false`/failure → existing capsule (class color) — the fallback body IS the current code path, moved behind a helper; local player: same, seeded from char-select data at ENTER_WORLD as today. Extend the world verify script for both branches.
- [ ] **Step 3: Char-select preview** — replace the shared placeholder model with an `AssembledCharacter` driven by the pickers; roster selection re-assembles with that character's persisted appearance (already in the char-list wire from ① T5).
- [ ] **Step 4: E2E proof (definition of done)** — full local stack (`scripts/dev/run-local.sh` or the documented harness): two clients/bots in-world; capture evidence that each renders the other assembled (screenshot via the boot-smoke harness if headless rendering is available; otherwise assert the scene tree — remote node has AssembledCharacter child with 8 geosets + socket attachment — via the verify hooks). Paste evidence in the PR.
- [ ] **Step 5: GREEN everything** — client build, clientnet tests, verify scripts, server suites untouched-but-run (`scripts/dev/test.sh`), pytest; commit `feat(client): assembled characters in world + char-select preview, capsule fallback (②/T4)`.

---

## Task dependencies / story mapping

```
T1 (wire+worldd) ──────────┐
T2 (ContentDB, #477) ─► T3 (AssembledCharacter) ─► T4 (integration + E2E)
                            T1 ────────────────────┘
```

T1 ∥ T2 (disjoint: server/schema vs client). T3 after T2. T4 after T1+T3.

Out of scope (spec §8): crowd budgets/merge/LOD-throttle, animations, real content (⑤), sex column/catalogs (M2), NPC assemble path, dye acquisition/UI.
