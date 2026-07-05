# Client Track PRD — Project Meridian

**Track:** Client (Godot 4.6 game client)
**Version:** 0.2 — 2026-07-04 (engine pivot UE5 → Godot 4.6 per Baseline v0.3 / sync decision log §4; structure, feature IDs, and milestones unchanged from v0.1)
**Status:** Draft for cross-track review
**Baseline:** [Game Design Baseline v0.3](../00-GAME-DESIGN-BASELINE.md). All feature IDs, milestone names (M0–M4), and technical decisions (TD-01..TD-12) referenced here are defined there and are binding.

---

## 1. Overview & goals

The Client track delivers the Godot 4.6+ Windows-native x64 game client (TD-01): the player-facing presentation layer for a custom, authoritative Linux server. Two baseline pillars dominate every client decision:

- **"Runs on real hardware" (Pillar 2, TD-02/TD-03).** Every rendering feature we adopt must have a no-hardware-RT, GTX 1060-class story. We treat the 1060/16GB "Low @ 1080p/30" target as a hard gate in CI-adjacent perf testing, not an aspiration.
- **"Server is law" (Pillar 3).** The client is a *predictor and presenter*, never an authority. The client renders what the server says exists, predicts only its own movement (CHR-02), and reconciles when corrected. No gameplay outcome (damage, loot, quest state, currency) is ever computed client-side; the client requests, presents, and animates.

### 1.1 Language architecture (TD-01)

The client is split along a hard performance boundary:

- **C++ GDExtension modules** for everything on the hot path: the network module (framing, TLS, dispatch), prediction/reconciliation, entity interpolation, the entity registry, and the custom zone-streaming system (§2). These are plain C++ libraries with an engine-agnostic core and a thin Godot binding layer — the FlatBuffers C++ library (TD-07) links cleanly into GDExtension, so generated schema code is consumed natively with zero-copy decode.
- **GDScript** for UI, screens/flows, settings, and glue — everywhere iteration speed beats microseconds. The UI model layer (§5) is the boundary: GDExtension systems publish state into it; GDScript views consume it.

Rule of thumb enforced in review: if it runs per-entity-per-frame or per-message, it's C++; if it runs per-user-interaction, it's GDScript.

Secondary goals:

- **Consume, don't own, content.** All gameplay data reaches the client as compiled `.pck` resource packs from the content compiler `mcc` (TLS-01) referenced by stable asset IDs (Baseline §5.3). The client binary contains systems, not content.
- **Custom protocol, not Godot multiplayer (TD-07).** The client speaks the schema-defined FlatBuffers binary protocol shared with the server; Godot's high-level multiplayer API (MultiplayerAPI/ENet/scene replication) is not used for game state (§3.1).
- **Moddable UI by design (UI-02).** The HUD is architected from M1 so that a Lua addon API can be layered on at M3 without a rewrite.
- **Ship continuously.** Nightly client builds feed the persistent test realm from end of M0 (Baseline §6).

Non-goals for 1.0 (Baseline §7): macOS/Linux/console clients, hardware ray tracing features, mobile companion.

---

## 2. Engine & rendering strategy

### 2.1 Godot 4.6 feature decisions

All choices are constrained by TD-02 (**Forward+ renderer on Direct3D 12** — the default Windows backend since Godot 4.6; **no hardware RT dependency, ever assumed**) and TD-03 (1060-class → high end). Guiding rule: Godot has **no Nanite equivalent and no Virtual Shadow Maps** — the classic pipeline is the only pipeline, so LOD/draw-call discipline is a first-class engineering requirement, not a fallback.

| Concern | Decision | Low-end story |
|---|---|---|
| **Renderer/backend** | Forward+ on D3D12 (TD-02). Vulkan backend kept buildable in CI as a diagnostic escape hatch for D3D12 driver bugs, never shipped as default. | Forward+ runs on GTX 1060-class D3D12 hardware; validated on a physical 1060 at M0. |
| **Global illumination** | **SDFGI** at High/Epic tiers (dynamic GI, no RT hardware needed). | Low/Medium disable SDFGI and use **baked lightmaps (LightmapGI) + reflection probes** with a matched ambient rig so zones read correctly in both paths. Zone lighting workflow (with Art/Tools) bakes both representations from one source lighting setup — one rig, two outputs, validated per zone from M1. |
| **Geometry density** | **No Nanite equivalent exists (TD-02): strict traditional LOD discipline is mandatory.** Every mesh ships an authored/generated LOD chain; visibility ranges and LOD bias are scalability knobs; occlusion culling enabled and tuned per zone; **MultiMesh/GPU instancing** for crowds, foliage, and repeated kit pieces. Art-track polycount budgets (Art PRD) are enforced at import time by a validation script. | Low tier applies an aggressive LOD bias and shorter visibility ranges. Draw-call and triangle budgets in §9 are the contract. |
| **Shadows** | **No Virtual Shadow Maps: shadow atlas + CSM tuning is the whole story.** Directional CSM with per-tier cascade counts/distances; positional-light shadow atlas sized per tier. Art track is on notice (via zone review) that shadow-dependent readability must hold under Low-tier CSM. | Low: 2 cascades, reduced distance, smaller atlas. High: 4 cascades, larger atlas, contact-hardening options. |
| **Zone streaming (WLD-01)** | **Godot has no World Partition; zone streaming is a custom chunk-streaming system** — see §2.4. | Smaller loaded chunk radius on Low; distant chunks represented by baked low-poly proxy meshes (our HLOD equivalent, produced by the Forge export pipeline). |
| **Upscaling** | **FSR2** (built into Godot 4.x) as the default temporal upscaler; render-scale is a first-class scalability axis (Low = 1080p output at ~65–75% render scale if needed to hold 30 FPS). Vendor-specific upscalers (DLSS) never required (spirit of TD-03). | FSR2 is vendor-agnostic and runs on the 1060. |
| **Anti-aliasing** | High tier: TAA (or MSAA 2x where the scene is instancing-heavy and TAA ghosting hurts readability — per-zone call). Low: FXAA or FSR2's built-in AA. | — |
| **Day/night (WLD-02)** | Dynamic sun/sky at Medium+ (SDFGI handles dynamic GI at High; Medium accepts probe-lit dynamic sun). Low tier uses time-of-day *blended baked* states (N baked lightmap snapshots interpolated) — cheaper, visually stable. Design cost: zones bake N snapshots; Tools/Art pipeline must support it by M2. | — |
| **Not used** | Hardware RT of any kind; Godot high-level multiplayer (§3.1); VoxelGI (SDFGI covers the dynamic-GI tier); volumetric fog on Low. | — |

