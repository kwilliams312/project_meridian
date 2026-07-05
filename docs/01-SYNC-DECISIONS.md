# Cross-Track Sync Decisions

**Version:** 1.3 — 2026-07-04 (§6 added: sharded-realm scale-up. §5: SAD reconciliation. §4: engine pivot UE5 → Godot 4.6)
**Purpose:** Resolutions for the conflicts and gaps the five track PRDs raised against Baseline v0.1. Decisions marked **[baseline]** are already folded into Baseline v0.2; the rest are binding interpretations the PRDs should be read against. Remaining items are in §3.

---

## 1. Resolved decisions

| # | Raised by | Question | Decision |
|---|-----------|----------|----------|
| D-01 | Server, Client | TD-07: FlatBuffers or protobuf? | **FlatBuffers** — zero-copy reads suit the hot movement/AoI path; client PRD already recommends it. **[baseline]** |
| D-02 | Server | ECO-01 "repair" at M1 conflicts with durability arriving in ITM-03 at M2. | Repair moves to **M2 with ITM-03** (durability & repair together). ECO-01 at M1 is currency + buy/sell only. **[baseline]** |
| D-03 | Server, Client | UDP movement: when is it decided, who owns it? | **Server track owns a go/no-go gate at end of M2**, driven by IT-M2 latency data on the test realm. TCP until then. **[baseline]** |
| D-04 | Server | No feature IDs for account registration, trade, banks, mounts. | Added: **ACC-03** registration (CLI M0 / web M3), **SOC-03** trade window (M2), **ECO-05** bank (M2), **CHR-05** ground mounts (M3). **[baseline]** |
| D-05 | Client | TD-03 lacks a storage assumption. | Min spec assumes **SATA SSD**; HDDs unsupported. **[baseline]** |
| D-06 | Client, Music | Login queue, localization, VO, voice/ping chat have no IDs. | All deferred to **M4 planning**; listed in Baseline §7. **[baseline]** |
| D-07 | Server, Client, Tools | TLS-06 live-preview ownership boundary. | Three-way split: **Server** owns the GM-authenticated hot-reload RPC on preview maps (per server PRD §5), **Tools** owns the edit→compile→push loop and iteration-time targets, **Client** owns joining/rendering preview maps. The server PRD's editor-channel design is the committed contract the tools PRD asked for. |
| D-08 | Tools | CMB-01 marks Tools ● but no editor covers ability/spell authoring. | Confirmed: **ability/spell authoring is Codex scope under TLS-03's umbrella** (abilities live with the NPC/class data model; spells reference SFX/VFX asset IDs per the music/art PRDs). No new feature ID. |
| D-09 | Tools, Client | Who owns community pack download/mount UX (TLS-08) client-side? | **Client track** — TLS-08 already carries a Client ● in the matrix. Client provides the mount/UX; `mcc`-verified pack format and ID-band namespacing (tools PRD) is the contract. |
| D-10 | Client, Server | May the client predict GCD/cast starts, given "server is law"? | Yes — client may **optimistically start GCD/cast presentation**; server rejection rolls it back within one RTT. Server remains authoritative for all outcomes (damage, resource spend, loot). |
| D-11 | Client | CHR-01 "M0 stub" scope. | M0 = **name + class selection over one placeholder character model**, no appearance customization. Full create/select with customization lands M1 per the matrix. |
| D-12 | Art, Music | AUD-03 ambient emitters: Art's role? | Art's ● = **visual counterpart assets** for ambient emitters (birds, insect swarms, waterfalls, fires); Music owns the sound beds/emitters themselves; Forge volumes (Tools) place both. |
| D-13 | Music | Buff/debuff audio (CMB-04) has no Music ●. | Covered under **AUD-01 SFX framework** — no matrix change; CMB-04 sound hooks are data-driven from content files per music PRD §7. |
| D-14 | Music | MetaSounds vs audio middleware. | MetaSounds-first; **decision gate at end of M0** after the pipeline-proof adaptive track (music PRD §2). If MetaSounds can't hit bar-quantized transitions reliably, revisit — but middleware must stay license-compatible (Apache-2.0 project; FMOD/Wwise licensing likely disqualifying for open source). |
| D-15 | Server, Art | Navmesh generation has no feature ID. | It's a **TLS-02 deliverable** (Forge exports navmesh to `/content`, per tools PRD §3); server consumes it. No new ID. |
| D-16 | Server, Client | OPS-02 GM tooling: UI ownership. | **Client** owns the in-game GM command UI; **Server** owns command execution/permissions; **Codex (Tools)** owns offline moderation/data-inspection tooling. |

