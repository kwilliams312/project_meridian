# Tools Track PRD — Forge & Codex

**Project:** Project Meridian (open-source WoW-style MMORPG)
**Track:** Tools (highest priority — critical path for M1 per Baseline §3)
**Version:** 0.2 — 2026-07-04 (revised for the engine pivot UE5 → Godot 4.6, Baseline v0.3; folds in sync decisions D-07/D-08/D-09)
**Baseline:** [00-GAME-DESIGN-BASELINE.md](../00-GAME-DESIGN-BASELINE.md) v0.3 (binding; all feature IDs, milestones, and TD-xx decisions reference it)
**Platform (TD-08):** Windows native x64

---

## 1. Overview & Goals

### 1.1 Tools-first rationale

Baseline Pillar 1 is the contract this track exists to fulfill: *"Zones, NPCs, mobs, items, and quests are authored in shipped, user-accessible tools. If the team can't build the world with the tools, the tools aren't done."*

This is not a slogan; it is the acceptance test for every deliverable in this PRD. The internal team is the first community. Every zone, mob, item, quest, loot table, vendor, and spawn in Zone-01 (M1) and beyond MUST be authored through Forge and Codex, with zero hand-written SQL and zero engine-side hardcoding. Any content that cannot be expressed through the tools is either (a) a tools gap to be fixed, or (b) out of scope for the game.

The engine pivot (Baseline v0.3, TD-01/TD-08) strengthens this contract: because the Godot editor is MIT-licensed, the *entire* creation stack — engine, editor, Forge, Codex, `mcc` — is now legally redistributable to community creators with no proprietary dependency. The old UE plugin distribution question is gone (§13).

Consequences we accept deliberately:

- Tools work is front-loaded. M0 ships editor alphas before the game has combat.
- Tools bugs block content. The content team dogfoods dailies; tools regressions are P0.
- Internal shortcuts are forbidden. There is no "internal-only" authoring path; a hack the community can't use doesn't exist.

### 1.2 Product surfaces

| Surface | What it is | Scope |
|---------|-----------|-------|
| **Forge** | Godot editor plugin suite (Godot 4.6+, per TD-01/TD-08) loaded into the Meridian client Godot project — EditorPlugin API, GDScript for UI/glue, C++ GDExtension for heavy operations (navmesh bake, terrain ops, chunk export) | Zone/world building: terrain, kit placement, spawns/patrols, volumes, navmesh, chunk-streaming export, day/night & weather authoring (TLS-02, TLS-06 spatial half) |
| **Codex** | Standalone Windows desktop data editor | All non-spatial game data: NPCs/mobs, items, quests, loot tables, spawn tables, vendors, recipes, talents, abilities/spells (TLS-03/04/05 + ITM-02, ECO-01/02, CHR-04, CMB-01 per D-08) |
| **`mcc`** (Meridian Content Compiler) | CLI, invoked by both apps, CI, and community | YAML content source → world-DB SQL + client `.pck` packs (TLS-01) |

Both Forge and Codex read/write the same Git-friendly YAML content source (TD-06) in `/content`; neither writes the database or packs directly — only `mcc` produces runtime artifacts. This single-funnel rule is what makes community packaging (TLS-08) possible later without a second code path.

### 1.3 Personas

1. **Internal world builder** ("Mara") — level designer on the team. Lives in Forge 6h/day; needs fast iteration (see §5 targets), reliable undo, and never wants to see YAML directly. Builds Zone-01 in M1.
2. **Community creator** ("Theo") — hobbyist with some Godot experience but no C++, no server knowledge. Downloads the Meridian Creator Kit at M3, follows the tutorial, builds a small zone with 20 quests, publishes a content pack. Success = his zone loads on someone else's unmodified server (IT-M3).
3. **Server operator** ("Ines") — runs a community realm on a Linux VPS (TD-04). Never opens Forge. Consumes `mcc` output and content packs; needs deterministic builds, clear validation errors, and safe pack install/uninstall. Also uses GM tooling (OPS-02).

### 1.4 Goals / non-goals

**Goals:** everything in §2–§9. **Non-goals for 1.0:** in-game building tools, Linux/macOS editor builds (TD-08; Godot makes this cheap later but it is not promised), scripting language for content beyond the data-driven schemas + the client Lua addon API (UI-02, Client track owns runtime; Tools owns packaging), collaborative real-time co-editing (Git is the collaboration layer), asset-creation DCC tooling (Art track pipeline).

---

## 2. Content Pipeline Architecture

### 2.1 YAML content-source format (TD-06)

`/content` is the single source of truth for all game data (Baseline §5.2). Design rules:

**Stable IDs.** Every entity has a permanent, human-readable string ID, namespaced by type and pack:

```yaml
# /content/base/npcs/zone01/innkeeper_thessaly.npc.yaml
id: npc.base.zone01.innkeeper_thessaly
schema: npc/2          # schema name/version, resolved against /schema
name: "Innkeeper Thessaly"
level: { min: 8, max: 8 }
faction: faction.base.merchant_guild
model: art.char.human.female.innkeeper01   # asset ID, never a file path (Baseline §5.3)
vendor: vendor.base.zone01.thessaly_inn
gossip: gossip.base.zone01.thessaly
loot: loot.base.humanoid.town_trash
```

- IDs are never renamed after first release; a `deprecated: true` + `superseded_by:` field handles retirement. `mcc` assigns stable numeric runtime IDs via a committed `idmap.lock` file so server DB keys never shuffle between builds.
- **References by ID only.** Cross-references (loot links, quest prerequisites, vendor items, music regions → `mus.*` asset IDs, ambience volumes → `sfx.*`/`amb.*` IDs) always use asset/entity IDs per Baseline §5.3. File paths are forbidden in content files; `mcc` resolves IDs through the asset ID registry and fails the build on unknown IDs.
- **Git diff-ability.** One entity per file (small entities like loot tables may batch per zone/category, ≤ ~50 per file); canonical key order enforced by a formatter (`mcc fmt`) so diffs are semantic, not cosmetic; no timestamps/machine-specific data in source files; deterministic sorting of lists where order is not meaningful. Directory layout mirrors packs: `/content/<pack>/<type>/<zone-or-category>/...`.