### 2.2 Scalability presets → TD-03 hardware tiers

Presets ship as data-driven settings profiles with an auto-benchmark on first run. Targets are **frame-time gates measured in the crowded-scene benchmark** (§9), not empty-map numbers.

| Preset | Reference HW (TD-03) | Target | Key settings |
|---|---|---|---|
| **Low** | GTX 1060 6GB / 16GB RAM (hard floor) | **30 FPS @ 1080p** (33.3 ms) | Baked lightmaps + reflection probes (SDFGI off); CSM 2 cascades, small shadow atlas; **FSR2 at reduced render scale**; aggressive LOD bias + short visibility ranges; reduced streaming radius; crowd LOD aggressive; SSAO low |
| **Medium** | GTX 1660 Super / RX 5600 class | 60 FPS @ 1080p | Baked GI path; CSM 3 cascades; full streaming radius; FSR2 at 100% or native |
| **High** | RTX 3070 class (RT silicon **unused**, TD-02) | **60 FPS @ 1440p** (16.7 ms) | **SDFGI on**; larger shadow atlas, CSM 4 cascades; higher crowd fidelity; TAA or MSAA 2x |
| **Epic** | RTX 4070+ class | 60+ FPS @ 4K/FSR2 or 120 @ 1440p | SDFGI high quality, max shadow atlas, max view/streaming distances |

Every preset must be individually overridable (WoW-player expectation); presets are starting points, and the settings UI exposes the underlying knobs from M1.

### 2.3 Renderer validation cadence

- **M0:** empty test map + 2 characters proves the Forward+/D3D12 baseline boots on a physical 1060 (and catches D3D12 driver edge cases early — §12 risk 1).
- **M1:** greybox Zone-01 with 50+ characters is the first real perf gate (IT-M1 has 50+ concurrent players). SDFGI vs baked-GI tier boundaries are decided *per the physical-hardware numbers*, not taste.
- **M2:** art-passed Zone-01 ("beautiful corner") re-validates everything at final visual density; this is where LOD-chain, shadow-atlas, and draw-call budgets get locked.

### 2.4 Custom zone streaming (WLD-01)

World Partition does not exist in Godot; WLD-01 is a **custom chunk-streaming system**, owned by the Client track as a C++ GDExtension module:

- **Chunk format:** zones are exported by the Forge zone editor (TLS-02) as a grid of **chunk scenes + a zone manifest** (chunk size, grid origin, per-chunk asset lists, proxy meshes for distant representation). This export format is a **coordinate contract with the Tools track** — chunk dimensions, origin conventions, and manifest schema live in `/schema` and require Client+Tools sign-off (Baseline §5.1). Agreement target: M0 end, before Zone-01 greybox starts.
- **Runtime:** the streaming module tracks the player position, computes the desired chunk set per tier radius, and loads/unloads via **Godot's background `ResourceLoader` (threaded requests) + deferred subscene instancing**, time-sliced so instancing never blows the frame budget (hitch gate in §9). Unloads are deferred and pooled.
- **Distant representation:** beyond the loaded radius, baked proxy meshes (cheap, few draws) stand in for unloaded chunks, streamed from the same manifest.
- **Interest alignment:** streaming radius ≥ server interest radius so an `EntityEnter` never references an unloaded chunk for long; entities arriving ahead of their chunk stand on server-authoritative position with placeholder ground rules (never fall through world).
- Loading screens only on map change (instance portals, GRP-02); in-zone movement is seamless at all tiers.

---

## 3. Networking layer

### 3.1 Protocol consumption (TD-07)

- The client links generated **FlatBuffers** serialization code from the **`/schema` repo** (decided baseline v0.2) as a versioned dependency, compiled directly into the GDExtension net module. Schema version is exchanged at connect; mismatches produce a clear "client out of date" error, not undefined behavior.
- **Godot's high-level multiplayer API (MultiplayerAPI, ENetMultiplayerPeer, MultiplayerSynchronizer/Spawner, RPCs) is not used for game state.** Reasons: (a) TD-07 mandates a custom FlatBuffers protocol shared with a non-Godot server — the server is plain C++/Linux and will never speak Godot's scene-replication wire format; (b) "server is law" needs full control over snapshots, deltas, and interest, which scene replication abstracts away; (c) prediction/reconciliation (§3.3) requires owning the message loop. We implement a client network module (**C++ GDExtension**, engine-agnostic core + thin Godot binding) that owns: TCP connection management via `StreamPeerTCP`/raw platform sockets, framing, TLS (auth connection, via `StreamPeerTLS` or bundled mbedTLS), message dispatch to game systems, and send queues with per-channel priorities (movement > combat > chat > bulk).
- Message pump runs on a dedicated network thread; decoded messages are handed to the main thread via lock-free queue and applied at a fixed point in the frame (before prediction/interp update).
- TCP for everything through M1; the module's transport interface is abstracted so a UDP movement channel can be introduced at/after M2 without touching game systems (TD-07 explicitly gates UDP to ≥M2).