## 2. Confirmed interpretations (no conflict, recorded for the record)

- **Clean-room stance:** CMaNGOS/TrinityCore are architectural references only; no GPL code enters the Apache-2.0 codebase. Both server PRD §1 and README state this.
- **Single-funnel content rule:** only `mcc` (the content compiler CLI) produces runtime artifacts — Forge, Codex, CI, and community packs all go through it. This is the enabling constraint for TLS-08.
- **Asset IDs everywhere:** content references assets by stable ID with a committed `idmap.lock` for numeric DB keys (tools PRD); art and music PRDs both adopt the ID registry with provenance sidecars.
- **Crowded-scene benchmark:** the client's canned-replay 50-player benchmark is the shared perf gate; art budgets (≤2,500 draw calls Low), music voice caps (64 global), and client frame budgets all validate against it at IT-M1.
- **Bot client:** built by the Client track at M0, doubles as the Server track's 500-CCU load-test driver for OPS-04.

## 3. Open action items (owners assigned, not yet resolved)

| # | Item | Owner | Due |
|---|------|-------|-----|
| A-01 | World design doc: canonical names/biomes for Zone-01..04, Dungeon-01/02, battleground map | Design (cross-track) | Before M1 zone kickoff |
| A-02 | Legal review: distributing Forge as a UE editor plugin to community creators (Epic launcher vs source build, EULA implications) | Tools + legal | Before M3 community release |
| A-03 | CHR-01 appearance-customization depth (slider count, morphs vs presets) — drives Art scope at M1 | Art + Client | M0 exit |
| A-04 | WLD-02 weather taxonomy (which weather states exist) — drives Art VFX, Music ambience variants, Forge authoring UI | Design (cross-track) | M1 |
| A-05 | UI-02 addon API: whether community addons may reuse the game's UI atlas art | Client + Art | M2 |
| A-06 | Audio options UI (channel volumes, voice-count tiers) ownership — proposed: Client owns UI, Music defines the mix buses | Client + Music | M1 |
| A-07 | Community content pack provenance enforcement for audio/art inside packs (TLS-08 × TD-09) | Tools | M3 |
| A-08 | Chunk export format v1 sign-off in `/schema` (Forge owns format; Client owns streamer; Server consumes cell metadata) + versioning/migration policy (re-export vs loader back-compat) | Tools + Client | Format: M0 exit; policy: M1 start |
| A-09 | Terrain spike: evaluate Terrain3D (MIT) — adopt/fork-and-vendor vs build our own terrain GDExtension | Tools | M0 |

---

## 4. Engine pivot: UE5 → Godot 4.6 (2026-07-04, Baseline v0.3)

**Decision D-17:** Unreal Engine 5 is replaced by **Godot 4.6+** across client and tools. Rationale: Epic's EULA prohibits redistributing the engine and encumbers editor-plugin distribution — incompatible with a fully open-source project and with shipping Forge to community creators. Godot is MIT end to end; D3D12 is its default Windows backend since 4.6, satisfying the DX12 requirement. Alternatives considered: O3DE (Apache-2.0, higher graphics ceiling, but small/declining community — abandonment risk), Stride (MIT, C# synergy with Codex, smallest ecosystem), UE5-hybrid (game code open, engine proprietary — rejected as not truly open source).

