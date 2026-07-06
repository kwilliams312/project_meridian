# Forge v1 — Proposed Frozen Scope

**Status: DRAFT — proposed freeze, pending owner approval (#135). No additions after M0 exit once approved.**

**Track:** Tools (Forge, the terrain/zone editor — TLS-02)
**Reads with:** [Tools PRD](prd/tools-prd.md) §3, §9/M1, §13/R3 · [Tools SAD](sad/tools-sad.md) §5 (Forge), §5.2 (`ITerrainBackend`), §8 (M1 column) · [Sync Decisions](01-SYNC-DECISIONS.md) §11 (A-09 terrain) · [terrain-eval.md](terrain-eval.md)
**Scope of this freeze:** the **Forge v1 (TLS-02) M1** feature set only. Codex, `mcc`, content CI, and community packaging are out of this document's scope — they have their own milestone phasing (Tools PRD §9).

> This is a **proposal for the project owner to approve or trim.** The IN / OUT lists below are drawn from what the Tools PRD already scopes for M1; the owner draws the final line. Nothing here changes the baseline matrix or any feature ID — it fixes *which parts of the already-planned M1 Forge work are locked* so the set can't grow on the critical path.

---

## 1. Rationale — why the freeze exists

Tools are the **M1 critical path** (Baseline §3; Tools PRD §13 **R3**). A Forge slip blocks the greybox vertical slice, so R3's mitigation is explicit: *"M0 exit demands the compiler pipe proven end-to-end + terrain decision made + plugin skeleton proven; **Forge greybox feature set frozen at M0 exit**."* The same freeze is a hard M0-exit criterion in the SAD (§8: *"Forge feature set for M1 frozen"*) and is echoed in R1 (*"terrain feature set frozen at M0 exit either way"*).

The freeze is meaningful now because the one thing that could still move the terrain surface has settled. **A-09 is resolved: fork-and-vendor Terrain3D** (Sync Decisions §11; terrain-eval.md — all four co-signed criteria PASS). With the backend chosen and wrapped behind the **`ITerrainBackend` seam** (Tools SAD §5.2), the terrain operation set is fixed and drawable cleanly: sculpt/paint, paint-layer↔`art.*` binding, 128 m region alignment, `export_heightfield`, and collision-geometry enumeration are the exact five operations both the vendored fork and any future in-house backend must implement. Nothing about the M1 terrain feature set is waiting on an open decision anymore.

So this document proposes the line: **everything in §2 is Forge v1; everything in §3 is explicitly not.** After owner approval and M0 exit, moving the line requires a decision-log entry (§4).

---

## 2. IN — Forge v1 feature set (proposed frozen)

The capability bar for the whole set is one sentence from Tools PRD §9/M1: **"Zone-01 built 100% in Forge."** Every item below is something Zone-01 greybox needs; nothing below is polish or a later-milestone capability.

Drawn from Tools PRD §3 (Forge), §9/M1 ("Forge v1 (TLS-02)"), and Tools SAD §5 / §8 (M1 Forge column):

- **Terrain sculpt & paint — via `ITerrainBackend`** (Tools PRD §3.1; SAD §5.2 op 1). Height edits (raise/lower/smooth/flatten/set) and layer paint over a brush region, world-space addressed, **clamped to authored zone bounds**, with undo/redo bridged into the Forge editor undo stack. All terrain access goes through `ITerrainBackend` — no direct Terrain3D API calls from docks, exporter, or bake (SAD §5.2).
- **Paint-layer ↔ `art.*` terrain-set binding** (Tools PRD §3.1; SAD §5.2 op 2). Bind a paint slot to an Art-track terrain-set asset ID (`art.terrain.*`) resolved through the IF-8 asset registry. Authoring-time budget enforcement: ~8 layers/zone, ≤ 4 blended/chunk (Art PRD §2.3), as a warn→error lint.
- **Zone bounds & origin authoring** (Tools PRD §3.1; SAD §3.1/§5.2). Author the zone origin (default: terrain bounds min-corner snapped to the 128 m grid) and the authored bounds that sculpt/paint clamp to and that the reachability lints test against. Zone-local coordinates, permanently (SAD §3.1, D-20).
- **Modular greybox kit placement** (Tools PRD §3.1, §3.4 M1 greybox; SAD §5.1 `KitInstance`). Palette browser fed by the IF-8 asset registry (`art.kit.greybox.*`), snapping, randomized scatter brushes, per-placement gameplay flags (collision-relevant vs. dressing-only). Placements serialize by kit **asset ID** (`KitInstance` resolves ID→scene at load) so a zone survives kit re-exports.
- **Spawn & patrol placement (NPC-01)** (Tools PRD §3.2; SAD §5.1 `MeridianSpawnPoint` / `MeridianPatrolPath`). Spawn-point nodes bound to Codex **spawn-table IDs** (Forge owns *where*, not *what*); patrol paths as curve nodes with per-waypoint wait times/emotes; leash-radius gizmo (CMB-02); respawn-timer overrides. Exported to `/content` for the server.
- **Gameplay volumes** (Tools PRD §3.2; SAD §5.1 `MeridianVolume`). The M1 subtypes: leash boundaries (CMB-02), ambience volumes → `amb.*` (AUD-03 basic), music regions → `mus.*` with adaptive-layer hints (AUD-02 basic, TD-11), and discovery/POI volumes with map-pin + discovery-XP data (WLD-03). *(Graveyard/resurrect regions CMB-03 consulted; instance-entrance markers GRP-02 are M2 — see §3.)*
- **Recast server navmesh bake** (Tools PRD §3.2; SAD §2.5/§3.2; D-15). Standalone vendored Recast bake in `forge_core` producing **server-consumed** Detour tiles (32 m tiles, 4×4 per chunk) — *not* Godot's NavigationServer — so the editor, `mcc`, and `worldd` share one bit-identical codebase (R4). In-editor bake with live unreachable-spawn surfacing (mirrors the TLS-07 lint).
- **Chunk grid presets + end-to-end chunk export (WLD-01, §2.4/IF-6)** (Tools PRD §3.3, §9/M1; SAD §5.4). Author against the 128 m Meridian chunk grid with per-zone-type presets; validate per-chunk streaming-cell budgets against Client-track budgets at export (L081); write the grid manifest + client scenes + server FlatBuffers payloads. Forge owns the chunk format (IF-6); this is the IT-M1 end-to-end exercise.
- **`export_heightfield` — server heightfield extraction** (Tools SAD §5.2 op 4; terrain-eval.md C4). Per-chunk `f32[129×129]` at 1 m spacing for server movement validation (OPS-03) and as navmesh-bake input; hole/un-sculpted cells surface as a lintable sentinel, never silently zeroed.
- **Validation dock + `mcc` invoker integration** (Tools SAD §5.4, §5.1). In-Forge `mcc` invocation with `--diag-format=json` diagnostics streamed into a validation dock (click diagnostic → select offending node), plus the build-panel UI. This is the Forge half of the semi-live M1 loop (below).
- **Semi-live dev-realm loop — Forge half (TLS-06 partial, M1)** (Tools PRD §5, §9/M1; SAD §8 M1). M1 ships the *semi-live* loop: save → auto-compile → manual `worldd` content-reload (targets ×2), plus one-click **"teleport my dev character here"** via the IF-7 GM channel. *(The fully automatic loop and the in-Forge live spawn-preview overlay are TLS-06 proper at M2 — see §3.)*
- **YAML round-trip export to `/content`** (Tools PRD §2.1 spatial exception; SAD §5.3). Canonical projection of the node tree to the server-read `/content` YAML/binary sidecars via the shared `mcc fmt` emitter, with hash-guarded re-import and explicit conflict UI (no silent overwrite in either direction).

---

## 3. OUT — explicitly deferred past Forge v1

Everything below is real Forge work that is **not** in v1. Most is a later milestone in the Tools PRD; a couple are tempting adjacencies that would grow the M1 surface. Listing them makes the freeze line unambiguous.

Deferred to **M2** (Tools PRD §3.4, §9/M2; SAD §8 M2):

- **Art-pass kit-remap workflow** — greybox→final kit ID remap table (Tools PRD §3.4). v1 ships greybox placement only; the remap that re-points to final art is M2.
- **Day/night & weather authoring (WLD-02)** — curve-based 24h lighting script + weather-state table + in-editor preview scrubber (Tools PRD §3.3). Also gates the spawn-table time-of-day/weather conditions, which are schema-reserved but unusable until M2.
- **TLS-06 full live preview** — the automatic <10s/<30s/<2min loop on the D-07 reload RPC, and the **in-Forge live spawn-preview overlay** showing server-side mob positions in the editor viewport (Tools PRD §5, §9/M2). v1's loop is semi-live with a manual reload step.
- **Instance-entrance markup + dungeon-instance content data (GRP-02)** (Tools PRD §3.3, §9/M2). Zone-01 greybox does not need it.
- **Ambience/music volume *polish*** (AUD-03 full, AUD-02 full) — v1 has basic volume placement; the adaptive/polish pass is M2 (Tools PRD §3.4).

Deferred to **M3** (Tools PRD §9/M3):

- **Battleground markup (PVP-02)** — capture points, spawn rooms, flag data.
- **Creator Kit packaging of Forge** — the redistributable Godot+Forge+docs bundle is TLS-08/M3, not a v1 editor feature.

Out of Forge scope entirely / not planned for 1.0 (tempting but excluded — flagged here so the freeze line is clear; several are called out as later or non-goals in Tools PRD §1.4, §13):

- **Advanced terrain material / shader-graph editing** — v1 binds paint layers to Art-track `art.terrain.*` sets by ID (§2); authoring the terrain *materials/shaders* themselves is Art-track DCC pipeline, not Forge (Tools PRD §1.4 "asset-creation DCC tooling" non-goal).
- **Runtime scripting / visual-scripting UI for content behaviour** — content is data-driven schemas + the client Lua addon API (Client track owns runtime); a scripting language for content beyond the schemas is a stated **non-goal for 1.0** (Tools PRD §1.4).
- **Real-time multi-user / collaborative co-editing** — Git is the collaboration layer; collaborative real-time co-editing is a stated **non-goal for 1.0** (Tools PRD §1.4).
- **In-game / in-client world-building tools** — stated **non-goal for 1.0** (Tools PRD §1.4). All authoring is in the Godot-editor Forge plugin.
- **Linux/macOS Forge builds** — Forge stays **Windows-only** (TD-08; D-28 rule 5 keeps Tools out of the macOS-client decision). Cross-platform is cheap on Godot later but not promised.
- **A second (in-house) terrain backend** — the `ITerrainBackend` seam exists so `MeridianTerrainBackend` *could* be a drop-in swap (SAD §5.2), but v1 ships **only** the vendored Terrain3D fork (A-09). Building the in-house backend is not v1 work.
- **Own-your-own navmesh runtime / Godot NavigationServer path** — v1 deliberately bakes standalone Recast for the server (D-15, R4); adopting Godot's navigation is explicitly rejected, not deferred.

---

## 4. Change-control note

Once the owner approves this scope and **M0 exit** passes, Forge v1 is frozen: **any addition to §2 (or promotion of a §3 item into v1) requires an explicit decision-log entry** in [`docs/01-SYNC-DECISIONS.md`](01-SYNC-DECISIONS.md) — a new `D-xx` (or an `A-xx` action item that resolves to one) naming the change, its M1-schedule impact, and sign-off. This mirrors how every other cross-track scope change is recorded and is what makes R3's "no additions after M0 exit" enforceable rather than aspirational: without a logged decision, the frozen set is the set.

Corollary: items already in §2 may be *cut* under the same change-control if M1 schedule pressure demands it — a freeze bounds growth, it does not forbid trimming. Cuts are logged the same way.

---

*Approval: this document is a DRAFT proposal (#135). The project owner approves, trims, or amends the IN / OUT split above; on approval, update this header to record the decision and the M0-exit freeze date.*