### 3.2 Session flow (ACC-01, ACC-02)

1. **Login UI → `authd` over TLS** (ACC-01): credentials submitted, server returns account status + **realm list**. Client renders realm list with population/latency indicators.
2. **Realm select → session handoff** (ACC-02): `authd` issues a session token/key; client opens the `worldd` connection and authenticates with the token (never re-sends the password). Reconnect-with-token supported for transient disconnects.
3. **Character list → enter world** (CHR-01): `worldd` serves the character list; on enter, the client streams the character's chunk set (WLD-01, §2.4) while showing a loading screen, then receives the initial interest snapshot.

Failure UX at every step (bad password, realm down, handoff timeout, version mismatch) is an M0 deliverable — IT-M0 is literally this flow.

### 3.3 Client-side prediction & reconciliation (CHR-02)

- Local player movement is **predicted**: input is sampled at a fixed simulation tick, applied immediately to the local character, and sent to the server stamped with a client sequence number.
- The client keeps a ring buffer of (sequence, input, resulting state). On each server movement ack/correction containing the last-processed sequence + authoritative state, the client rewinds to that state and **re-simulates** buffered unacknowledged inputs. Small errors are smoothed (positional error offset decayed over ~100–200 ms); large errors snap.
- The client movement simulation must be **the same rules the server validates** (speed, gravity, jump arcs, swim volumes — walk/run/jump/swim per CHR-02). The movement constants live in shared content/schema data, not duplicated magic numbers. We write a **custom kinematic movement controller in the C++ GDExtension** (character-shape sweeps against the physics world) rather than building on `CharacterBody3D`'s GDScript-side move-and-slide — the simulation must be tick-deterministic, re-simulable N times per frame during reconciliation, and mirror server code, none of which fits node-frame coupling.
- Non-movement actions (cast, loot, vendor) are **not predicted for outcome** — the client may play anticipatory presentation (button press feedback, cast-start animation on ack) but state changes wait for the server (Pillar 3). **GCD is the one latency-sensitive exception, now decided per sync decision D-10: the client optimistically starts the GCD on use and the server rolls it back on rejection** (see §4 CMB-01).

### 3.4 Entity interpolation

- Remote entities (players, NPCs) are rendered **in the past** at `server_time - interp_delay` (initially 2× server tick, ~100–150 ms), interpolating between buffered snapshots; hermite/velocity-aware interpolation for smooth turns. Implemented in the C++ entity module (§1.1) — per-entity-per-frame work never runs in GDScript.
- Extrapolation is capped (~250 ms) when snapshots are late, then the entity freezes/fades rather than rubber-banding across the zone.
- Clock sync: client maintains an estimated server clock via ping/offset filtering; all snapshot buffering keys off it.

### 3.5 Interest-managed entity lifecycle

- The **server decides visibility** (interest management, OPS-04 scale); the client never assumes an entity list. Protocol delivers `EntityEnter` (full state), `EntityUpdate` (delta), `EntityLeave`.
- Client maintains an entity registry keyed by server-assigned entity ID → spawns/despawns presentation scenes accordingly. Scene-instance pools for common types (players, humanoid mobs, loot objects) to avoid instancing hitches when 50+ players cross an interest boundary (IT-M1 scenario).
- Entity appearance (race/class/equipment) arrives as asset IDs resolved through the client asset registry (§6); missing assets render a placeholder and log — never crash, never block the stream.

---

## 4. Gameplay presentation by milestone (M0 → M3)

### M0 — Foundation
| Feature | Client deliverable |
|---|---|
| ACC-01 | Login screen (account/password), TLS connection to `authd`, realm list UI, error states. |
| ACC-02 | Token handoff to `worldd`, session establishment, reconnect handling. |
| CHR-01 (stub) | Character stub select screen — **scope decided per sync decision D-11: name entry + class selection over one shared placeholder model** (no appearance options at M0); enter-world flow into the empty test map. |
| CHR-02 (basic) | Predicted walk/run/jump on the test map; remote clients interpolated; third-person camera with WoW-style mouse controls (hold-RMB steer, hold-LMB orbit, mouse-wheel zoom). |
| **IT-M0** | See §10. |