Effects on earlier decisions:
- **A-02 (Forge plugin legal review): RESOLVED** — the Godot editor is MIT; Forge ships freely as part of a "Meridian Creator Kit."
- **D-14 (MetaSounds vs middleware): SUPERSEDED** — adaptive music now targets Godot `AudioStreamInteractive`/`AudioStreamSynchronized`; fallback is a custom GDExtension mixer, not commercial middleware (FMOD/Wwise license-incompatible with the open stack). M0 decision gate stands.
- **New sourcing constraint (TD-09):** Quixel Megascans / Fab / Unreal Marketplace assets are disallowed (engine-locked licenses). Art sourcing shifts to CC0 libraries (PolyHaven, ambientCG, Kenney, Sketchfab-CC0) + AI + original.
- **New cross-track contract:** UE World Partition is gone, so WLD-01 zone streaming is a custom chunk-streaming system — **Forge (Tools) owns the chunk export format; Client owns the runtime streamer.**
- **New risks accepted:** no Nanite equivalent (strict LOD/draw-call discipline, lower env-density ceiling), Godot D3D12 backend maturity edge cases, custom terrain tooling (evaluate Terrain3D plugin at M0), GDExtension API churn across Godot minors.

---

## 5. SAD reconciliation (2026-07-04, after the five `docs/sad/` documents landed)

| # | Raised by | Question | Decision |
|---|-----------|----------|----------|
| D-18 | Tools vs Art, Music | Tools' IF-8 sidecar draft is a minimal envelope; Art and Music each require richer fields. | **IF-8 v1 = Tools' core envelope + per-class extension blocks, union of all three lists.** Core: `id, class, source, license (SPDX allowlist), provenance{source_tier, origin_url, authors, attribution, license_verified_on, ai{tool, prompts_file}, transform_notes}`. Art block: `import_hints{lod_policy, lightmap_uv2, occluder, multimesh_safe}, contract_envelope, restyle_status, reviewed_by, tags`. Audio block: `loudness{lufs_integrated, true_peak_dbtp}, music{stem_set, layer, bpm, time_signature, length_bars, key, loop}, sfx{category, variation_group, attenuation}, encode{tier, preload}`. Tools authors `asset.schema.yaml` from this union (A-12); lint rules L020–L022 replace the validator's "pending asset registry" info line. |
| D-19 | Server | IF-6 cell metadata is an M0 movement-validation dependency, but the A-08 chunk contract signs only at M0 exit. | **M0 runs on a flat bootstrap test map with bounds-only movement validation** (no heightfield/navmesh consumption). Full IF-6-driven validation lands M1 with Zone-01 greybox. Removes the circular dependency without moving A-08. |
| D-20 | Tools, Client | Is Tools' IF-6 draft (128 m chunks, zone-local coordinates permanently — teleport zone transitions, 1 m heightfield, 32 m Recast tiles, proxy LOD rings, `format_version`) the working baseline? | **Yes, adopted as working baseline.** The A-08 sign-off is now a requirements walk: Client's nine-point list (client SAD §5.3 — asset-ID scene refs, load-priority hints, per-chunk AABB + content hash, ambient placements, N/N−1 loader compat, deterministic export, AoI grid alignment) checked line-by-line against tools SAD §3, plus Server's cell-metadata needs. Zone-local coordinates also retroactively settle the coordinate question left open in `position` (Content Schema v1). |
| D-21 | Tools, Server | mcc's dev-realm SQL push (distinct from IF-7 preview reload) has no interface number. | Registered as **IF-10** in the Architecture Overview: `mcc → dev/test-realm world DB push + worldd reload signal`. Server owns the reload-signal side, Tools the push side. Design M1 with TLS-06. |
| D-22 | Server, Client | IF-2 protocol details missing from the overview registry: clock-sync message (client interpolation), reconnect window in the session grant (IF-3), AEAD nonce budget under a future UDP move. | All three are **M0 protocol-design work items inside IF-1/2/3**, owner Server with Client sign-off; the AEAD nonce re-costing is added to the D-03 UDP gate checklist. |

