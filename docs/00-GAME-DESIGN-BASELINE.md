# Game Design Baseline (GDB)

**Project:** Open-source 3D MMORPG (working title: **Project Meridian**)
**Version:** 0.6 — 2026-07-05 (v0.6: **OPS-05 player-experience telemetry & observability** added per D-29 — server-side-first UX capture, OpenTelemetry-compatible export, client ERROR/CRITICAL log channel, Grafana dashboards as versioned deliverables. v0.5: **macOS client added** per D-28 — TD-01/02/03 revised for a phased Apple-Silicon client (CI builds M0, supported test-realm client M1, launch platform 1.0, native Metal backend); §7 out-of-scope updated; tools remain Windows-only (TD-08 unchanged). v0.4: **sharded-realm scale-up** — OPS-04 revised to 3000+ CCU per realm via zone sharding, WLD-04 added for player shard transfer; see docs/01-SYNC-DECISIONS.md §6. v0.3: engine pivot UE5 → Godot 4.6 — TD-01/02/08/09/11 revised, Quixel/Fab sourcing disallowed. v0.2: post-PRD reconciliation — TD-07 FlatBuffers decided, SSD added to TD-03, repair moved M1→M2, feature IDs ACC-03/CHR-05/SOC-03/ECO-05 added)
**Status:** Foundation document. Every track PRD (Art, Music, Client, Tools, Server) MUST reference this document by feature ID and milestone. Changes to this document require a cross-track review.

---

## 1. Vision

A free, open-source, WoW-style themepark MMORPG: tab-target combat with action elements, zone-based open world, quests, dungeons, itemization, and social systems. Built on Godot 4 (MIT-licensed engine — the entire stack, editor included, is open source and redistributable) with a custom Linux server, and — critically — a **tools-first philosophy**: the same user-accessible creation tools we ship to the community are the tools we use to build the initial world.

### Pillars
1. **Tools-first.** Zones, NPCs, mobs, items, and quests are authored in shipped, user-accessible tools. If the team can't build the world with the tools, the tools aren't done.
2. **Runs on real hardware.** High visual quality (better than WoW) that scales from GTX 1060-class machines to high end. DX12 required; hardware ray tracing NOT required and never assumed.
3. **Server is law.** Authoritative Linux server, CMaNGOS/TrinityCore-inspired architecture. Client predicts, server decides.
4. **Content is data.** All game content lives in a versioned, Git-friendly content database — not in code, not in binary blobs.
5. **Legally clean.** WoW-inspired playstyle only. No Blizzard assets, names, formats, or code. CMaNGOS is an architectural reference, not a code source.

---

## 2. Binding technical decisions