### M1 — Greybox Vertical Slice
| Feature | Client deliverable |
|---|---|
| CHR-01 | Full character create: 1 race, 2 classes (melee + caster), appearance options from art's M1 set; character select with server character list, delete/confirm. |
| CHR-02 | Swim volumes, fall damage presentation, movement polish (turn-in-place, slope handling) matched to server validation. |
| CHR-03 | XP bar, level-up presentation (VFX/SFX hook via AUD-01), character sheet with server-provided stats. |
| WLD-01 | Zone-01 streamed via the custom chunk-streaming system (§2.4) from compiled `.pck` packs; seamless in-zone streaming, loading screen only on map change. |
| WLD-03 | World map + minimap (see UI-01, §5), POI discovery toast driven by server discovery events. |
| CMB-01 | Tab-target combat presentation: tab/click targeting with priority cycling, target unit frame, nameplates, cast bars (self + target, pushback display), **GCD sweep on action buttons: optimistic client start with server rollback per D-10** (client starts the sweep on use, server rejection cancels/refunds), instant vs cast abilities, range/facing failure feedback ("Out of range" + button red-tint), floating combat text (damage/heals/misses/crits, pooled Control nodes). |
| CMB-02 (○ consulted) | No client deliverable beyond presenting server AI behavior; client provides aggro/leash-visible feedback (mob nameplate aggro tint) per server flags. |
| CMB-03 | Death presentation (ragdoll/death anim, desaturated ghost mode), release-spirit dialog, corpse-run UX (corpse marker on map, resurrect prompt), rez acceptance flow. |
| CMB-04 (basic) | Buff/debuff bar on player + target frames: icons, stack counts, duration spirals from server aura updates. |
| NPC-02 | Gossip/dialogue window (server-driven option lists), vendor window, trainer window (list + learn flow). |
| QST-01 | Quest giver indicators (! / ?), quest offer/turn-in dialogs, quest log, on-screen quest tracker with objective progress updates (kill/collect/deliver), quest item highlighting. |
| ITM-01 | Bags UI, drag-drop item management, equipment/paperdoll window, item tooltips rendered from compiled item data + server instance data. |
| ITM-02 | Loot window on corpse interaction, loot roll presentation stub (solo loot at M1; group loot completes at M2 with GRP-01), rarity-colored beams/tints. |
| ECO-01 | Currency display, vendor buy/sell/buyback UI (repair moved to ITM-03, M2 per baseline v0.2). |
| SOC-01 | Chat panel: say/whisper/general channels, tabbed chat, slash commands (`/s /w /join`), chat bubbles (say). |
| OPS-02 (basic) | Client-side GM command console entry (chat-based `.` commands routed to server), GM-visibility flags rendering. |
| AUD-01/02 | Audio hooks live (§7). |
| **IT-M1** | See §10. |

### M2 — Systems Depth
| Feature | Client deliverable |
|---|---|
| CHR-04 | Talent/spec UI: server-defined trees rendered from compiled data, point spend with server confirmation, respec flow. |
| WLD-02 | Day/night sky + lighting transitions (per-tier approach in §2.1), weather VFX/audio states from server weather messages. |
| CMB-04 (full) | Full aura model presentation: dispel types, hidden auras, aura-driven stat/VFX changes, debuff limits UI. |
| QST-02 | Quest chain presentation, prerequisite gating in gossip, scripted event presentation (server-directed actor sequences). |
| ITM-03 (○) | Tooltip/comparison rendering for the full itemization model (rarity colors, stat budgets, procs text, durability/repair UI); no client logic. |
| ECO-02 | Profession UI: recipe list, craft queue with cast-bar, gathering node interaction + skill-up toasts. |
| ECO-03 | Auction house UI: search/filter/sort, listings, bid/buyout, my-auctions; server paging protocol (no full-AH downloads). |
| ECO-04 | Mailbox UI: compose (item/gold attach), inbox, COD flow. |
| ECO-05 | Bank storage UI: bank window, slot purchase flow. |
| SOC-03 | Player-to-player trade window: offer slots, gold field, dual-accept lockstep from server trade state. |
| GRP-01 | Party frames, invite/leave/kick flows, group loot UI (need/greed roll dialogs), loot method display. |
| GRP-02 | Dungeon instancing client flow: portal transition to instanced map (Dungeon-01), instance-reset messaging, party-in-instance state. |
| TLS-06 | Client render support for live server preview of spawn/patrol placement (editor-connected client mode; joint with Tools track). |
| **IT-M2** | See §10. |

### M3 — Alpha World
| Feature | Client deliverable |
|---|---|
| SOC-02 | Friends/ignore lists with online status, guild UI (roster, ranks, guild chat channel, MOTD). |
| GRP-03 | LFG tool UI: role selection, queue status, proposal/accept flow, teleport-to-instance handling. |
| PVP-01 | Duel request/countdown/flag presentation, PvP-flag state on nameplates/unit frames, faction hostility coloring. |
| PVP-02 | Battleground (10v10 CTF-style): queue UI, scoreboard, flag-carrier presentation, objective HUD, BG-specific map overlay. |
| CHR-05 | Ground mount presentation: mount/dismount flow, mounted movement speeds matched to server validation, mount bar state. |
| UI-02 | Lua addon API v1 (§5.3). |
| TLS-08 | Client loads community content `.pck` packs: **the Client track owns the mount UX per sync decision D-09** — pack signing/trust prompt, per-realm content-manifest validation so a community zone loads only when the realm serves matching data (IT-M3 requirement). |
| WLD-01 (scale) | Multi-map/cross-instance transfer flows against the M3 multi-map server (transfer screens, seamless where server supports it). |
| **IT-M3** | See §10. |

---

## 5. UI framework

### 5.1 Recommendation: Godot Control nodes + theme system on a data-driven core

- **Godot's Control-node UI** with a single project-wide **Theme resource** (style boxes, fonts, colors) so the whole HUD reskins centrally; input routing via Godot's built-in focus/input-event propagation, with an activatable-panel stack (dialogs over HUD over world) implemented as a thin manager since Godot has no CommonUI equivalent — small, well-understood code.
- **Control scenes are views only.** All HUD state flows through a **UI model layer** (plain view-model objects fed by the GDExtension game systems, MVVM-style, surfaced to GDScript as observable objects/signals). No widget ever reads game state directly from entity scenes. This is the single most important architectural rule because it is what makes UI-02 possible: the Lua API binds to the model layer, not to Control-node internals. It is also the C++/GDScript boundary (§1.1).
- Perf discipline: no per-frame `_process` polling on HUD widgets; event-driven updates from the model layer (signals); Control-node pooling for floating combat text, nameplates, and buff icons; static chrome kept out of per-frame redraw paths (containers arranged once, `queue_redraw` only on change).