**Spatial data exception.** Forge-authored zone data (terrain heightfields, kit placements) lives in Godot scenes (`.tscn`) and terrain resource files under `/client` (LFS per TD-12) because YAML cannot sanely represent it. However, everything the *server* needs from a zone — spawn points, patrol paths, volumes, navmesh — is **exported by Forge into YAML/binary sidecar files in `/content`** so the server never reads Godot assets. Placements reference kit pieces by asset ID.

### 2.2 Content compiler `mcc` (TLS-01)

One compiler, two outputs (TD-06).

**Inputs:** `/content` tree (one or more packs), `/schema` content schemas, asset ID registry, `idmap.lock`.

**Outputs:**
- **Server side:** world-DB SQL (MariaDB/MySQL per TD-05) — either full-dump `.sql` or incremental migration scripts, plus compiled navmesh tiles and spawn/patrol binary blobs `worldd` loads directly.
- **Client side:** Godot `.pck` resource packs (compiled data tables for items/quests/NPC display data, POI/map data, localization strings, and zone chunk data per §2.4). Client-facing subset only — drop rates, loot table contents, and AI internals are server-only and never enter `.pck` files (anti-datamining posture; supports OPS-03's server-is-law stance).

**Determinism.** Same input tree + same `mcc` version ⇒ byte-identical SQL and content-identical `.pck` packs (pack container timestamps normalized; `mcc` writes `.pck` files itself rather than shelling out to the Godot export pipeline, precisely to control byte layout). Verified in CI by double-build + hash compare. Determinism is a hard requirement: it underpins content CI (TLS-07), nightly test-realm deploys (Baseline §6), and community pack verification (TLS-08).

**Incremental builds.** Content-hash-based build cache keyed by (file digest, schema version, compiler version). Editing one item recompiles that item's outputs and the indexes that reference it; target <2s for a single-entity rebuild (see §5). `mcc build --full` always available and is what CI/release uses.

**CLI surface (v1):** `mcc build | check | fmt | diff <buildA> <buildB> | pack | migrate | idmap verify`.

**Implementation:** C++20 static-linked CLI (shares schema-generated code with server per Baseline §5.1), so `mcc` runs on both the Windows tools machines and the Linux server/CI (TD-04) from one codebase — with zero Godot editor dependency.

### 2.3 Schema evolution & versioning (`/schema`)

- Content schemas live in `/schema/content/` alongside the network schemas (Baseline §5.1); changes require client+server+tools sign-off per that contract.
- Every content file declares `schema: <type>/<version>`. `mcc` ships migrators for N-1 → N (`mcc migrate` rewrites YAML in place, producing a reviewable Git diff). Editors always write the newest version.
- Compatibility window: `mcc` vX reads schema versions from the current and previous milestone. Older packs must be migrated (tooling provided) — we do not carry unbounded legacy readers.
- Schema documentation is generated from the schema definitions (field, type, constraints, since-version) and published with the tools — this is the community's data reference (see §10).

### 2.4 Zone chunk export format — cross-track contract (WLD-01)

Godot has no World Partition equivalent; Baseline WLD-01 specifies **custom chunk streaming**, and the split of ownership is a formal cross-track contract:

- **Forge (Tools) owns the chunk export format.** Forge partitions a zone into a Meridian streaming grid and exports per-chunk artifacts: geometry/kit-placement manifests (asset IDs + transforms), terrain tile references, chunk-local gameplay markup indexes, and a zone chunk manifest (grid dimensions, chunk hashes, dependency lists). The format is versioned in `/schema` like any other contract.
- **Client track owns the runtime streamer** that consumes this format (load/unload by camera position, budget enforcement at runtime). A format change requires Tools+Client sign-off; Server also consumes the chunk cell metadata for interest management.
- Forge validates streaming-cell budgets (node counts, LFS asset sizes per chunk) against Client-track streaming budgets at export time, so budget violations are caught in the editor, not on min-spec hardware.

This contract is the single riskiest interface introduced by the engine pivot and is exercised end-to-end at IT-M1 (Zone-01 streams through it).

---

## 3. Forge — Zone Editor (TLS-02)

Godot editor plugin suite (per TD-08) providing a "Meridian mode" toolset inside the stock Godot 4.6 editor: an `EditorPlugin` set with custom docks, 3D gizmos, and inspector plugins. GDScript for UI/glue; C++ GDExtension (`forge_core`) for heavy operations — terrain ops, navmesh bake, chunk export. Ships to the team at M1 and to the community at M3 as part of the Meridian Creator Kit (§7) — fully redistributable, since both the Godot editor (MIT) and Forge (Apache-2.0) are open source.

### 3.1 Terrain & modular kits

- **Terrain sculpt/paint:** Godot has **no built-in heightfield terrain**, so Meridian commits to a terrain GDExtension. Decision gate at M0 exit: **evaluate Terrain3D** (MIT-licensed, GDExtension-based, actively maintained) for adopt-or-fork vs. building our own; the recommendation is to adopt/fork Terrain3D unless the evaluation finds blocking gaps (clipmap LOD quality on min-spec, paint-layer count, region streaming alignment with §2.4 chunks). Whichever path wins, the Meridian layer on top is the same: paint layers map to Art-track terrain sets by asset ID, zone bounds are enforced, and Forge exports a server-side heightfield for movement validation (OPS-03 consulted). Terrain tiles must partition on the §2.4 chunk grid.
- **Modular kit placement:** palette browser fed by the asset ID registry (Art track publishes kits, e.g. `art.kit.town.human01.*`). Snapping, randomized scatter brushes for foliage/rocks, and per-placement gameplay flags (collision-relevant vs. dressing-only). Placements are Godot nodes serialized with kit asset IDs (a `KitInstance` node type resolving IDs to scenes at edit/load time) so a zone survives kit re-exports.

### 3.2 Gameplay markup

Spawns, volumes, and patrols are authored as **custom Godot node types with editor gizmos** (spawn points, volume shapes, patrol path curves) — native undo/redo, multi-select, and scene-tree organization come free from the editor; Forge adds the Meridian semantics and the `/content` export.

- **Spawn & patrol placement (NPC-01):** place spawn-point nodes bound to Codex spawn-table IDs (Forge never defines *what* spawns, only *where*); draw patrol paths as curve nodes with per-node wait times/emotes; leash-radius gizmo visualization per CMB-02 leash params; respawn-timer overrides per point. Exported to `/content` for the server.
- **Volumes:** leash-boundary volumes (CMB-02), ambience volumes → `amb.*` asset IDs (AUD-03), music regions → `mus.*` IDs with layer hints for the adaptive system (AUD-02, TD-11), discovery/POI volumes with map pin + discovery-XP data (WLD-03), plus graveyard/resurrect regions (CMB-03, consulted) and instance-entrance markers (GRP-02, M2).
- **Navmesh generation:** **standalone Recast** bake (in `forge_core` and `mcc`) producing **server-consumed** navmesh tiles (`worldd` AI pathing for CMB-02). We deliberately do *not* use Godot's NavigationServer bake: the server and CI must be able to bake headlessly on Linux without any Godot editor or engine runtime in the loop, and `worldd`'s runtime pathing must query the exact same Recast/Detour data structures the bake produced. A single vendored Recast codebase shared between `forge_core`, `mcc`, and `worldd` gives bit-identical tiles from editor and CI, keeps the unreachable-spawn lint (TLS-07) authoritative, and avoids coupling server pathing correctness to Godot version behavior. Bake runs in-editor with error surfacing (unreachable spawn points flagged live, mirroring the TLS-07 lint) and in CI for the full zone.

### 3.3 World systems authoring

- **Chunk streaming integration (WLD-01):** zones are authored against the Meridian chunk grid (§2.4) with grid presets per zone type; Forge validates streaming-cell budgets (node counts, LFS asset sizes) against Client-track streaming budgets and performs the chunk export — Forge owns the format, Client owns the runtime streamer, Server consumes cell metadata for interest management (contract in `/schema`, see §2.4).
- **Day/night & weather (WLD-02, M2):** curve-based authoring of a zone's 24h lighting script and weather-state table (states, transition times, weights); preview scrubber in-editor (driving the Godot `WorldEnvironment`/lighting setup); exported as content data so `worldd` drives time/weather authoritatively and clients render it.

### 3.4 Greybox (M1) vs. art-pass (M2+) workflow

- **M1 greybox:** blockout kit (`art.kit.greybox.*`, Art track supplies at M0), terrain + markup + navmesh fully functional, no visual polish gates. Definition of done for greybox = IT-M1 playability, not looks.
- **M2 art-pass:** kit-swap workflow — greybox placements are re-pointed to final kits via an asset-ID remap table (greybox ID → final ID) so gameplay markup (spawns, volumes, navmesh inputs) survives the art pass untouched. Lighting/weather authoring (WLD-02) and ambience/music volume polish (AUD-02/03) land in this phase; per TD-02 there is no Nanite equivalent, so the art pass also re-validates chunk budgets and LOD discipline at export. Zone-01 art pass at M2 is the proving run.

---

## 4. Codex — Data Editors

Standalone Windows x64 desktop app (TD-08). Unaffected by the engine pivot by design: Codex has zero engine dependency.

**Tech stack (recommendation):** **C# / .NET 8 LTS + Avalonia UI 11**, MVVM, with `mcc` invoked as a subprocess for compile/validate and a shared C ABI library (`libmccore`) for schema-driven form generation and live validation. Rationale: (a) C#/XAML is the fastest path to rich desktop forms + a node-graph canvas (quest editor) without Electron's memory/footprint and Chromium supply chain; (b) Avalonia over WPF because it is MIT-licensed and fully open-source — matching Apache-2.0 project licensing (TD-09) and friendlier to community contributors than Windows-only WPF, while still shipping Windows-native x64 per TD-08 (cross-platform later is free but not promised); (c) mature ecosystem: AvaloniaEdit for YAML views, community node-graph controls for QST graphs. Electron rejected (footprint, native interop friction with `libmccore`); C++/Qt rejected (LGPL friction, slower UI iteration).

**Shared Codex behaviors:** every editor is a schema-driven form over YAML files (Git stays the merge/versioning layer — Codex shows file status and diffs but never wraps Git workflows); reference fields are typed pickers backed by the project-wide ID index (with "find usages"/backlink navigation); inline validation runs the same TLS-07 lint rules live; every save runs `mcc fmt`; undo/redo per document.

### 4.1 NPC/Mob editor (TLS-03) — M0 alpha / M1

Field-level scope:
- **Identity/display:** stable ID, name/subname, model + scale (asset ID), faction, level range, rank (normal/elite/boss), classification flags (beast/humanoid/undead...).
- **Stats (CHR-03 alignment):** HP/resource, damage min–max, attack speed, armor, resistances — either explicit or derived from a level-scaling curve table (curves themselves are Codex-editable data, shared with Server track's CHR-03 implementation).
- **AI params (CMB-02):** aggro radius (+ level-delta modifier), threat table config, leash distance & behavior, respawn time (+ variance), call-for-help radius, ability rotation (ability ID + priority/cooldown/HP-threshold conditions — data-driven, references CMB-01/CMB-04 ability and aura definitions), movement type (stationary/wander-radius/patrol-path ref).
- **Ability/spell data forms (CMB-01):** confirmed Codex scope under TLS-03 per sync decision **D-08** — ability definitions (cast/instant, cost, range, GCD class, effects, aura refs) are authored here alongside the NPC rotations and player trainer data that reference them (was open question #1 in v0.1; now resolved, no new feature ID needed).
- **Roles (NPC-02):** gossip tree (text nodes, options, conditions on quest state/level/faction), vendor link (→ §4.5), trainer data (trainable abilities/spells with level+cost, feeds CHR-03/CHR-04), quest-giver flag (quest links are authored on quests, displayed here as backlinks).
- **Loot links (ITM-02):** loot-table reference + money drop range; pickpocket/skinning table refs reserved for M2 (ECO-02 gathering).

### 4.2 Item editor (TLS-04) — M0 alpha / M1

- **Core:** stable ID, name, icon + model asset IDs, item class/subclass (weapon/armor/consumable/quest/trade-good), equip slot, bind rules (BoP/BoE/none), stack size, sell/buy price (ECO-01), required level, durability.
- **Combat data:** weapon damage min–max + speed, armor value, stat blocks (primary/secondary stats), on-use/on-equip/proc effects by ability ID (CMB-04 auras).
- **Rarity & budget (ITM-03, M2):** rarity tier; the itemization model (stat-point budget per item level × slot × rarity, defined with the Server track as shared data in `/content/base/rules/`) drives a live budget meter in the editor — over/under budget is a warning at M1, a hard TLS-07 CI failure once ITM-03 lands at M2. "Auto-fill to budget" helper distributes remaining points by a chosen stat weight profile.

### 4.3 Quest editor (TLS-05) — M1

Node/graph-based canvas (QST-01/QST-02):
- **Quest node fields:** ID, title, description/objective/completion text, level + required level, quest-giver NPC ref, turn-in NPC ref, shareable flag, repeatable/daily flags (data present at M1; server support per Server PRD).
- **Objectives (QST-01):** kill (NPC ref + count), collect (item ref + count, with drop-source hint linking into loot tables), deliver (item ref + target NPC); multiple objectives per quest; M2 adds scripted-event and exploration (WLD-03 POI ref) objective types per QST-02.
- **Chains & prerequisites (QST-02):** graph edges = prerequisites (quest completed, level, class/race, faction standing); branch/exclusive-group support (choose-one-of); chain visualization across zones with cycle detection (a TLS-07 lint, surfaced live on the canvas).
- **Rewards:** XP (or auto from level curve), money, fixed item rewards, choose-one item rewards, faction standing; reward-value sanity lint vs. quest level (pairs with ITM-03 budgets).
- **Simulation panel:** "walk the chain" — pick a character level/class and step through the chain to catch dead ends before ever hitting the test realm.

### 4.4 Loot-table editor (ITM-02) — M1

- Table = stable ID + entries: item ref (or nested table ref, one level of nesting v1), drop chance, min–max count, condition flags (quest-active-only, class-restricted).
- Groups within a table (roll one item from group) for exclusive drops.
- **Probability preview:** expected drops per N kills, and a seeded 1000-kill simulation button.
- Backlinks: every NPC/object/quest referencing this table. Orphan tables and dead item refs are TLS-07 lints.

### 4.5 Spawn tables, vendors, recipes, talents

- **Spawn tables — M1:** spawn-table ID → weighted NPC entries, time-of-day/weather conditions (data reserved until WLD-02 at M2), pool sizes (max concurrent from this table). Forge spawn *points* reference these tables (§3.2) — clean split between "what" (Codex) and "where" (Forge).
- **Vendor inventories (ECO-01) — M1:** vendor ID → item entries: price override (default from item), limited-stock count + restock timer, purchase conditions (level/faction/quest). Buyback/repair behavior is server logic; the repair-capable flag is set here.
- **Crafting recipes (ECO-02) — M2:** recipe ID, profession + skill requirement, skill-up color thresholds, reagent list (item ref + count), product (item ref + count, proc-extra chance), craft time, source (trainer ref / drop / vendor). Gathering-node tables (herb/ore spawn data) reuse spawn tables + loot tables.
- **Talent data (CHR-04) — M2:** per class: tree layout (tier/column grid), talent nodes (ID, max ranks, per-rank effects by ability/aura ref or stat mod), prerequisite edges, points-per-tier gating. Grid-canvas editor with the same graph validation approach as quests. Balancing rules stay in shared `/content/base/rules/` data with the Server track.
- **Auction house (ECO-03) — M2:** Codex scope is limited to the AH category/search taxonomy data (item class/subclass mapping); runtime is Server/Client.

---

## 5. Live Preview Loop (TLS-06)

The edit→see cycle is the tools track's core UX metric. Architecture: local **dev-realm profile** — a Dockerized `worldd`+`authd`+MariaDB (from Server track's OPS-01 images) run via WSL2/Docker Desktop on the creator's machine, or a shared dev server; Codex/Forge target it via a `dev-realm.toml` config.

**Loop mechanics:**
1. Save in Codex/Forge → incremental `mcc build` of the affected entities (§2.2).
2. `mcc` pushes deltas: SQL applied to the dev realm's world DB + a `reload content` RPC to `worldd` (Server track has **committed** this hot-reload endpoint per sync decision **D-07** — reloadable-table scope defined in the Server PRD for M2; gated to GM-level auth per OPS-02 permissions); client-side data deltas served to a dev Godot client via an editor-mode loose-resource mount over `res://` (no `.pck` rebuild in the inner loop; `.pck` files are for CI/release).
3. Creator sees the change on their connected Godot client — the dev-preview map join is a Client-track feature of the Godot client (dev-realm connect + loose-mount mode). Forge additionally supports one-click "teleport my dev character here."

**Iteration-time targets (measured, tracked as tools-CI perf tests) — unchanged by the engine pivot:**

| Change type | Target (save → visible in client) |
|---|---|
| Data edit (item stat, loot entry, quest text, vendor price) | **< 10 s** |
| NPC/spawn change requiring respawn | **< 30 s** |
| Zone markup (volumes, spawn points, patrols) | **< 30 s** |
| Zone geometry (terrain/kit edit incl. local navmesh re-bake of dirty tiles) | **< 2 min** |
| Full zone build from clean (CI path) | < 15 min |

Milestone phasing: M1 ships a semi-live loop (save → auto-compile → manual `worldd` content-reload command; targets ×2). TLS-06 proper — the fully automatic loop with in-Forge spawn preview showing live server-side mob positions overlaid in the editor viewport (an `EditorPlugin` overlay reading a dev-realm telemetry stream) — lands at M2 per the baseline matrix.

---

## 6. Validation & Content CI (TLS-07) — M1

Tools track **owns content CI** (Baseline §6): every merge to `/content` is compiled and validated before deploy.

### 6.1 Lint rules (`mcc check`)

Severity: **error** (blocks merge) / **warning** (report). Initial ruleset:

| Rule | Severity | Notes |
|---|---|---|
| Broken reference (any ID → nonexistent entity/asset) | error | includes asset-ID registry misses |
| Schema violation (missing/invalid field, unknown version) | error | |
| Duplicate or renamed stable ID / `idmap.lock` drift | error | |
| Orphan quest (unreachable: no giver, or prerequisite cycle/dead-end) | error | uses the QST graph walker from §4.3 |
| Unreachable spawn point (off-navmesh / outside zone bounds) | error | needs navmesh artifact; runs in zone-build CI stage |
| Loot table: probabilities invalid, empty group, orphan table | error / warn (orphan) | |
| Stat-budget violation (ITM-03) | warn at M1 → error from M2 | |
| Kill/collect objective target drops/spawns nowhere | error | cross-checks loot + spawn tables |
| Vendor sells nonexistent/deprecated item; trainer teaches unknown ability | error | |
| Text lints: missing localization keys, placeholder text ("TODO", "xxx") | warn | error on release branches |
| Reward/XP outlier vs. quest level curve | warn | |

Same engine everywhere: live in Codex (§4), pre-commit hook (optional), CI (mandatory).

### 6.2 Pipeline

- **Pre-merge (`/content` PRs):** `mcc fmt --check` → `mcc check` → incremental `mcc build` → double-build determinism hash → post lint report + content-diff summary (`mcc diff` vs. main: "3 items changed, 1 new quest chain") as a PR comment.
- **Pre-merge (`/tools`, `/schema` PRs):** compiler golden tests (§11.1), editor test suites, and a full `/content` rebuild to catch compiler regressions against real content.
- **Nightly (Baseline §6):** full deterministic build of `/content@main` → artifacts (SQL + `.pck` packs) versioned and handed to the test-realm redeploy job (Server track owns deploy; Tools owns producing correct artifacts and a machine-readable build manifest). A failed nightly content build pages the tools track.

---

## 7. Community Packaging (TLS-08) — M3

**Content pack** = the distribution unit: a signed archive (`.mcpack`) containing a pack manifest (pack ID, semver, `mcc`/schema version, **pinned Godot engine version** for `.pck` compatibility, dependencies incl. base-content version range, license + asset provenance per TD-09), the pack's YAML content subtree, cooked zone data (navmesh, spawn/volume exports, chunk manifests per §2.4) if it includes zones, and client-side Godot `.pck` files for its custom art (or references to base asset IDs only).

**Build:** `mcc pack` compiles, validates (full TLS-07 ruleset — packs get no lint exemptions), and emits the `.mcpack` + content hash. **Share:** any file host; a community index site is out of tools scope (open question §13). **Load:** server operator runs `mcc install <pack>` against their realm — applies the pack's SQL into a pack-namespaced ID range (via `idmap` partitioning: base content and each pack get non-overlapping numeric ID blocks), registers the pack in a realm manifest; clients connecting to the realm are told the pack list + hashes and fetch/mount the client-side `.pck` — the in-client pack download/mount UX is **owned by the Client track per sync decision D-09** (was open question #3 in v0.1; the Godot client mounts pack `.pck` files at runtime via `ProjectSettings.load_resource_pack`). `mcc uninstall` reverses it (pack-namespaced rows make this tractable).

**Creator distribution — the "Meridian Creator Kit":** Theo's download at M3 is Godot 4.6 (official build, MIT), the Forge plugin suite, `mcc`, Codex, sample content (a tiny complete zone), and docs — the whole kit redistributable under MIT + Apache-2.0 with no accounts, no proprietary installers, no multi-hundred-GB engine download. This is the engine pivot's biggest tools-track win: the v0.1 UE-plugin distribution risk (editor licensing, Epic account, setup burden) is gone.

**Versioning/compat:** packs pin a base-content major version, schema versions, and a Godot engine version (the `.pck` format is not guaranteed stable across engine minors — see §13); `mcc install` refuses incompatible packs with actionable errors; `mcc migrate` upgrades a pack's source (not its binary) to newer schemas.

**What IT-M3 ("community-made zone loads on an unmodified server") requires — this track's checklist:**
1. Zero server code/config changes needed to load a pack — everything data-driven through the standard world DB + pack manifest (Server-track contract: `worldd` loads pack-namespaced content like base content).
2. ID-collision safety via namespaced string IDs + partitioned numeric ranges.
3. A community creator (not a team member) authors the zone using only the released Creator Kit + public docs (§10).
4. Client joins the packed realm, streams into the custom zone (WLD-01 chunk-streaming path works for pack zones), completes a quest in it.

---

## 8. GM/Moderation Tooling Contribution (OPS-02) — M1 basic

OPS-02 is Server-track-led (command execution, permissions) and Client-track-surfaced (chat commands/GM UI in the Godot client). Tools track contributes:

- **GM command data:** command/permission-tier definitions as content data so realm operators can adjust tiers without code.
- **Codex "GM console" panel (M1 basic):** connect to a realm (GM auth), then: teleport-to/summon, spawn NPC by Codex ID (with "spawn this" button on any NPC editor page against the dev realm — shared plumbing with TLS-06 and the D-07 reload RPC), give item by ID, set quest state for a character (invaluable for quest-chain QA), reload-content trigger.
- **Moderation support (M4 direction only, per Baseline §3/M4):** ticket/report data schemas and a Codex-hosted review panel — direction stated here, scoped at end of M2 per baseline rule.

---

## 9. Deliverables by Milestone

### M0 — Foundation
- **`mcc` v0 (TLS-01):** YAML → world-DB SQL + client `.pck` for NPC + item schemas; deterministic full builds; `build/check/fmt`; golden tests in CI (Windows + Linux).
- **Codex alpha:** app shell (Avalonia), schema-driven forms, ID index/pickers; **NPC editor alpha (TLS-03)** and **item editor alpha (TLS-04)** — create/edit/save round-trip with live schema validation.
- **Forge terrain spike:** Terrain3D evaluation (adopt/fork vs. build a Meridian terrain GDExtension) — **decision gate at M0 exit** (§3.1); plus an `EditorPlugin` skeleton proving the Godot plugin architecture (dock, custom node with gizmo, GDExtension call).
- `/schema/content/` v1 for NPC + item; chunk-format contract v1 drafted with Client track (§2.4); `idmap.lock` mechanism; content CI skeleton (fmt+check on `/content` PRs).
- **IT-M0 contribution:** the character stub's test NPC and starter items are authored in Codex and land on `worldd` via `mcc` — proving the pipe end-to-end even for trivial content.

### M1 — Greybox Vertical Slice (Tools = critical path)
- **Forge v1 (TLS-02):** terrain sculpt/paint (per M0 terrain decision), greybox kit placement, spawn/patrol placement (NPC-01), leash/ambience/music/POI volumes (CMB-02, AUD-02/03 basic, WLD-03), Recast server navmesh bake, chunk grid presets + chunk export (WLD-01, §2.4). **Capability bar: Zone-01 built 100% in Forge.**
- **Codex v1:** NPC/mob editor complete incl. AI params + ability data (D-08) + gossip/vendor/trainer (TLS-03, CMB-01/02, NPC-02); item editor complete (TLS-04, ITM-01); **quest graph editor (TLS-05, QST-01)**; loot-table editor (ITM-02); spawn tables; vendor inventories (ECO-01); level/stat curve tables (CHR-03).
- **`mcc` v1:** incremental builds, all M1 schemas, `mcc diff`; semi-live dev-realm loop (§5).
- **TLS-07 v1:** full §6.1 M1 ruleset, pre-merge pipeline, nightly build artifacts for test-realm deploy.
- GM console basic (OPS-02); SFX/music hookup data paths (AUD-01/02 content references) authored via Codex/Forge volumes.
- **IT-M1 contribution:** every NPC, mob, item, quest, loot table, and vendor in the 10-quest chain authored via Codex; Zone-01 via Forge, streamed through the §2.4 chunk format; content deployed through `mcc` nightly artifacts.

### M2 — Systems Depth
- **TLS-06 full live preview:** automatic <10s/<30s/<2min loop (§5) on the D-07 reload RPC, in-Forge live spawn preview against dev realm.
- **Forge art-pass workflow:** kit-remap (greybox→final), day/night & weather authoring (WLD-02), instance-entrance markup + dungeon-instance content data (GRP-02), ambience/music volume polish (AUD-03, AUD-02 full).
- **Codex:** itemization budget enforcement (ITM-03 → lint error), crafting recipe editor (ECO-02), talent editor (CHR-04), AH taxonomy data (ECO-03), QST-02 scripted-event/exploration objectives + branch groups, buff/aura data forms (CMB-04).
- **IT-M2 contribution:** Dungeon-01 spatial markup in Forge; all recipes/professions/AH categories in Codex; economy-loop content (gather nodes → recipes → vendor/AH data) fully tool-authored; Zone-01 art pass executed via kit-remap without breaking gameplay markup.

### M3 — Alpha World
- **TLS-08 community release:** `mcc pack/install/uninstall`, pack namespacing, compat gates (incl. engine-version pin); **Meridian Creator Kit distribution** (Godot 4.6 + Forge plugin + Codex installer + `mcc` + sample content), docs/tutorials (§10).
- Scale-out proof: 4 zones + battleground markup (PVP-02: capture points, spawn rooms, flag data) + Dungeon-02 via existing tools; Lua addon-API packaging support for UI-02 (addon manifest format + pack inclusion; runtime is Client track).
- Tooling perf at scale: full-world build < 30 min, Codex responsive on 10k+ entity index (§13 risk).
- **IT-M3 contribution:** recruit 2–3 external creators pre-M3; at least one community-made zone pack (built exclusively on the released Creator Kit + docs) loads on an unmodified server with 500 CCU alpha realm running base + pack content.

*(M4 per baseline: direction only — moderation tooling depth (OPS-02/§8), localization pipeline in `mcc`, launcher/patcher content-delivery integration. Scoped at end of M2.)*

---

## 10. UX & Docs

**Usability bar:** persona Theo (§1.3) — Godot-literate hobbyist, no C++/SQL — must go from Creator Kit download to "my quest runs on my local realm" in **under one day** using only public docs. Concretely: no raw YAML editing required for any supported content type; every validation error names the entity, field, and fix; every reference field is a searchable picker; destructive actions are undoable; Codex first-run wizard sets up `/content` checkout + dev realm config; the Creator Kit installer sets up Godot + Forge in one step (no separate engine hunt).

**Documentation deliverables:**
- M0–M1 (internal, in `/docs/tools/`): `mcc` CLI reference; content-format & ID conventions; "author an NPC/item/quest" quickstarts (written *for* community quality, dogfooded internally per Baseline §5.5 definition of done).
- M2: generated schema reference (§2.3); Forge zone-building guide (greybox), including "Godot editor basics for Meridian creators" so Theo doesn't need prior engine docs; dev-realm setup guide.
- M3 (community release): end-to-end tutorial series — "Your first zone" (Forge), "Your first quest chain" (Codex), "Publish a content pack" (TLS-08); 3 video walkthroughs; troubleshooting/FAQ; sample pack repo (a tiny complete zone) as living reference.

---

## 11. Testing

### 11.1 Compiler golden tests (`mcc`)
- Golden-corpus tests: curated `/tools/mcc/tests/corpus/` YAML sets → committed expected SQL + `.pck`-manifest outputs; any diff is a reviewed change.
- Determinism tests: double-build hash equality per PR (Windows and Linux runners must agree).
- Migration tests: every schema bump ships corpus files in version N-1 + expected N output.
- Negative tests: one fixture per TLS-07 lint rule proving it fires (and only when it should).
- Scale test: synthetic 50k-entity content tree; asserts build-time and memory ceilings (tracks §13 YAML-scale risk).

### 11.2 Editor tests
- Codex: unit tests on view-models + round-trip property tests (open → edit → save → reparse ≡ semantic identity, formatting canonical); Avalonia headless UI tests for critical flows (create NPC, wire loot link, build quest graph with cycle rejection).
- Forge: headless Godot editor test runs (`godot --headless`, gdUnit4-style suite + GDExtension unit tests for `forge_core`) covering export correctness (place spawn/volume node → exported YAML matches), Recast navmesh bake smoke test on a fixture scene, chunk-export golden test against the §2.4 schema, kit-remap round-trip. Editor-plugin CI runs against the pinned Godot version *and* the next minor in a non-blocking canary lane (tracks §13 API-churn risk).

### 11.3 Integration-test contributions & done-criteria

| IT | Tools contribution | Tools "done" criterion |
|----|--------------------|------------------------|
| **IT-M0** | Test NPC + starter items authored in Codex alphas; `mcc` v0 artifacts feed the `worldd` DB and client `.pck` used in the test | Content visible in-world came through the full YAML→`mcc`→SQL/`.pck` path; zero hand-written SQL; build reproducible from `/content@tag` |
| **IT-M1** | Zone-01 (Forge, incl. chunk export §2.4), all NPCs/mobs/items/quests/loot/vendors (Codex), nightly content artifacts for the public test realm | The 10-quest chain to level 5 is completable using only tool-authored content; TLS-07 green on the shipped content revision; a mid-test content fix lands realm-side via the pipeline in < 1 hour |
| **IT-M2** | Dungeon-01 markup, crafting/AH/talent data, art-passed Zone-01 via kit-remap, live preview loop in team use | Economy-loop data (nodes→recipes→vendor/AH) 100% Codex-authored; kit-remap introduced zero gameplay-markup regressions (diff-verified); §5 iteration targets met in measured team usage |
| **IT-M3** | Released Creator Kit + docs + `.mcpack` toolchain; external-creator pack | A non-team creator's zone pack passes `mcc check`, installs via `mcc install` on an **unmodified** server, and is played at 500 CCU alongside base content; uninstall leaves the realm DB clean |

---

## 12. Traceability Table

Every feature ID where Tools is ● in the baseline matrix (Baseline §4):

| Feature ID | Tools deliverable(s) | Milestone |
|-----------|----------------------|-----------|
| CHR-03 | Level/stat/XP curve tables in Codex; NPC stat derivation (§4.1) | M1 |
| CHR-04 | Talent tree editor (§4.5) | M2 |
| WLD-01 | Forge chunk grid presets, streaming-cell budgets & chunk export format (§2.4, §3.3) | M1 |
| WLD-02 | Day/night & weather authoring in Forge (§3.3); spawn-table conditions (§4.5) | M2 |
| WLD-03 | POI/discovery volumes in Forge; map-pin data compiled by `mcc` (§3.2) | M1 |
| CMB-01 | Ability/spell data forms + compiled ability tables (Codex scope per D-08) (§4.1) | M1 |
| CMB-02 | AI params in NPC editor (§4.1); leash volumes + Recast server navmesh in Forge (§3.2) | M1 |
| CMB-04 | Buff/debuff/aura data forms; proc/effect refs on items (§4.2) | M1 basic / M2 |
| NPC-01 | NPC definitions (Codex §4.1); spawn/patrol placement (Forge §3.2); spawn tables (§4.5) | M1 |
| NPC-02 | Gossip tree, vendor link, trainer data in NPC editor (§4.1) | M1 |
| QST-01 | Quest graph editor: kill/collect/deliver objectives, rewards (§4.3) | M1 |
| QST-02 | Chain/prerequisite graph, branches, scripted-event objectives (§4.3) | M2 |
| ITM-01 | Item editor core (§4.2) | M1 |
| ITM-02 | Loot-table editor + probability preview (§4.4); NPC loot links (§4.1) | M1 |
| ITM-03 | Budget model data + live budget meter + CI enforcement (§4.2, §6.1) | M2 |
| ECO-01 | Vendor inventory editor; item pricing fields (§4.5, §4.2) | M1 |
| ECO-02 | Crafting recipe editor; gathering-node data (§4.5) | M2 |
| ECO-03 | AH category/search taxonomy data (§4.5) | M2 |
| GRP-02 | Instance-entrance markup + dungeon content data in Forge (§3.3) | M2 |
| PVP-02 | Battleground markup (capture points, flags, spawn rooms) in Forge (§9/M3) | M3 |
| UI-02 | Addon manifest format + pack inclusion in `.mcpack` (§9/M3) | M3 |
| AUD-01 | SFX event reference fields across editors; compiled SFX hookup tables (§9/M1) | M1 |
| AUD-02 | Music-region volumes with adaptive-layer hints in Forge (§3.2, TD-11) | M1 basic / M2 |
| AUD-03 | Ambience volumes/emitter placement in Forge (§3.2) | M2 |
| TLS-01 | `mcc` content compiler (§2.2) | M0 |
| TLS-02 | Forge zone editor — Godot editor plugin suite (§3) | M1 |
| TLS-03 | NPC/mob editor incl. ability forms per D-08 (§4.1) | M0 alpha / M1 |
| TLS-04 | Item editor (§4.2) | M0 alpha / M1 |
| TLS-05 | Quest graph editor (§4.3) | M1 |
| TLS-06 | Live preview loop + in-editor spawn preview (§5) | M2 |
| TLS-07 | `mcc check` lint engine + content CI (§6) | M1 |
| TLS-08 | `.mcpack` build/share/install toolchain + Creator Kit release (§7) | M3 |
| OPS-02 | GM command data + Codex GM console (§8) | M1 basic |

*(ACC-01, CHR-01, CMB-03, ECO-04, UI-01, OPS-01, OPS-03: Tools is ○ consulted — no deliverables owed; contributions noted inline where relevant, e.g. graveyard volumes for CMB-03.)*

---

## 13. Risks & Open Questions

**Resolved by the engine pivot:** v0.1's top risk — *UE editor plugin distribution to the community* (Epic account, 100+ GB download, EULA-bound combined distribution, and the related Forge-licensing open question #6) — is **eliminated**. The Godot editor is MIT; the entire Creator Kit (§7) is redistributable as free software with a download measured in hundreds of MB, not hundreds of GB.

### Risks

| # | Risk | Impact | Mitigation |
|---|------|--------|-----------|
| R1 | **Terrain build-vs-adopt** — Godot has no built-in heightfield terrain; adopting/forking Terrain3D risks upstream divergence and gaps (chunk-grid alignment, min-spec LOD), while building our own risks schedule | TLS-02 / M1 critical path | M0 terrain spike with hard decision gate at M0 exit (§3.1, §9); evaluation criteria written up front; fork-and-vendor if adopted, so upstream churn can't break M1; terrain feature set frozen at M0 exit either way |
| R2 | **YAML scale limits** — tens of thousands of files: parse time, Codex index memory, Git checkout size | Editor responsiveness, CI time | Incremental build cache (§2.2), 50k-entity scale test in CI (§11.1), Codex on-disk index; if breached, batch-per-category files are the escape hatch (format already permits) |
| R3 | Tools are the M1 critical path (Baseline §3); a Forge slip blocks the vertical slice | M1 date | M0 exit demands the compiler pipe proven end-to-end + terrain decision made + plugin skeleton proven; Forge greybox feature set frozen at M0 exit; content team embedded with tools team from month 4 |
| R4 | Navmesh/pathing quality mismatch between Forge bake and `worldd` runtime behavior | CMB-02 correctness | Single vendored Recast codebase shared by `forge_core`/`mcc`/`worldd` (§3.2); unreachable-spawn lint runs on the same artifact the server loads |
| R5 | Hot-reload (TLS-06, D-07) destabilizes `worldd` state | Dev-realm trust | Reload endpoint dev/GM-gated; reloadable-table scope explicitly bounded in the Server PRD; nightly realm always rebuilt from clean artifacts, never hot-patched |
| R6 | Merge conflicts in `idmap.lock` under parallel content work | Contributor friction | Partitioned ID ranges per pack/branch allocation policy; `mcc idmap verify` in CI; conflict-free append-only format |
| R7 | **Godot editor plugin API churn** — `EditorPlugin`/GDExtension interfaces shift across engine minors (4.6 → 4.7+), breaking Forge for creators on mismatched versions | Forge maintenance, Creator Kit support burden | Pin the blessed Godot version per release; Creator Kit bundles the exact engine build; CI canary lane against the next minor (§11.2); keep heavy logic in `forge_core` GDExtension behind a thin editor-facing layer to shrink the churn surface |
| R8 | **`.pck` format stability** — Godot's `.pck` container is not guaranteed stable across engine versions; a realm's packs could fail to mount on a newer client | TLS-08 pack ecosystem, patching | Pack manifest pins the Godot engine version (§7); `mcc install`/client mount refuse engine-mismatched packs with actionable errors; `mcc pack` can re-cook a pack's `.pck` from source for a new engine version without touching content |

### Open questions (baseline gaps — flagged, not invented)

*Resolved since v0.1 via cross-track sync (docs/01-SYNC-DECISIONS.md): ability/spell authoring is Codex scope under TLS-03 (**D-08**, §4.1); the `worldd` hot-reload RPC is committed by the Server track (**D-07**, §5); client pack download/mount UX is owned by the Client track (**D-09**, §7). The Forge/UE licensing question is moot post-pivot.*

1. **Pack distribution/discovery:** TLS-08 covers build/share/load, but no baseline decision on a hosted community index/registry vs. file-sharing only. Proposed: out of 1.0, revisit at M3.
2. **Localization pipeline:** baseline M4 mentions localization; no TD/feature ID covers how `/content` strings are extracted/translated. `mcc` reserves string-key structure now; decision needed by end of M2.
3. **Spawn-table conditions vs. WLD-02 timing:** spawn conditions on time-of-day/weather (§4.5) are schema-reserved at M1 but unusable until WLD-02 (M2); confirm Server track's evaluation order for condition data.
4. **Terrain evaluation criteria sign-off:** the M0 Terrain3D adopt/fork-vs-build gate (§3.1) needs its evaluation criteria co-signed by Art (paint-layer/material needs) and Client (streaming/min-spec perf) before the spike starts.