| ID | Decision |
|----|----------|
| TD-01 | Client: **Godot 4.6+**, Windows native x64 **and macOS Apple Silicon (arm64)** at launch (D-28; macOS phased: CI builds M0 → supported test-realm client M1 → launch platform 1.0). Engine is MIT-licensed — full stack (editor included) is open source and redistributable. Perf-critical client code (netcode, prediction, streaming) in C++ via GDExtension (universal per-platform builds); UI/glue in GDScript. |
| TD-02 | Rendering: Godot **Forward+ renderer** — **Direct3D 12** on Windows (default backend since Godot 4.6), **native Metal** on macOS Apple Silicon (default there since 4.4); Vulkan (MoltenVK on macOS) kept buildable as a diagnostic fallback on both. No hardware RT dependency — SDFGI on higher tiers, baked lightmaps + reflection probes on low end. No Nanite equivalent exists: strict traditional LOD/occlusion discipline is mandatory (budgets in Art PRD). |
| TD-03 | Scalability: playable at 30 FPS / 1080p Low on GTX 1060 6GB / 16GB RAM class **and Apple M1 8 GB unified** (D-28); 60 FPS / 1440p High on RTX 3070 class / M2 Pro-class (RT features still unused). Min-spec storage assumption: SATA SSD (HDDs unsupported; all Apple Silicon Macs qualify). The 1060 bench machine remains the authoritative min-spec gate; the M1 is the second Low-tier reference from M1. |
| TD-04 | Server: Linux (Ubuntu LTS primary target), C++20, CMake. Two daemons minimum: `authd` (login/realm list) and `worldd` (game simulation), CMaNGOS-style. |
| TD-05 | Database: MariaDB/MySQL for server runtime (auth DB, characters DB, world DB). |
| TD-06 | Content pipeline: human-editable source-of-truth content files (YAML/JSON, Git-friendly) compiled by tooling into (a) world-DB SQL for the server and (b) client data paks. One compiler, two outputs. |
| TD-07 | Network: custom binary protocol over TCP (TLS for auth), schema-defined with **FlatBuffers** (decided v0.2) in a single shared schema repo consumed by client and server. Movement may move to UDP later; Server track owns a go/no-go decision gate at end of M2. |
| TD-08 | Tools: Windows native x64. Zone/world building = **Godot editor plugin suite ("Forge")** — legally redistributable to the community since the Godot editor is MIT. Data editors (NPC/item/quest/loot) = standalone desktop app ("Codex"); both write to the content-source format (TD-06). |
| TD-09 | Licensing: code Apache-2.0 (clean-room; no GPL code copied from CMaNGOS/TrinityCore), original art/music CC-BY 4.0, third-party assets must be CC0/CC-BY only; **engine-locked marketplace assets (Quixel/Fab/Unreal Marketplace, Unity Asset Store) are disallowed** — their licenses bind to those engines. AI-generated assets allowed with provenance recorded per asset. |
| TD-10 | Art direction: stylized-realistic ("painterly PBR") — readable silhouettes and strong color scripts over photorealism. |
| TD-11 | Audio: Godot audio system — adaptive layered music per zone (explore / tension / combat layers) built on `AudioStreamInteractive`/`AudioStreamSynchronized` (Godot 4.3+), with a custom GDExtension mixer as fallback if bar-quantized transitions prove insufficient (decision gate end of M0). |
| TD-12 | Repo layout: monorepo `open-mmo/` with `/client`, `/server`, `/tools`, `/content`, `/schema`, `/docs`; large art binaries via Git LFS. |

---

## 3. Milestones (all tracks synchronize on these)

Every PRD must organize deliverables under these milestone names. The **exit criterion of every milestone is a cross-track integration test** — client + server + tools + content tested together.

### M0 — Foundation (months 0–4)
Goal: everything boots and talks.
- Repo, CI (Linux server builds; Windows client/tools builds; macOS client export, boot-smoke only per D-28), shared network schema v1.
- Server: `authd` login, `worldd` accepts a connection, character stub, echo world state.
- Client: Godot project, login screen → server session → character loads into an empty test map, basic movement replicated.
- Tools: content-compiler v0 (YAML → SQL + client pak), NPC/item editors alpha.
- Art: art bible, style tests, pipeline (DCC → glTF → Godot) proven with 1 character + 1 environment kit.
- Music: audio direction doc, pipeline proven with 1 adaptive zone track in Godot's interactive-music system.
- **Integration test IT-M0:** log in through `authd`, enter world on `worldd`, move a character, see another connected client move.

### M1 — Greybox Vertical Slice (months 4–9)
Goal: the full gameplay loop, ugly on purpose. **Tools track is the critical path.**
- One greybox zone (Zone-01) built entirely with the zone tools.
- macOS client reaches **supported test-realm status** (signed + notarized nightly builds, M1-Mac Low-tier validation) per D-28.
- 1 playable race, 2 classes (1 melee, 1 caster), levels 1–10.
- Tab-target combat vs. mobs (aggro, leash, respawn), death/resurrect.
- 3 quest types working end-to-end (kill, collect, deliver), quest tracker UI.
- Loot, inventory, equipment, vendor buy/sell, basic chat (say/whisper/general).
- All NPCs/mobs/items/quests in Zone-01 authored via the data editors.
- **Integration test IT-M1:** a new player installs client, creates a character, completes a 10-quest chain to level 5 on a public test realm with 50+ concurrent players.

### M2 — Systems Depth (months 9–15)
Goal: the systems that make it an MMO.
- Talents/specialization, full stat & itemization model (rarity, budgets, procs).
- Groups/party, first 5-player dungeon (instancing), threat mechanics.
- Crafting (2 professions + gathering), auction house, mail.
- Zone-01 fully art-passed (the "beautiful corner" for the whole game); Zone-02 greybox.
- Adaptive music fully in for Zone-01; combat/dungeon music systems.
- **Integration test IT-M2:** 5-player group clears Dungeon-01; economy loop (gather → craft → auction → mail) works on test realm.