### 5.2 Core HUD — UI-01 (M1)

- **Unit frames:** player, target, target-of-target, party (M2). Health/resource bars, cast bar, buff/debuff rows (CMB-04), level/name/classification.
- **Action bars:** main bar + 2 extra bars at M1; drag-drop from spellbook, keybind display, GCD sweep + cooldown spirals + range/usability tinting (CMB-01), keybinding UI.
- **Bags:** bag bar, unified or per-bag view, item tooltips (ITM-01).
- **Minimap + world map (WLD-03):** minimap with rotation lock option, tracking icons, POI pins; full-screen world map with discovered-area reveal, quest objective areas (QST-01), player/party positions.
- **System panels:** chat (SOC-01), quest tracker (QST-01), character sheet (CHR-03), spellbook, settings (graphics tiers §2.2, audio, keybinds, interface).

### 5.3 Path to the Lua addon API — UI-02 (M3)

Phased so M1/M2 work is not thrown away:

1. **M1:** model-layer discipline (§5.1) enforced in review; every HUD feature lands with its data exposed via the model layer. A registry of named UI events/state (e.g. `UNIT_HEALTH_CHANGED`, `PLAYER_TARGET_CHANGED`) exists internally from M1 — the internal event bus **is** the future addon event system.
2. **M2:** internal "first-party addon" experiment — one HUD panel (e.g. quest tracker) rebuilt against the internal API only, proving the boundary; sandbox/security design review (no file IO outside addon dir, no network access from Lua, no protected-action calls in combat where design requires).
3. **M3 (UI-02 v1):** **embed Lua** (Lua 5.4 or LuaJIT — decision by M2 end) via the GDExtension layer, where a Lua VM embeds cleanly and sandboxing is a solved problem. (Alternative considered: a restricted, sandboxed GDScript subset — rejected because GDScript has no hardened sandbox story, and Lua is the WoW-addon-community lingua franca, which is the audience UI-02 exists for.) Expose: event registration, unit/state query API (read-only game state via model layer), widget creation API (a curated widget-template set over Control nodes, not raw scene-tree access), saved-variables per addon per character, addon loader with load-on-demand + error isolation (one broken addon must not kill the HUD). Documentation for community authors is part of the Definition of Done (Baseline §5.5).

Server involvement (marked ○ on UI-02): the server validates that no addon-driven message violates protocol constraints — the client API simply cannot construct raw network messages.

---

## 6. Content & asset consumption

- **Client packs from the content compiler `mcc` (TLS-01):** the compiler's second output (TD-06) is client-consumable **Godot `.pck` resource packs**: item/spell/quest/NPC display data, zone chunk scenes + manifests (§2.4), localization tables. The client ships a **pack loader** that mounts versioned `.pck`s at startup via `ProjectSettings.load_resource_pack()` (and on the fly for TLS-08 community content at M3 — mount UX Client-owned per D-09), with a manifest (content version, schema version, per-pack hashes) validated against the realm at connect — client and server must agree on the content version or the realm rejects the session with a clear message.
- **Asset-ID registry (Baseline §5.3):** all cross-references use stable asset IDs (`art.char.human.male.base`, `mus.zone01.explore.layer2`). The client maintains an asset-ID → `res://` resource-path resolution table (itself compiler-generated into the pack). Game systems and content data never contain resource paths. Unresolvable IDs resolve to typed placeholders (magenta-checker mesh, default icon, silence with log) — hard rule: **missing content is a visible placeholder + telemetry event, never a crash**.
- **Greybox content at M1:** Zone-01 is authored entirely in the Forge zone-editor plugin (TLS-02) inside the Godot editor; the client consumes its exported chunk scenes + manifest (§2.4) like any zone. NPCs/items/quests authored in the data editors (TLS-03/04/05) flow through `mcc` into packs; the client renders greybox mobs with the placeholder rig + art's M1 kit. The pipeline (tool → compiler → pack → client render) is exactly the shipping pipeline — greybox differs only in asset quality, per Pillar 1.
- **Patching granularity:** packs are chunked by zone/system so the M4 patcher (§8) can do partial updates; chunking scheme agreed with Tools track by M2.

---

## 7. Audio integration

The Client track owns the **runtime hooks**; the Music track owns the sound design and the `AudioStreamInteractive`/`AudioStreamSynchronized` resources (TD-11).

- **AUD-01 (SFX framework, M1):** the client emits a well-defined set of gameplay audio events — combat events (cast start/succeed/interrupt, melee impacts by weapon/armor type, crits), foley (footsteps by physical surface via surface-type queries, gear rustle), UI sounds (button, window open/close, error, loot, level-up). Each event carries context payloads (surface, material, intensity) and routes to audio-stream resources referenced **by asset ID**. Concurrency groups + priority ducking rules (audio bus layout, `AudioServer` bus effects) are client-owned; mix profiles are Music-track-owned.
- **AUD-02 (adaptive zone music, M1 basic / M2 full):** the client feeds a **music state component** with the parameters the Music track's layered system consumes: zone/subzone ID, explore/tension/combat state (combat state derived from server threat/combat flags), time-of-day, player death. The component drives clip/transition selection on the Music track's `AudioStreamInteractive` resources and layer sync via `AudioStreamSynchronized`; TD-11's decision gate (end of M0) covers whether bar-quantized transitions suffice or the custom GDExtension mixer fallback is needed — the client hook API is identical either way. M0 proves one adaptive zone track end-to-end (Baseline M0); M2 delivers combat/dungeon music-system hooks (GRP-02 instance music states, boss stingers).
- **AUD-03 (ambient beds & emitters, M2):** zone tools (TLS-02) place ambient emitters and bed volumes as content; the client instantiates them on chunk stream-in (WLD-01), with day/night/weather parameter routing (WLD-02).
- Settings: master/music/SFX/ambient sliders in the M1 settings UI (mapped to audio buses); audio scalability (max concurrent voices/polyphony) tied to the §2.2 presets.

