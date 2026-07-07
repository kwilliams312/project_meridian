# Cross-Track Sync Decisions

**Version:** 1.13 — 2026-07-07 (§15 added: D-36 — TD-11 music-timing gate run **under load**; the modelled under-load pass (music SAD §3.1, all five criteria, 10/10 seeds, wide margins) supports **native Godot audio**, so the provisional ruling is **native — GDExtension mixer held in reserve** (#147); the ruling is **ratified by the owner-runtime measured `--source godot` pass** under the #111 bot fleet on min-spec, which CI cannot run (#283/#146); evidence in [docs/td11-gate-decision.md](td11-gate-decision.md); no baseline change. §13 added: D-34 — keep the **monorepo through M2**; a Server/Client/Tools/Assets multi-repo split is deferred and **revisited at M3**, gated on a hard prerequisite — the shared wire contract + shared movement/crypto constants must first be extracted into a versioned `meridian-protocol` package; tracked by epic #266 (#267–#271); no baseline change, no code impact now. §12 added: D-33 — client engine pin bumped Godot 4.6 → 4.7-stable (#262), owner decision; no baseline matrix change. §7.2 added: A-03 RESOLVED — CHR-01 appearance customization is **presets-first** (discrete preset IDs: hair/face/skin + class/race options) plus an optional 1–2 continuous morphs if the blend-shape budget allows, and the server-persisted appearance data is a **versioned, extensible appearance record** so post-1.0 presets/morphs are additive with no breaking schema change (D-32). §11 added: A-09 terrain decision — Terrain3D fork-and-vendor, no baseline change. §7.1: A-15 RESOLVED — asset source + IF-8 sidecars live pack-local at `content/<ns>/assets/**` (D-31), ratifying the existing `core` layout. §10 added: server packaging & CD, D-30 — no baseline change, OPS-01 extension. §9: telemetry & observability, D-29/Baseline v0.6. §8: macOS client, D-28/Baseline v0.5. §7: Content Schema v1.1 reconciliation — A-12 discharged, D-24..D-27, A-15 opened, A-13 done. §6: sharded-realm scale-up. §5: SAD reconciliation. §4: engine pivot UE5 → Godot 4.6)
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
| A-03 | ~~CHR-01 appearance-customization depth (slider count, morphs vs presets) — drives Art scope at M1~~ **RESOLVED (D-32, §7.2):** presets-first (discrete preset IDs) + optional 1–2 morphs if budget allows; server persists a **versioned, extensible appearance record** (additive post-1.0, no breaking migration) | Art + Client | M0 exit |
| A-04 | WLD-02 weather taxonomy (which weather states exist) — drives Art VFX, Music ambience variants, Forge authoring UI | Design (cross-track) | M1 |
| A-05 | UI-02 addon API: whether community addons may reuse the game's UI atlas art | Client + Art | M2 |
| A-06 | Audio options UI (channel volumes, voice-count tiers) ownership — proposed: Client owns UI, Music defines the mix buses | Client + Music | M1 |
| A-07 | Community content pack provenance enforcement for audio/art inside packs (TLS-08 × TD-09) | Tools | M3 |
| A-08 | Chunk export format v1 sign-off in `/schema` (Forge owns format; Client owns streamer; Server consumes cell metadata) + versioning/migration policy (re-export vs loader back-compat) | Tools + Client | Format: M0 exit; policy: M1 start |
| A-09 | Terrain spike: evaluate Terrain3D (MIT) — adopt/fork-and-vendor vs build our own terrain GDExtension | Tools | **RESOLVED — §11 (fork-and-vendor Terrain3D)** |

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

---

## 7. Content Schema v1.1 reconciliation (2026-07-04, review pass)

A full-repo consistency review surfaced contract drift between the schemas, the reference validator, and the track documents. Resolutions:

| # | Raised by | Question | Decision |
|---|-----------|----------|----------|
| D-24 | Music vs schema | Music PRD/SAD use first-class `amb.*` asset IDs (`amb.zone01.bed.night`), but the ID grammar, `common.defs.yaml`, and the validator only knew `art/mus/sfx` — example content worked around it with `sfx.amb.*`. | **`amb.` is a first-class asset-ID prefix.** Added to the schema README grammar, `common.defs.yaml` (`ambRef`, `assetId`), the validator's asset regex, and `zone.ambience` (now `ambRef`). Example content migrated (`core:amb.zone01.forest_day`). |
| D-25 | Review | `kobold_miner` defines a money range on the NPC (`loot.money`) *and* on its loot table — semantics were unspecified (override? additive?). | **Additive:** both ranges are rolled and summed. Documented in `npc.schema.yaml` and `loot.schema.yaml`. |
| D-26 | Tools PRD vs loot schema | Tools PRD §4.4 says loot tables allow "one level of nesting v1"; `loot.schema.yaml` said "max depth 3" and cited lint **L020** — an ID the D-18 reconciliation reassigned to asset-registry checks. | **One level of nesting in v1** (a nested table may not itself reference tables), enforced by new lint **L052**. The vendor price rule similarly moves from the stale "L030" citation to **L062**. Schema comments corrected. |
| D-27 | Review | Two validation holes: an id's *type segment* was never checked against the file's schema type (an `item.` id in an `.npc.yaml` resolves as a valid itemRef to an NPC), and `intRange` never enforced `min ≤ max`. | New lints **L003** (id type ↔ schema type) and **L004** (min ≤ max), implemented in `validate_content.py` and registered in the tools SAD §2.2 band table. The spawn `wander_radius_m`/`patrol` exclusivity (old L040) is now enforced directly in `spawn.schema.yaml`. |

Action-item status changes:

- **A-12 — DISCHARGED (authoring):** `schema/content/asset.schema.yaml` (`meridian/asset@1`) is authored per the D-18 union and exercised by 19 example sidecars in `content/core/assets/`; validator lint **L020** replaces the old "pending asset registry" info line (warn by default, `--assets=error` from M0 exit). Remaining: Art + Music sign-off of the schema PR.
- **A-13 — DONE:** Client PRD v0.3 and Client SAD v0.2 carry WLD-04 rows (transfer masking, AoI-refresh handling in `sim` §5.6, join-friend UI, co-presence indicators) and the v0.4 CCU targets.
- **A-15 — RESOLVED (D-31, §7.1):** asset source files and their IF-8 `.asset.yaml` sidecars live **pack-local at `content/<ns>/assets/**`**. This ratifies what the repo already does (the `core` pack's 19 sidecars are already there); the Art SAD's repo-level `/assets/<ns>/**` and the Art PRD's `/client/art/` are corrected. `asset.schema.yaml` `source` is now definitively pack-root-relative.
- Fold-back completed for the **D-04** features: Server PRD v0.3 now carries ACC-03/CHR-05/SOC-03/ECO-05; Tools PRD v0.3 and Music PRD v0.3 now carry CHR-05. A CI check (`tools/check_traceability.py`) now enforces matrix-●-to-PRD traceability and baseline-version sync so this drift class cannot recur silently.

### 7.1 Asset source location — A-15 ruled (2026-07-06)

**Decision D-31 (project owner):** asset source files and their IF-8 `.asset.yaml` sidecars live **pack-local at `content/<namespace>/assets/**`**. The two competing locations are rejected:

- The **Art SAD**'s repo-level `/assets/**` (`/assets/<ns>/**`) tree — rejected; it splits a pack's files across the repo and breaks self-containment.
- The **Art PRD**'s `/client/art/` (`res://art/...`) — this is not a source location at all: it is the *imported* Godot resource output that `mcc` generates into the `.pck` (Tools SAD §2.7), a different artifact from hand-authored source.

**Rationale:** pack-local source makes `.mcpack` community-pack self-containment automatic — a pack is one directory subtree (`content/<ns>/`) carrying everything it needs (`pack.yaml`, `idmap.lock`, content YAML, and now assets + sidecars). That self-containment is the enabling constraint for **TLS-08**. The ruling **ratifies the existing layout**: the example `core` pack's 19 sidecars already sit at `content/core/assets/` (`art/`, `sfx/`, `mus/`, `amb/`), matching the Tools SAD §4 contract.

Fold-back: Tools SAD §4 already stated pack-local (no change beyond dropping the A-15 pending note). Corrected: Art SAD §2.2/§3/§10 (repo-level `/assets` → `content/<ns>/assets`), Art PRD §3.3/§4.3 (source is pack-local; `/client/art` clarified as imported output), `schema/content/asset.schema.yaml` (`source` comment de-pended), `.gitattributes` (comment noting the location; the extension-global LFS patterns already cover `content/**/assets/**` binaries).

### 7.2 CHR-01 appearance-customization depth — A-03 ruled (2026-07-06)

**Decision D-32 (project owner):** CHR-01 character appearance customization is **presets-first with a forward-compatible data model.** A-03 was opened at M0 exit (§3) precisely so this could be settled before Art commits M1 asset scope; it governs the **M1** customization depth, not M0 (M0 remains the D-11 placeholder model — name + class, no appearance — unchanged). Three parts:

1. **Presets, not a full morph rig.** Customization is a set of **discrete preset choices** — hair style, face, skin tone, and the class/race-appropriate options — each stored as a small **preset ID**. This is the WoW-style reference point, and it bounds Art's M1 customization asset scope (a countable set of variant parts, not an open-ended slider space). The Art PRD §5 M1 line ("customization set v1: 4 hair, 4 face, 3 skin palettes") is exactly this shape and stands.
2. **Plus 1–2 continuous morphs, budget-permitting.** Up to **one or two** continuous blend-shape morphs (e.g. height/build) **if** the min-spec blend-shape/GPU budget (Art PRD §4.2, §2.5) allows — optional and small. Deep multi-slider morph customization is **deferred post-1.0**; it is not an M1 (or 1.0) commitment.
3. **The persisted appearance data MUST be forward-compatible / scalable** (the owner's explicit requirement). The appearance the server persists on the character record is modelled as a **small, versioned, extensible appearance record** — a bounded typed set of preset IDs (+ optional morph values) carrying a **schema-version field** — *not* a fixed hardcoded column set. The design intent is that **future presets or morphs can be added post-1.0 without a breaking schema change** to the character record: adding customization depth is **additive** (new fields / new preset slots behind the version tag + a migration shim), never a destructive migration per new slider. The concrete field lands when CHR-01 is built (M1); this decision fixes the *shape* (versioned/extensible), not the wire format.

**Why it matters:** A-03 sits on three contracts at once — it drives **Art's M1 customization asset scope** (how many preset parts to author), the **blend-shape / GPU budget on min-spec** (Art PRD §4.2 — the optional morphs are the variable here), and the **CHR-01 server/client appearance data contract** (the versioned record persisted by the server, read by the client to assemble the character). Deciding presets-first at M0 exit lets Art plan M1 against a bounded, countable set instead of an unbounded slider space, while the versioned record keeps the door open to add depth later without a character-DB migration.

Fold-back: Server PRD §9 (CHR-01 row) + §10 open-question 2 → resolved, appearance record noted as versioned/extensible; Client PRD §4-M1 + §11 (CHR-01 row) → appearance contract noted; Art PRD §4.2 (blend-shape budget reflects presets-first + optional 1–2 morphs, deep morphs deferred) + §10 open-question 5 → resolved; Server SAD §4.2 (`character` table gains the design-intent note that appearance persists as a versioned, extensible record, concrete column at M1).

---

## 8. macOS client (2026-07-04, Baseline v0.5)

**Decision D-28: the client ships on macOS.** Project-owner direction; Godot's cross-platform engine makes the marginal cost a platform matrix, not a port. Scope and rules:

1. **Phasing:** CI builds a macOS client from **M0** (export + boot smoke only — no gate on IT-M0); **supported test-realm client from M1** (signed + notarized nightlies, Mac Low-tier validation in IT-M1's install path); **launch platform at 1.0**. Rationale: keeping a second platform green continuously is cheaper than a porting project, and prevents Windows-only assumptions baking into the GDExtension modules.
2. **Hardware floor: Apple Silicon only (M1, 8 GB unified, macOS 13+).** Intel Macs are out of scope permanently — they exit Apple's own support window before any realistic 1.0, and dropping them deletes the MoltenVK/AMD/Intel-GPU test matrix. The M1 8 GB joins TD-03 as the second Low-tier reference; **the GTX 1060 bench machine remains the authoritative min-spec gate** (art budgets are ratified there; the Mac validates, it does not ratify).
3. **Rendering:** Godot's **native Metal backend** (default on Apple Silicon since 4.4) mirrors the Windows D3D12 choice; Vulkan-via-MoltenVK stays buildable as the diagnostic fallback, same pattern as the Windows Vulkan fallback. **FSR2 remains the cross-vendor upscaler default; MetalFX temporal upscaling is a per-platform option** (Godot 4.4+), never required.
4. **No content/pipeline change:** Apple Silicon Metal supports BC-class compressed textures, so **one `.pck` set serves both platforms** — IF-5, `mcc` determinism, and the pack manifest are untouched (engine pin already covers export-template versioning). Bot client runs `--headless` on macOS unchanged.
5. **Tools stay Windows (TD-08 unchanged):** Forge/Codex/Creator Kit are not part of this decision. Codex (Avalonia) and Forge (Godot editor plugin) are cheap to bring later, but it is not promised — revisit after the M3 community release.
6. **Distribution:** macOS nightlies are codesigned + notarized from M1 (Gatekeeper makes unsigned test builds a support tax); `.dmg`/notarized archive via the same bootstrap updater; the M4 launcher adds a macOS variant. Crash reporting: Crashpad supports macOS — same minidump pipeline, dSYM symbol upload added to CI.
7. **Server, Art, Music, Tools tracks: no deliverable changes.** Art budgets stay GPU-tier-based and 1060-ratified (rule 2); audio runtime is platform-neutral Godot.

| # | Item | Owner | Due |
|---|------|-------|-----|
| A-16 | Mac CI runner + M1 8 GB bench machine procurement; macOS export templates + signing in nightly CI | Client + Ops | M0 |
| A-17 | Apple Developer ID + notarization pipeline (certs, `notarytool` in CI, dSYM upload) | Client | M1 (supported-status gate) |

---

## 9. Player-experience telemetry & observability (2026-07-05, Baseline v0.6)

**Decision D-29: feature ID OPS-05 (Client ●, Server ●, M0→).** Project-owner direction: we must be able to understand the player experience (lag, errors) and operate on logs — captured **server-side wherever possible**, OpenTelemetry-compatible, with Grafana dashboards as first-class deliverables. Binding rules:

1. **Server-side first.** The server is where UX is measured authoritatively: per-session action-RTT histograms (already in the SAD metric set), movement correction/snap-back rates, disconnect reasons + reconnect outcomes, per-opcode error/drop rates, tick health, DB/queue latencies. The client sends **nothing** routinely.
2. **The client channel is exactly three things:** Crashpad minidumps (existing, M0), **ERROR/CRITICAL log events** (batched, rate-limited, with session/build/platform context), and the existing missing-content placeholder events — all on the one project-hosted, Sentry-compatible endpoint. **No behavioral analytics, no PII, ever**; a documented opt-out flag ships with the setting. This is both a privacy posture and an open-source-community trust posture.
3. **OpenTelemetry compatibility via the collector pattern.** In-process instrumentation stays Prometheus-style (the server SAD §8.5 metric set is unchanged); an **OTel Collector** in the deploy stack scrapes `/metrics` and ingests OTLP logs/traces, making OTLP the export lingua franca — operators pipe to any backend without touching daemon code.
4. **Reference observability stack ships in compose** (OPS-01 extension): otel-collector + Prometheus + Loki + Grafana with **provisioned dashboards** (`/server/ops/grafana`, versioned JSON — not screenshots) and alert rules. Dashboards v1: *Realm health* (tick p99, CCU, opcode rates), *Player experience* (RTT, corrections, disconnects/reconnects), *Errors* (server log error rates, client ERROR/CRITICAL ingest, crash rate by build).
5. **Traces:** session-flow spans (auth → grant → world handshake → enter-world) from the start; the bus envelope reserves a trace-context field so cross-process spans light up at the M2 gateway split without a protocol change.
6. **Community operators:** the stack is optional — daemons degrade gracefully to `/metrics` + local JSON logs when no collector is configured.
7. **Phasing:** M0 = foundation (stack in compose, log pipeline, session spans, client channel, dashboards v1); M1 = UX depth (correction/lag instrumentation lands with movement/combat, alert thresholds tuned against IT-M1); M2+ = shard/gateway labels and bus spans (already anticipated in server SAD §8.5).

Epic + stories tracked on GitHub under milestone M0 (OPS-05 epic). Server PRD and Client PRD carry the traceability rows as of v0.5 of each.

---

## 10. Server packaging & continuous delivery (2026-07-05)

**Decision D-30: Docker-first delivery with an automatic packaging pipeline; Kubernetes as a supported option.** Project-owner direction. This extends **OPS-01** (deployment is already its charter) — no new feature ID, no baseline matrix change. Binding rules:

1. **Autopackage on green main.** Every push to `main` that passes CI builds and publishes the `authd`/`worldd` (later `gatewayd`/`servicesd`/`coordd`) container images to **GHCR**, tagged with the commit SHA and `latest`; semver tags on releases. Image digests are recorded in the nightly build manifest so the test realm, operators, and the content-hash tie all reference immutable digests — CI is the only image producer (the single-funnel principle applied to binaries).
2. **Docker/compose remains the reference deployment** and the contributor onboarding path (server PRD §6, unchanged). The compose file consumes *published* GHCR images by default (build-from-source stays one flag away).
3. **Kubernetes is a supported option, not the reference:** a versioned **Helm chart** ships alongside the compose file — v0 covers the single-`worldd` realm (M0–M2 topology + the optional OPS-05 observability stack); the sharded-topology chart (gateway/coordinator/workers/services) lands with OPS-04 at M3, when community operators most need orchestration. Charts are config-only consumers of the same images and the same 12-factor env overrides — no K8s-specific code paths in the daemons.
4. **Supply chain:** images are multi-stage, non-root, healthchecked; SBOM generation and signing (cosign-class) attach at publish so community operators can verify provenance — same trust posture as `.mcpack` signing (A-10), applied to binaries.
5. **Releases:** a semver tag produces a GitHub Release with pinned image digests + checksums; the nightly test realm tracks `main`, public/community realms track releases.

Phasing: publish workflow + compose-from-GHCR at M0; SBOM/signing + release workflow at M1; Helm v0 at M1–M2 (best-effort), sharded chart at M3 with OPS-04.

---

## 11. Terrain decision — A-09 resolved (2026-07-06, no baseline change)

**Decision: fork-and-vendor Terrain3D.** Resolves action item **A-09** (Tools, M0). Full evaluation with sources and per-criterion verdicts in [docs/terrain-eval.md](terrain-eval.md) (which also records the co-signed criteria, discharging #131). No baseline matrix change — this is the M0-exit terrain gate under TLS-02 (Tools PRD §3.1, §9; Tools SAD §5.2), not a new feature.

Terrain3D (MIT, GDExtension) was scored against the four co-signed criteria (Tools PRD open question #4). All four **PASS**, none is a blocker:

1. **128 m region alignment — PASS (exact):** `region_size = SIZE_128` is a native enum value; a Terrain3D region becomes exactly one Meridian 128 m chunk (129×129 vertices at 1 m).
2. **Clipmap LOD on min-spec — PASS (empirical residual):** Witcher-3-style geomorphing geoclipmap, 7 tunable LODs; terrain draw-call cost is negligible against the ≤ 2,500 Low ceiling. The 30 FPS/1060 number is confirmed architecturally; a bench measurement folds into the M0 EditorPlugin-skeleton spike.
3. **Paint-layer count — PASS (headroom):** 32 texture layers vs. the ~8-per-zone / ≤ 4-blended-per-chunk budget (Art PRD §2.3) — 4× margin.
4. **Heightfield extraction — PASS (native):** float32 is Terrain3D's native height storage; the region-image API or `get_height` sampling yields the exact `f32[129×129]` per-chunk grid the server needs (SAD §3.2/§5.2).

**Rationale for fork-and-vendor over adopt-as-is or build-our-own:** no blocking gap justifies building from scratch (Tools PRD R1/R3 flag that as an M1-critical-path schedule risk); vendoring a pinned fork lets upstream/GDExtension churn (PRD R1/R7) not break M1, freezes the terrain feature set at M0 exit, and allows small Meridian-specific patches (e.g. a `forge_core` heightfield-export entry point) — all unencumbered under MIT. The **`ITerrainBackend` seam (Tools SAD §5.2, firmed up in that revision)** keeps the choice reversible: an in-house GDExtension remains a drop-in second implementation with no caller changes.

Residuals to close before the M0-exit feature freeze: the C2 bench measurement, vendoring mechanics (upstream commit pin + Godot-version pin into the Creator Kit engine pin per PRD R7/R8), and confirming the `export_heightfield` path against the vendored build. Tracked under the existing M0 terrain gate; no new action item.

---

## 12. Client engine pin — Godot 4.6 → 4.7 (2026-07-07, no baseline change)

**Decision D-33: bump the client engine pin from Godot `4.6-stable` to `4.7-stable`.** Project-owner direction. Resolves the M0 client-toolchain prerequisite (#262) that gates all client GUI/CI work. No baseline matrix change and no new feature ID — this is a milestone-boundary engine pin move under Client SAD R4 (§9.7), not a scope change.

Rationale:

1. **4.7 is the ref that actually exists cleanly.** godot-cpp has **no `4.6` release branch and no `godot-4.6-stable` tag** upstream — the prior 4.6 pin was a bare `master` sync commit (`58d1de7`). 4.7 matches the developer's local install and godot-cpp's mainline `extension_api`; the `master` "Sync … (4.7-stable)" commit (`5ffd70e`) is the exact, verifiable ref whose `extension_api.json` reads `Godot Engine v4.7.stable.official`.
2. **Minor step, no code churn.** All M0 GDExtension modules — `meridian_client`, movement controller (#102), telemetry (#168), pack-mount (#107), login (#99), remote-interpolation (#104), `register_types` — rebuilt against 4.7 godot-cpp with **zero source changes**: the cores are engine-agnostic and the thin godot-cpp adapters saw no 4.6→4.7 API breaks. Build is 0-warning for both `template_debug` and `editor` (arm64).
3. **Load-proven on 4.7.** Verified by a headless load against the local `4.7.stable.official.5b4e0cb0f` editor: the extension loads and all six registered classes (`MeridianClient`, `MeridianMovementController`, `MeridianTelemetry`, `MeridianPackMount`, `MeridianLogin`, `MeridianRemoteInterpolator`) are available; `MeridianClient.get_version()` reports `godot 4.7-stable`.

Binding artifacts (kept in sync): `client/ENGINE_VERSION` (machine-readable pin — engine `4.7-stable`/`5b4e0cb`, godot-cpp `5ffd70e`, 4.7 download SHA-512s), `client/project/meridian.gdextension` (`compatibility_minimum = "4.7"`), and the human pin table + rationale in `client/README.md`. Recorded architecturally in Client SAD §9.7. CI (a macOS runner build/export job, #112) is a deliberate follow-up — not added with this pin move.

---

## 13. Repo topology — monorepo through M2, multi-repo split deferred to M3 (2026-07-07, no baseline change)

**Decision D-34: keep the monorepo through M2; defer a Server/Client/Tools/Assets multi-repo split and revisit it at M3, gated on a hard prerequisite.** Project-owner direction. The split is not rejected — it is deferred, and its revisit at M3 is **conditional on the shared wire contract and the shared movement/crypto constants first being extracted into a versioned `meridian-protocol` package** (the extraction itself happens in-monorepo). No baseline matrix change and no new feature ID — this is a repo-topology ruling, not a scope change. Tracked on GitHub as epic **#266** with sub-issues **#267–#271**.

The `meridian-protocol` extraction is the **true prerequisite**, not a nicety: in a monorepo a single atomic PR touching both sides keeps client and server honest against a contract that is still moving, so the seam can stay implicit; across repos that same contract becomes a **published-artifact contract** between independently-versioned repos, and without a versioned package to pin against, a split would relocate the coupling onto an unstable, unversioned interface.

Rationale:

1. **The contract is still churning through M0/M1.** The wire protocol (IF-1/IF-2, `schema/net/*.fbs`) and the shared constants — the hand-duplicated `movement_constants.h` guarded by a cross-track `static_assert`, plus the session-crypto framing — are actively changing during M0/M1. Splitting now would relocate that coupling onto an unstable contract and cost more than it saves; the protocol seam must exist and stabilize first.
2. **The pull toward splitting is real but premature.** Client (Godot/GDExtension, community/modding-facing, moddable) and server (C++ realm daemons, operator-facing) genuinely have different architectures, audiences, and release cadences — that is the legitimate case for eventually splitting. But different cadences only pay off once the interface between the two is a versioned artifact; until then a split trades a compiler-enforced seam for a coordination tax.
3. **Extract in-monorepo, then decide.** Pulling the wire contract + shared constants into `meridian-protocol` inside the monorepo is valuable on its own (it removes the hand-duplicated constants and gives the contract a semver + wire-compat policy) and is the gating deliverable the M3 go/no-go decision reads against.

Action tracked in M3 (epic **#266**):

| Issue | Work | Role |
|-------|------|------|
| #267 | Extract the shared wire contract + movement/crypto constants into a versioned **`meridian-protocol`** package (in-monorepo) | **Prerequisite** — gates everything below |
| #268 | Protocol **semver + wire-compat CI policy** for the package | Contract discipline |
| #269 | **Assets topology finalize** — reconcile with D-31 (pack-local `content/<ns>/assets/**`) and #160 (Git LFS) | Resolve the Assets-repo question |
| #270 | **Split-readiness go/no-go** decision point | Gate |
| #271 | **Cross-repo integration-test strategy** — pinned-SHA super-repo, only if go | Contingent on #270 = go |

**Status:** deferred to M3. No baseline change; no code impact now. The next concrete step is the in-monorepo `meridian-protocol` extraction (#267); the split itself is not committed and is re-decided at the #270 go/no-go point.

---

## 14. Character management rides the world session — M0 (2026-07-07, no baseline change)

**Decision D-35: character management (list/create/delete) rides the authenticated WORLD session (worldd, WoW-style), and it is M0 — implement now.** Project-owner direction, recorded on issue **#286**. This is an intentional, authorized additive extension to the previously-frozen M0 world protocol (IF-2, `schema/net/world.fbs`). No baseline matrix change and no new feature ID — it discharges the CHR-01 stub's (D-11) missing wire surface, which #110 had to work around with a client-local in-memory stub.

Binding rules:

1. **On the world session, not a separate service.** The three ops are IF-2 messages carried over the same authenticated, post-grant worldd session as movement/entity/clock — mirroring WoW's realm/world connection, where character management happens before entering world. No new daemon, port, or interface number; the messages are additive opcodes in the session/system range (`0x0010`–`0x0015`), leaving the frozen movement (`0x1xxx`) / entity (`0x2xxx`) / clock (`0x0004`) shapes untouched and backward-compatible.
2. **The account is always the session's own.** Every op acts on `ConnCtx.account_id` — the account the IF-3 grant authenticated this session as — never an account named in the message (no `account_id` field appears in any of the three request tables). A client can therefore only ever list/create/delete its OWN characters. Ownership is additionally enforced at the DB by the meridian-characters CRUD's `WHERE id = ? AND account_id = ?` predicate (the soft-ref rule, server SAD §4.4), so deleting or listing another account's characters is impossible by construction.
3. **Backed by the existing CRUD, not a reimplementation.** worldd's handlers call the merged `meridian-characters` library (#85) — `list_characters` / `create_character` / `delete_character` — mapping its typed create exceptions (`DuplicateName` / `InvalidRace` / `InvalidClass` / `InvalidName`) 1:1 onto typed wire statuses. The D-11 M0-frozen race/class roster (`roster.h`) is the validation source. The char-DB read/write runs synchronously on the IO worker, exactly like enter-world's grant consume + placeholder load (no world-thread involvement).
4. **Guarded by the golden conformance corpus (#68) + determinism gate (#122).** Six golden fixtures (one per new message) decode-assert + semantically round-trip in the proto-conformance gate; the additive schema change keeps every existing IF-1/IF-2 golden byte-identical.

Scope landed with this decision (issue **#286**): the six messages + opcode wiring, the three worldd handlers on the CRUD, a DB-backed integration test over a real authenticated session (in the `worldd-session` CI job), and the conformance fixtures.

**Client wiring is a scoped follow-up, not part of this decision's server landing.** The #110 char-select store (`client/project/scenes/charselect/character_store.gd`) remains a local in-memory stub for now: replacing it with real requests over the net thread (#279) is a client-track change that cannot be verified headlessly on the M0 client (the MoltenVK/Apple-Silicon caveat, #283), so it is deferred rather than shipped unverified. The store was authored specifically so only that one file changes when the wiring lands — its create/delete validation already mirrors the server's failure taxonomy, which now has a wire home.

---

## 15. TD-11 gate under load — native audio (provisional) (2026-07-07, no baseline change)

**Decision D-36: run the TD-11 adaptive-music timing gate under load and record the native-vs-fallback ruling. The modelled under-load pass supports native Godot audio, so the provisional ruling is native — the GDExtension stem mixer (music SAD §3.2) is held in reserve. The ruling is ratified by the owner-runtime measured `--source godot` pass under the #111 bot fleet on min-spec.** Resolves issue **#147** under epic **#15** (music SAD [§3.1](sad/music-sad.md); supersedes the D-14 "MetaSounds vs middleware" gate, already retargeted to Godot native/GDExtension by D-17). No baseline matrix change and no new feature ID — this is the M0 TD-11 gate ruling, recorded with evidence, not a scope change. Full evidence (commands, per-seed distribution, negative control, artifacts) in [docs/td11-gate-decision.md](td11-gate-decision.md); canonical CSV/JSON artifacts in `docs/reviews/td11-gate-under-load/`.

Binding facts and rules:

1. **What ran.** The TD-11 measurement harness (`tools/td11_music_timing/`, #145; real-source seam #144) on its **modelled** source (`SampleClockModel`) under the §3.1 load profile: `--profile load --trials 2000 --bars 128`, 10 seeds (0–9). 2000 scripted state flips ≈ **833 min** of scripted timeline (§3.1 asks ≥30 min); 128-bar drift pass (the §3.1 64–128-bar maximum). The `load` profile is the harness's model of the CPU-load jitter + read-noise the 50-bot-fleet gate exercises.
2. **Result: PASS, 10/10 seeds, wide margins.** Transition error **p95 ≈ 4.6–4.9 ms** (limit ±10 ms), worst case **≈ 0.003 bars** (limit 1 bar), stem lockstep drift **≤ 0.29 ms** (limit 1 ms), drift trend **≈ 0 ms/bar** (limit ±0.02), **zero** starvation. No criterion sits near its threshold. Thresholds were **not** altered to reach PASS (§3.1 verbatim). The `--profile failing` negative control, run with identical load parameters, **FAILS** (exit 1) — the gate discriminates a healthy runtime from a broken one.
3. **Honesty: these numbers are MODELLED, not measured audio.** The harness stamps `measured: false` on every mock run. The modelled pass establishes that **nothing in the modelled physics contradicts native audio** and that the statistics/gate code the real probe feeds is correct — it is **not** itself the §3.1 authoritative verdict on native Godot audio. The one quantity the model cannot pin (does `AudioStreamInteractive` apply the crossfade sample-accurately within a mix block, or only at its boundary) is exactly what the real probe resolves.
4. **Provisional ruling = native; fallback held in reserve.** This matches the project owner's stated default (#147: "default to native audio; only fall back if the gate fails"). The GDExtension stem mixer (SAD §3.2) — sample-accurate integer bar clock, per-stem ring buffers, a ~4–6-week one-engineer swap of the playback layer *below* the `ZoneMusicPlayer` API — remains the designed fallback, selected only if the measured gate fails.
5. **Ratification is owner-runtime and still pending.** The authoritative §3.1 evidence is the **measured** `--source godot` run on the min-spec reference machine under the #111 50-connection bot fleet on the flat bootstrap map, with an adaptive track (#146) to exercise. CI cannot run it: no Godot 4.7 binary is available here (#283) and #111/#146 do not yet exist. The `--source godot` seam is wired (#144) and, run without an engine, fails fast with an actionable error — confirming the only gap is the runtime, not the harness. When the owner runs the measured pass: all five criteria clear ⇒ **D-36 ratified final (native)**; any criterion fails under real load ⇒ the gate selects the GDExtension fallback. Either way, record the measured numbers by updating D-36.