### M3 — Alpha World (months 15–24)
Goal: a real (small) world.
- 4 zones art-complete, 2 dungeons, 1 battleground (10v10), 2 races, 4 classes, levels 1–30.
- Guilds, friends/ignore, LFG tool.
- Server: full sharded-realm architecture (OPS-04) — gateway, realm coordinator, shard workers, realm-global services; player shard transfer live (WLD-04). Architecture rated for 3000+ CCU per realm.
- Community tools release: external creators can build and locally host a custom zone.
- **Integration test IT-M3:** closed alpha, 1500 CCU sustained across ≥3 zone shards with live party auto-merge and join-friend transfers; community-made zone loads on an unmodified server. (Full 3000+ CCU load test is an M4 gate using bot fleets.)

### M4 — Open Beta → 1.0 (months 24+)
Goal: hardening, content breadth, launch. Scope set at end of M2; PRDs describe only direction (localization, accessibility, launcher/patcher, moderation tooling, first raid).

---

## 4. Canonical feature matrix

Feature IDs are permanent. PRDs reference them in a traceability table: `feature ID → this track's deliverable(s) → milestone`. Tracks marked ● have deliverables for that feature; ○ = consulted.

| ID | Feature | Client | Server | Tools | Art | Music | Milestone |
|----|---------|:------:|:------:|:-----:|:---:|:-----:|-----------|
| ACC-01 | Account auth & realm list | ● | ● | ○ | ○ | ○ | M0 |
| ACC-02 | Session/handoff auth → world | ● | ● | | | | M0 |
| ACC-03 | Account registration (CLI at M0, web signup by M3) | ○ | ● | ○ | | | M0 basic / M3 |
| CHR-01 | Character create/select (race, class, appearance) | ● | ● | ○ | ● | ● | M0 stub / M1 |
| CHR-02 | Movement & replication (walk/run/jump/swim) | ● | ● | | ● | | M0 basic / M1 |
| CHR-03 | Stats, leveling, XP | ● | ● | ● | | | M1 |
| CHR-04 | Talents/specs | ● | ● | ● | | | M2 |
| CHR-05 | Ground mounts | ● | ● | ● | ● | ● | M3 |
| WLD-01 | Zone streaming & spatial partitioning (custom chunk streaming) | ● | ● | ● | ● | ● | M1 |
| WLD-02 | Day/night, weather | ● | ● | ● | ● | ● | M2 |
| WLD-03 | Points of interest, discovery, world map | ● | ● | ● | ● | ● | M1 |
| WLD-04 | Realm sharding & player shard transfer (party auto-merge, "join friend", transfer UX) | ● | ● | ○ | | | M3 |
| CMB-01 | Tab-target combat core (GCD, cast/instant, range) | ● | ● | ● | ● | ● | M1 |
| CMB-02 | Mob AI: aggro/threat/leash/respawn | ○ | ● | ● | | | M1 |
| CMB-03 | Death, resurrect, corpse run | ● | ● | ○ | ● | ● | M1 |
| CMB-04 | Buffs/debuffs/auras | ● | ● | ● | ● | | M1 basic / M2 |
| NPC-01 | NPC definitions, spawns, patrol paths | ○ | ● | ● | ● | | M1 |
| NPC-02 | Gossip/dialogue, vendors, trainers | ● | ● | ● | | | M1 |
| QST-01 | Quest system (kill/collect/deliver), tracker | ● | ● | ● | | ● | M1 |
| QST-02 | Quest chains, prerequisites, scripted events | ● | ● | ● | | | M2 |
| ITM-01 | Items, inventory, equipment | ● | ● | ● | ● | | M1 |
| ITM-02 | Loot tables & drops | ● | ● | ● | | ● | M1 |
| ITM-03 | Itemization model (rarity, stat budgets, durability & repair) | ○ | ● | ● | ● | | M2 |
| ECO-01 | Currency, vendors buy/sell (repair moved to ITM-03, M2) | ● | ● | ● | | | M1 |
| ECO-02 | Crafting & gathering professions | ● | ● | ● | ● | | M2 |
| ECO-03 | Auction house | ● | ● | ● | | | M2 |
| ECO-04 | Mail | ● | ● | ○ | | | M2 |
| ECO-05 | Bank storage | ● | ● | ○ | ● | | M2 |
| SOC-01 | Chat channels (say/whisper/general) | ● | ● | | | | M1 |
| SOC-02 | Friends/ignore, guilds | ● | ● | | | | M3 |
| SOC-03 | Player-to-player trade window | ● | ● | | | | M2 |
| GRP-01 | Party/group, group loot | ● | ● | | | | M2 |
| GRP-02 | Dungeon instancing | ● | ● | ● | ● | ● | M2 |
| GRP-03 | LFG tool | ● | ● | | | | M3 |
| PVP-01 | Duels, PvP flagging | ● | ● | | | | M3 |
| PVP-02 | Battleground (10v10 CTF-style) | ● | ● | ● | ● | ● | M3 |
| UI-01 | Core HUD (unit frames, action bars, bags, map) | ● | ○ | | ● | | M1 |
| UI-02 | UI addon/scripting API (Lua) | ● | ○ | ● | | | M3 |
| AUD-01 | SFX framework (combat, foley, UI) | ● | | ● | | ● | M1 |
| AUD-02 | Adaptive zone music system | ● | | ● | | ● | M1 basic / M2 |
| AUD-03 | Ambient beds & emitters | ● | | ● | ● | ● | M2 |
| TLS-01 | Content compiler (YAML → server SQL + client paks) | ○ | ● | ● | | | M0 |
| TLS-02 | Zone editor (Godot editor plugin: terrain, kits, spawns, volumes) | ● | ○ | ● | ● | ● | M1 |
| TLS-03 | NPC/mob editor (stats, AI params, loot links) | | ● | ● | ○ | | M0 alpha / M1 |
| TLS-04 | Item editor | | ● | ● | ○ | | M0 alpha / M1 |
| TLS-05 | Quest editor (graph-based chains) | | ● | ● | | | M1 |
| TLS-06 | Spawn/patrol placement in-editor with live server preview | ● | ● | ● | | | M2 |
| TLS-07 | Content validation & CI linting | | ● | ● | | | M1 |
| TLS-08 | Community packaging: build/share/load custom content | ● | ● | ● | | | M3 |
| OPS-01 | Server config, deployment (Docker), monitoring | | ● | ○ | | | M0→ |
| OPS-02 | GM commands & moderation | ● | ● | ● | | | M1 basic |
| OPS-03 | Anti-cheat: server validation, rate limits | ○ | ● | | | | M1→ |
| OPS-04 | Scale: sharded realm architecture — 3000+ CCU per realm (gateway + coordinator + shard workers + realm-global services); dynamic zone-shard spin-up/down | ○ | ● | | | | M2 gateway / M3 |
| OPS-05 | Player-experience telemetry & observability (D-29): server-side UX capture (RTT, corrections, disconnects, errors), OpenTelemetry-compatible export, client ERROR/CRITICAL log channel, provisioned Grafana dashboards + alerts | ● | ● | | | | M0→ |