---

## 8. Distribution

- **M0–M3 (test realm):** nightly client builds (Baseline §6) produced by Windows CI using **Godot export templates** (release export, `.pck`-chunked content) + a minimal **bootstrap updater** (delta-download changed packs by manifest hash, verify, swap). This is deliberately the embryo of the real patcher — testers stop doing full re-downloads from M1 on, and the patch pipeline gets ~2 years of hardening before launch. Because Godot's export is fast and deterministic relative to a UE cook, nightly build times are minutes, not hours — CI budget still tracked (§12).
- **Bot client stays M0-cheap:** Godot's `--headless` mode runs the same client binary with no renderer/audio, which is exactly what the bot client (§10.2) needs — no special null-RHI build flavor to maintain.
- **M4 (direction only, per Baseline M4):** a proper **launcher/patcher**: installer (MSI or NSIS), launcher app handling account news + realm status + patching (binary-delta pack updates, resumable, hash-verified, staged background download), and client build signing. Torrent/CDN distribution strategy, localization of the launcher, and accessibility review are M4-scoped decisions.
- Crash reporting (Windows crash handler + minidump upload — e.g. Crashpad/Breakpad in the GDExtension layer, since Godot ships no crash reporter — to a project-hosted Sentry-compatible endpoint) ships with the first test-realm build at end of M0 — nightly builds without crash telemetry are wasted testing.

---

## 9. Performance budgets

All budgets are gated on the **crowded-scene benchmark**: a scripted scene with **50+ visible player characters** (mixed gear), 20 mobs, combat VFX active, in the densest Zone-01 area — because IT-M1 puts 50+ concurrent players in one greybox zone. Budgets are enforced per tier:

### Frame-time (crowded scene)
| Tier | Total | Main thread | Render prep (RenderingServer) | GPU |
|---|---|---|---|---|
| Low (1060) | 33.3 ms (30 FPS) | ≤ 12 ms | ≤ 10 ms | ≤ 30 ms |
| High (3070) | 16.7 ms (60 FPS) | ≤ 8 ms | ≤ 7 ms | ≤ 14 ms |

Sub-budgets (all tiers, main thread): networking decode + apply ≤ 1.5 ms; entity interp/prediction ≤ 2 ms; animation (with crowd animation-rate throttling) ≤ 3 ms; UI ≤ 1.5 ms in combat; **GDScript total ≤ 2 ms in combat** (enforces the §1.1 language boundary).

### Memory
- Low tier: client fits in **6 GB VRAM** and ≤ 12 GB system RAM (16 GB machines with OS + background apps, TD-03). Texture budget enforced per tier via import settings + VRAM tracking in the nightly perf run; hard cap honored via quality reduction, never via crash.

### Streaming (WLD-01)
- No hitch > 50 ms during in-zone chunk streaming at run speed on the Low tier (SATA SSD is the Low-tier storage floor per TD-03; HDDs unsupported). Chunk instancing is time-sliced (§2.4) to meet this.
- Map-change load (instance portal, GRP-02) ≤ 30 s on Low tier.

### Entities & draw calls (crowded scene)
- Interest set: client handles ≥ 150 simultaneous entities (players+NPCs) without exceeding main-thread budget; presentation LOD ladder for crowds: full anim → reduced anim rate → static-pose imposters beyond N meters (N per tier).
- **Draw calls are the primary render constraint — there is no Nanite collapsing the static world (TD-02), so every draw is paid for.** Budget: ≤ ~2,000 draw calls on Low in the benchmark scene, achieved via MultiMesh/GPU instancing for crowds and kit pieces, mesh merging in the Forge chunk export, LOD visibility ranges, and occlusion culling. Triangle budget per tier set jointly with the Art PRD; both are hard gates at the M2 "beautiful corner" lock (§2.3).
- Floating combat text / nameplates: pooled, hard-capped counts with priority eviction (your target > party > others).

Budgets are tracked with Godot's built-in profiler + `RenderingServer` statistics captures (draw calls, primitives, video memory) exported from the nightly perf run on physical Low/High tier machines, plus Tracy instrumentation in the GDExtension modules; a >10% regression fails the nightly and pages the client team.

---

## 10. Testing

### 10.1 Automated
- **Unit/functional:** C++ **doctest** suites inside the GDExtension modules for the hot path — network module (framing, reconnect, schema-version mismatch), prediction/reconciliation math (deterministic replays of input+correction sequences), asset-ID resolution and placeholder fallback. **GUT (Godot Unit Test)** for GDScript-side logic: UI model layer, screen flows, settings.
- **Protocol conformance:** golden-message tests generated from `/schema` fixtures — client decode/encode round-trips validated against the same fixtures the server track uses (shared in the schema repo).
- **Screenshot/smoke:** nightly exported-build smoke on both tier machines — boot → login (against test realm) → enter world → run a fixed path → screenshot diff (tolerance-based) + perf capture (§9).