New action items:

| # | Item | Owner | Due |
|---|------|-------|-----|
| A-10 | `.mcpack` community-pack signing/trust model (unsigned-but-hashed at M2? signed at M3?) — joins the D-09 mount UX and TLS-08 story | Tools + Server + Client | design by M2 exit |
| A-11 | IF-1/2/3 detailed protocol design (framing, clock sync, reconnect window, session-token format) — first `/schema/net/*.fbs` drafts | Server + Client | M0 |
| A-12 | Author `schema/content/asset.schema.yaml` per D-18 union + example sidecars + validator L020–L022 | Tools (+ Art/Music review) | M0, next work item |

---

## 6. Sharded-realm scale-up (2026-07-04, Baseline v0.4)

**Decision D-23: realm/shard model.** A **realm** is the shared logical server: one auth domain, one character DB, one social graph, one economy. A **shard** is a running copy of open-world zone simulation inside a realm; dungeon/BG instances are the pre-existing special case. OPS-04 is revised from "500+ CCU per world server" to **3000+ CCU per realm** via horizontal shard workers. Player-visible shard mechanics get feature ID **WLD-04** (M3): party auto-merge to leader's shard, "join friend" transfer, transfer masking UX.

Binding architecture rules (expanded in server SAD §sharding):

1. **Gateway owns the client connection.** Clients connect to a gateway process, never directly to shard workers. A shard transfer is a server-side reroute — no reconnect, no loading screen within a zone.
2. **Realm coordinator** manages shard lifecycle: spins zone shards up/down against a population band (target 80–150 players per open-world zone shard, hysteresis to prevent flapping), places players on zone-enter (party > friends > guild > load balance), and brokers transfers.
3. **Shard workers** are the existing map processes (one process = N map threads); capacity target 500–750 CCU per worker; 3000+ CCU ≈ 5–6 workers plus gateway/coordinator/services.
4. **Realm-global services** (chat channels, guilds, friends, groups, mail, AH) live above shards in a services process — a single economy and social space regardless of shard. Spatial chat (/say, /yell, emotes) is shard-local; zone general channel is realm-wide; whisper/party/guild are realm-global.
5. **Transfer protocol:** eligibility check (no combat lock, cooldown, capacity) → target shard pre-spawns the player entity → gateway atomically reroutes the session → source shard despawns → client receives an AoI refresh (despawn/respawn of surroundings, own position preserved). Same-zone only; cross-zone "join friend" is normal travel plus a shard pin.
6. **Anti-abuse:** transfer cooldown (~30 s), combat/loot-roll lock; per-shard gathering nodes and rares are a known farm vector — mitigation design is an open question for M2.
7. The M0 message-bus rule (server SAD) already makes this a transport/topology change, not a rewrite; the gateway split lands **M2**, full sharding **M3**.

Consequences for other tracks / registry:
- **IF-2** gains shard-transfer + AoI-refresh messages; **IF-3** session grants become gateway-scoped. Folded into the A-11 protocol design scope.
- **Client** owns WLD-04 UX: transfer masking (despawn/respawn smoothing), party-merge prompts, join-friend entry points, and nameplate/social indicators of shard co-presence.

| # | Item | Owner | Due |
|---|------|-------|-----|
| A-13 | Client PRD/SAD: add WLD-04 rows (transfer UX, AoI-refresh handling in `sim`, join-friend UI) | Client | next client-doc revision |
| A-14 | Per-shard resource farming mitigation design (node/rare tagging vs shared spawns vs diminishing returns) | Server + Design | M2 |