---

## 5. Cross-track contracts

1. **Schema repo (`/schema`)** — the network protocol and content-file schemas are versioned here. Client, server, and tools consume generated code from it. A schema change requires client+server+tools sign-off.
2. **Content database (`/content`)** — YAML source of truth for all game data (TD-06). Tools write it, the compiler (TLS-01) transforms it, server and client consume the outputs. Art and music assets are referenced from content files by stable asset IDs.
3. **Asset ID registry** — every art/music/SFX asset gets a stable ID (`art.char.human.male.base`, `mus.zone01.explore.layer2`); content files reference IDs, never file paths.
4. **Integration tests (IT-M0…IT-M3)** — defined above; each PRD must state what that track contributes to each IT and what "done" means for it.
5. **Definition of done (all tracks):** feature works in the cross-track integration test on the test realm, authored through tools where applicable, documented for community contributors.

## 6. Testing-as-we-go

- A persistent **test realm** stands up at end of M0 and is redeployed nightly from `main` (server) + latest client build + latest compiled content.
- Every feature lands behind the rule: server logic + client presentation + tool authoring path + test on the realm, in the same milestone.
- Tools track owns "content CI": every merge to `/content` is compiled and validated (TLS-07) before deploy.

## 7. Out of scope for 1.0

Linux/console clients (macOS is in scope per D-28; Intel Macs are not — Apple Silicon only), hardware ray tracing features, player housing, flying mounts, cross-realm tech, cash shop/monetization, mobile companion app. Deferred to M4 planning: voice-over, localization infrastructure, login queue, voice/ping chat.