### 10.2 Replay & bot client
- **Headless bot client** built from the same network module, run via Godot `--headless` (no renderer/audio; same binary, same protocol code): scriptable login/move/cast/chat behaviors. Primary consumer is the Server track's load testing (500 CCU for IT-M3 / OPS-04), but the Client track owns building and maintaining it because it shares the protocol code. Deliverable at end of M0 (basic move bot), extended each milestone with that milestone's verbs (quest at M1, group/dungeon at M2, BG queue at M3).
- **Network replay:** record server message streams in the client; replay them deterministically into a client build for repro of presentation bugs and for the perf benchmark (the crowded scene in §9 is a canned replay, so it's identical run-to-run).

### 10.3 Integration test contributions & done-criteria

| IT | Client contribution | Client "done" means |
|---|---|---|
| **IT-M0** | Login UI → `authd` TLS → realm select → `worldd` handoff → character stub (D-11 scope) → empty map; predicted local movement; remote client interpolation. Bot client v0 available as the "other connected client". | Two real clients on two machines: both complete the full session flow; each sees the other move smoothly (interp, no teleporting) at ≤ 150 ms simulated latency; disconnect/reconnect works; zero crashes across a 30-min soak. |
| **IT-M1** | Everything in §4-M1: install (nightly build + bootstrap updater), character create, complete the 10-quest chain to level 5 in Zone-01 with 50+ concurrent players. | A fresh Windows machine (1060-class) installs the client, patches, creates a character, and finishes the quest chain with no client-side blocker; crowded-scene budget (§9) holds at Low/30 FPS during the 50-player gathering; all quest/combat/loot/vendor/chat UI flows complete without workaround. |
| **IT-M2** | Group formation UI, Dungeon-01 instancing flow, group loot rolls; crafting/AH/mail UIs for the economy loop (gather → craft → auction → mail). | A 5-player group forms, enters, clears Dungeon-01, and rolls loot entirely through shipped UI; one tester completes the full economy loop end-to-end in-client; instance map-change load within budget; no desync requiring relog. |
| **IT-M3** | 500-CCU alpha client stability; BG queue/scoreboard (PVP-02); guild/friends/LFG UI (SOC-02, GRP-03); community content pack loading (TLS-08). Bot fleet drives the CCU load. | Client remains within perf/memory budgets in a 500-CCU realm hotspot; a community-made zone pack loads and plays on an unmodified client against an unmodified server; LFG-formed group completes a dungeon; a full 10v10 BG match completes with correct scoreboard. |

---

## 11. Traceability table

Every feature where Client = ● in the baseline matrix (§4). Milestones are the baseline's, including split entries.

| Feature ID | Client deliverable(s) | Milestone |
|---|---|---|
| ACC-01 | Login UI, TLS `authd` client, realm list UI, error states | M0 |
| ACC-02 | Session token handoff to `worldd`, reconnect | M0 |
| CHR-01 | Character stub select — name + class over one placeholder model per D-11 (M0); full create/select, 1 race 2 classes, appearance | M0 stub / M1 |
| CHR-02 | Predicted movement + reconciliation (GDExtension), remote interp, camera (M0); swim, polish (M1) | M0 basic / M1 |
| CHR-03 | XP bar, level-up presentation, character sheet | M1 |
| CHR-04 | Talent/spec UI, respec flow | M2 |
| CHR-05 | Mount/dismount presentation, mounted movement | M3 |
| WLD-01 | Custom chunk-streaming module over Forge chunk exports (§2.4), streaming budgets, multi-map transfer flows (M3) | M1 |
| WLD-02 | Day/night lighting per tier strategy, weather VFX/audio states | M2 |
| WLD-03 | Minimap, world map, POI discovery presentation | M1 |
| CMB-01 | Targeting, nameplates, cast bars, GCD optimistic-start feedback (D-10), floating combat text, range/facing feedback | M1 |
| CMB-03 | Death/ghost/corpse-run/resurrect presentation | M1 |
| CMB-04 | Buff/debuff bars (M1); full aura presentation, dispel types (M2) | M1 basic / M2 |
| NPC-02 | Gossip, vendor, trainer windows | M1 |
| QST-01 | Quest indicators, offer/turn-in dialogs, log, tracker | M1 |
| QST-02 | Chain/prereq presentation, scripted event playback | M2 |
| ITM-01 | Bags, equipment paperdoll, tooltips | M1 |
| ITM-02 | Loot window, rarity presentation; group rolls with GRP-01 | M1 |
| ECO-01 | Currency, vendor buy/sell/buyback UI | M1 |
| ECO-02 | Profession/crafting/gathering UI | M2 |
| ECO-03 | Auction house UI with server-paged search | M2 |
| ECO-04 | Mail UI (compose, attachments, COD) | M2 |
| ECO-05 | Bank storage UI | M2 |
| SOC-01 | Chat panel, channels, slash commands, bubbles | M1 |
| SOC-02 | Friends/ignore, guild UI + guild chat | M3 |
| SOC-03 | Trade window UI, dual-accept flow | M2 |
| GRP-01 | Party frames, invites, group loot rolls | M2 |
| GRP-02 | Instance portal flow, Dungeon-01 client support | M2 |
| GRP-03 | LFG UI, queue/proposal/teleport flow | M3 |
| PVP-01 | Duel + PvP flag presentation | M3 |
| PVP-02 | BG queue, scoreboard, CTF objective HUD | M3 |
| UI-01 | Core HUD (Control nodes + theme): unit frames, action bars, bags, minimap/map; settings UI | M1 |
| UI-02 | Lua addon API v1 (GDExtension-embedded), sandbox, addon loader, docs | M3 |
| AUD-01 | Gameplay audio event hooks → AudioStreamInteractive/Synchronized resources (combat/foley/UI) | M1 |
| AUD-02 | Music state component (zone/combat/ToD params) | M1 basic / M2 |
| AUD-03 | Ambient emitter/bed instantiation on stream-in | M2 |
| TLS-02 | Client-side render/runtime support for Forge-authored chunk content | M1 |
| TLS-06 | Editor-connected client mode for live spawn/patrol preview | M2 |
| TLS-08 | Community `.pck` loading (mount UX Client-owned per D-09), trust prompts, realm manifest validation | M3 |
| OPS-02 | GM command console entry, GM presentation flags | M1 basic |

(Consulted-only ○ rows — CMB-02, ITM-03, NPC-01, TLS-01, OPS-03, OPS-04 — appear above only where the client has presentation obligations.)

---

## 12. Risks & open questions

### Risks
1. **D3D12 backend maturity.** D3D12 became Godot's Windows default only at 4.6; edge-case driver bugs (older GPUs, laptop hybrids) are likelier than on the long-soaked Vulkan path. Mitigation: physical 1060/3070 validation from M0 (§2.3), Vulkan backend kept buildable as a diagnostic fallback, crash telemetry from the first test-realm build (§8), GPU/driver matrix widened during M1 testing.
2. **No Nanite ceiling on art density.** Without virtualized geometry, art density is bounded by classic draw-call/triangle budgets, and "better than WoW" visuals must be won through art direction (TD-10), not brute geometry. Mitigation: budgets are hard gates co-owned with the Art PRD (§9), import-time validation, and the M2 "beautiful corner" lock is the go/no-go on density targets.
3. **Custom chunk streaming (WLD-01) is new engineering** where UE gave us World Partition for free — we own hitching, memory, proxy meshes, and the Forge chunk-format contract. Mitigation: format contract signed by M0 end; streaming module has dedicated hitch gates (§9) tested from M1 greybox; time-sliced instancing designed in from day one, not retrofitted.
4. **GDExtension API churn across Godot minors.** godot-cpp/extension_api compatibility can shift between 4.x minors, and we carry substantial C++ (net, prediction, streaming, Lua host). Mitigation: pin the engine version per milestone, vendored godot-cpp, upgrade only at milestone boundaries with a dedicated soak week; keep GDExtension surface thin (engine-agnostic cores, adapters at the edge).
5. **Custom netcode means we own every desync bug** (unchanged by the pivot — we were never using engine replication). Mitigation: deterministic replay tests (§10.2), shared golden fixtures with the server track, bot client at M0 so soak testing starts immediately.
6. **Custom kinematic movement controller** (§3.3) must match server simulation exactly across slopes, stairs, and swim transitions — feel/edge-case regressions are on us. Spike scheduled in M0 to lock the shared-constants approach and the physics-query method.
7. **Lua sandbox security (UI-02):** addon APIs are a classic exploit surface (automation, protected actions). Mitigation: read-only state API, no raw network access, protected-action design review at M2 before any implementation.
8. **50+ visible players in one greybox area (IT-M1)** stresses skeletal animation and nameplate/UI cost long before art density does — and without Nanite the crowd's draw calls compete with the world's. Mitigation: crowd LOD ladder + MultiMesh imposters + pooled UI are M1 deliverables, not polish.

### Resolved since v0.1 (now decisions, not questions)
- **Serialization:** FlatBuffers, decided baseline v0.2 (TD-07); links cleanly into the GDExtension net module (§1.1, §3.1).
- **Low-tier storage:** SATA SSD floor added to TD-03 (baseline v0.2); §9 streaming budgets assume it.
- **CHR-01 M0 stub scope:** resolved per sync decision **D-11** — name entry + class selection over one shared placeholder model (§4-M0).
- **GCD prediction (CMB-01):** resolved per sync decision **D-10** — optimistic client start with server rollback (§3.3, §4-M1).
- **TLS-08 mount UX:** Client-owned per sync decision **D-09** (§6, §4-M3).

### Open questions (baseline gaps — flagged, not invented around)
1. **Interrupted-session/queue UX at scale:** baseline defines 500+ CCU (OPS-04) but no login-queue feature ID. If world servers cap, the client needs a queue screen — where does that live (ACC-01 extension?) and which milestone?
2. **Localization:** M4 mentions localization as direction-only, but string externalization must start at M1 to be retrofittable. Client will externalize all strings from M1 (Godot translation resources); no feature ID exists for localization infrastructure — flag for a baseline amendment.
3. **Voice chat / ping systems:** now explicitly deferred to M4 planning in Baseline §7 — client assumes zero 1.0 scope; confirm no BG (PVP-02) design dependency on ping chat.
4. **TLS-06 client/editor boundary:** "live server preview" implies a client session connected to a dev `worldd` from inside the Forge editor plugin. Which track owns the connection UX inside the Godot editor plugin — Client or Tools? Proposed: Client owns the runtime module, Tools owns the editor UI; needs sign-off.
5. **Forge chunk-format ownership details (WLD-01):** the §2.4 contract puts the manifest schema in `/schema`, but versioning/migration policy for already-exported zones when the chunk format evolves (re-export vs. loader back-compat) needs a Client+Tools decision by M1 start.
