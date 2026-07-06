# Server Track PRD — Project Meridian

**Track:** Server
**Version:** 0.7 — 2026-07-06 (v0.7: **A-03 RESOLVED / D-32** — CHR-01 appearance is presets-first, and the server persists it as a **versioned, extensible appearance record** (preset IDs + optional morph values behind a schema-version tag), additive post-1.0 with no breaking character-record migration; §9 CHR-01 row and §10 open-question 2 updated (concrete column lands at M1 per Server SAD §4.2). v0.6: **packaging & CD** per D-30 — GHCR autopublish on green main, compose-from-published-images, Kubernetes/Helm as a supported option, supply-chain posture; OPS-01 extension, no baseline change. v0.5: **OPS-05 telemetry & observability** per Baseline v0.6 / D-29 — OTel-compatible export, player-experience metrics, log pipeline, provisioned Grafana dashboards added to §6 and §9. v0.4: reviewed against Baseline v0.5 / D-28 (macOS client) — no server impact; the protocol and realm are client-platform-neutral. v0.3: fold-back of the Baseline v0.2 / D-04 additions this PRD's own v0.1 gap reports produced — **ACC-03** registration, **SOC-03** trade, **ECO-05** bank, **CHR-05** mounts now carried in §4 and §9; resolved open questions pruned into §10. v0.2: sharded-realm scale-up per D-23/Baseline v0.4 — OPS-04 revised to 3000+ CCU per realm, WLD-04 added, gateway/coordinator/services/shard-worker process model; architecture detail in the [Server SAD](../sad/server-sad.md))
**Status:** Draft for cross-track review
**Baseline:** [Game Design Baseline v0.6](../00-GAME-DESIGN-BASELINE.md) — all feature IDs (ACC/CHR/WLD/CMB/NPC/QST/ITM/ECO/SOC/GRP/PVP/TLS/OPS), milestones (M0–M4), and technical decisions (TD-01..TD-12) referenced here are defined there and are binding.

---

## 1. Overview & Goals

The Server track delivers the authoritative Linux game server for Project Meridian: a C++20, CMake-built pair of daemons (`authd`, `worldd`) targeting Ubuntu LTS, backed by MariaDB/MySQL (TD-04, TD-05). The architecture is *inspired by* CMaNGOS and TrinityCore — auth/world daemon split, three-database layout, grid/cell spatial partitioning, a fixed-cadence update loop, opcode-style message dispatch — but is a **clean-room implementation under Apache-2.0** (TD-09). We study their public architecture and documentation as prior art; **we never copy, port, or transcribe GPL-licensed code** from either project. Contributors who have recently worked in those codebases must implement from written specs in this repo, not from source recall. Any contribution suspected of derivation is rejected in review.

### Goals, tied to the pillars

- **"Server is law" (Pillar 3).** Every gameplay outcome — movement legality, combat resolution, loot rolls, economy transactions — is computed or validated server-side. The client predicts (CHR-02); the server decides and corrects. Anti-cheat (OPS-03) is a property of the architecture, not a bolt-on.
- **Tools-first (Pillar 1).** `worldd` has no hand-authored content path. All world data arrives via the content compiler (TLS-01) from `/content` YAML (TD-06). The server exposes the hooks the Tools track needs: validation callbacks (TLS-07), hot-reload for live editor preview (TLS-06), and community pack loading (TLS-08).
- **Content is data (Pillar 4).** The world DB is a build artifact, never a source of truth. `worldd` treats it as read-mostly, regenerable at any time.
- **Legally clean (Pillar 5).** No Blizzard formats or code; no GPL code. Protocol, DB schema, and file formats are original designs.

### Non-goals (1.0)
Cross-realm tech (a realm remains the sharing boundary — zone *shards inside* a realm are in scope per D-23/WLD-04), UDP transport before M2 (TD-07), Windows server builds (dev-container workflow covers Windows contributors), player housing (Baseline §7).

---

## 2. Architecture

### 2.1 Daemons and process model

```
                 ┌──────────┐   TLS/TCP    ┌────────┐
  Client ───────►│  authd   │◄────────────►│ auth DB│
    │            └────┬─────┘              └────────┘
    │ session key     │ realm list, session grant (via auth DB)
    ▼                 ▼
  ┌──────────────────────────────┐         ┌─────────────┐
  │           worldd             │◄───────►│characters DB│
  │  ┌────────┐  ┌────────────┐  │         ├─────────────┤
  │  │network │  │ map update │  │◄───────►│  world DB   │
  │  │ threads│  │ workers    │  │         └─────────────┘
  │  └────────┘  └────────────┘  │
  └──────────────────────────────┘
```

- **`authd`** — stateless login daemon. Terminates TLS, authenticates accounts (SRP6a-style verifier scheme; passwords never stored or transmitted in plaintext), serves the realm list, writes a one-time session grant to the auth DB (ACC-01). Horizontally scalable behind a TCP load balancer from day one because it holds no session state beyond the handshake.
- **`worldd`** — the simulation. Owns maps, entities, combat, quests, chat, groups. One process per realm at M0–M1; from M2 the client listener moves to the gateway; at M3 `worldd` is recast as a **shard worker** (500–750 CCU each, no client listener, bus-attached; see 2.6 / OPS-04).
- **`gatewayd` (M2)** — owns every client TCP connection and the IF-2 per-session encryption; routes sessions to workers. A shard transfer (WLD-04) is a server-side reroute, never a client reconnect.
- **`servicesd` (M2)** — realm-global services above the shards: chat channels, guilds, friends, groups, mail, auction house, LFG. One economy and one social space per realm regardless of shard.
- **`coordd` (M3)** — realm coordinator, control plane only: spins zone shards up/down against a population band (80–150 players per open-world zone shard, with hysteresis), places players on zone-enter (party > friends > guild > load balance), and brokers shard transfers. Warm standby + journal replay removes it as a single point of failure (SAD §8.6).
- Shared static libraries: `libmeridian-core` (logging, config, threading, RNG), `libmeridian-db` (async MySQL layer, prepared statements only), `libmeridian-proto` (generated from `/schema` per TD-07), `libmeridian-game` (simulation systems, linkable by the sim-test harness).

### 2.2 Database layout (TD-05)

CMaNGOS-style three-DB split, original schema:

| DB | Contents | Access pattern |
|----|----------|----------------|
| `auth` | accounts, verifiers, session grants, realm list, bans, GM levels | low volume, latency-sensitive |
| `characters` | characters, inventory, quest state, mail, auctions, guilds, friends | write-heavy, per-player |
| `world` | **compiled output of TLS-01 only**: creature/item/quest/loot/spawn/gossip tables, content-pack manifest + content hash | read-mostly, bulk-loaded at boot |

All runtime DB access goes through the async DB layer: worker-thread pool with per-DB connection pools, futures/callbacks delivered back on the owning map thread. **No synchronous DB calls from the update loop** except during startup and shutdown. Character persistence is dirty-flagged and batched (see §7).

### 2.3 Threading model

- **Network I/O:** N I/O threads (epoll via Asio standalone), doing framing, decrypt, and deserialization into typed messages; zero game logic.
- **Map update workers:** a pool of M worker threads executes map updates each tick. **A map (or instance) is updated by exactly one thread per tick** — all entity state on a map is single-threaded-by-construction, so gameplay code needs no locks. Cross-map effects (whispers, mail, guild chat, group state for members on different maps) go through lock-free MPSC message queues drained at tick boundaries.
- **DB workers:** dedicated pool, as above.
- **World thread:** tick orchestration, session lifecycle, global managers (guild mgr, auction mgr, LFG queue).

Tick: fixed **20 Hz (50 ms budget)** world update; per-map soft budget with overrun logging (see §7).

### 2.4 Spatial partitioning & map/instance management

- Each map is divided into a **grid of cells** (CMaNGOS-inspired, clean-room): grids of 533 m × 533 m, subdivided into 8×8 cells (~66 m). Grids activate when a player (or player-adjacent object) is nearby and deactivate on a timer when empty — mobs in inactive grids do not tick.
- **Visitor pattern over cells** for AoI queries: "notify all players within X of position P" iterates only the covered cells.
- **Instancing (GRP-02):** an `InstanceMap` is a map-id + instance-id pair with its own entity space, spawned on first group entry, bound to the group, destructed after a grace period. Continent maps are singletons per realm.
- **Navigation:** server-side movement for mobs uses navmesh (Recast/Detour, zlib-licensed — license-compatible) generated by the Tools pipeline from zone geometry; `worldd` loads `.navmesh` artifacts referenced by the world DB.

### 2.5 Message dispatch

Opcode-handler style: every client→server message type has a registered handler with declared **required session state** (e.g. `IN_WORLD`, `AT_CHARACTER_SELECT`), **processing thread** (map thread vs world thread), and **rate class** (see OPS-03). Handlers are generated-skeleton + hand-written body; the opcode table is generated from `/schema` so client/server can never disagree on message IDs (TD-07).

### 2.6 Path to OPS-04 (sharded realm, 3000+ CCU, per D-23)

A **realm** is the shared logical server (one auth domain, one character DB, one social graph, one economy); a **shard** is a running copy of open-world zone simulation inside it. M0–M1 run one `worldd` per realm with the map-worker pool; the split is phased:

1. **M2 — gateway split + services extraction.** `gatewayd` takes over the client listener, session table, and IF-2 encryption (IF-3 grants become gateway-scoped); realm-global managers (chat SOC-01, mail ECO-04, AH ECO-03, groups GRP-01) extract into `servicesd` behind their existing bus addresses. IT-M2 must prove the split is transparent: all M2 integration criteria pass through the gateway with < 1 ms p99 added forwarding latency, and a worker restart no longer drops client TCP connections.
2. **M3 — coordinator + multi-worker + transfers.** `coordd` manages shard lifecycle (spin-up above the population band, drain-and-merge below it) and placement (party > friends > guild > load balance); 5–6 shard workers at 500–750 CCU each serve the realm; the WLD-04 player-visible transfer protocol goes live (eligibility check → target pre-spawn → atomic gateway reroute → source despawn → client AoI refresh; anti-abuse: ~30 s cooldown, combat/loot locks).
3. **Cross-process messaging** uses the same typed message bus as cross-map queues, over an internal FlatBuffers/TCP mesh with realm/zone/shard addressing. Every cross-map effect and global manager has been bus-addressed since M0 precisely so these splits are transport changes, not rewrites.
4. **Character-save correctness across transfers** is enforced by an ownership fence in the characters DB (per-character `save_epoch`, compare-and-set writes): exactly one process can persist a character at any instant, so a crash mid-transfer can neither duplicate nor lose state (SAD §4.7).
5. Chat scoping: `/say`/`/yell`/emotes are shard-local; the zone general channel is realm-wide; whisper/party/guild are realm-global (`servicesd`).

### 2.7 What we take from CMaNGOS/TrinityCore, and what we do differently

| Area | Adopted (architecturally) | Different in Meridian |
|------|---------------------------|----------------------|
| Process split | authd/worldd, 3-DB layout | authd stateless + horizontally scalable from M0; session grant table, not realmd handoff quirks |
| Spatial | grid/cell + visitor AoI | grid sizes tuned to our zones; navmesh from tools pipeline, not extracted client data |
| Dispatch | opcode-handler tables | **schema-generated protocol** (FlatBuffers or protobuf per TD-07) from `/schema`; no hand-maintained opcode enums or packing code |
| Content | world DB tables drive spawns/loot/quests | **world DB is compiled from YAML (TD-06) by TLS-01** — never hand-edited SQL; content hash versioning; hot-reload (TLS-06) and community packs (TLS-08) are first-class |
| Scripting | C++ script hooks for bosses/events | data-driven behaviors first (authored in TLS-03/TLS-05); C++/embedded-script escape hatch only where data can't express it |
| Licensing | none — reference only | Apache-2.0 clean-room; contribution policy enforced in review |

---

## 3. Network Protocol

All messages are schema-defined in `/schema` (TD-07); client, server, and tools consume generated code. Schema changes require cross-track sign-off (Baseline §5.1). Final IDL choice (FlatBuffers vs protobuf) is an M0 week-2 spike — see Open Questions.

### 3.1 Session flow (ACC-01, ACC-02)

1. **Auth (TLS over TCP, port `authd`):** client connects with TLS 1.3, SRP6a-style proof exchange (server stores verifier+salt only), receives realm list + per-realm population/status.
2. **Grant:** on realm selection, `authd` writes `{account, realm, session_key, expiry(30s), client_build}` to the auth DB and returns the session token.
3. **World handshake (TCP, world port):** client connects (to `worldd` at M0–M1, to `gatewayd` from M2 — the realm list always carries the right address), sends token; the world side validates against the auth DB, derives per-session keys from `session_key` (HMAC on headers + encrypted payloads — world traffic is not full TLS at M0; revisit at M2 alongside the UDP decision). Session encryption terminates at the gateway from M2, which is what makes WLD-04 shard reroutes invisible to the client. Then character list (CHR-01) → enter-world.
4. **Reconnect:** grants are single-use; a dropped world session re-authenticates through `authd`.

### 3.2 Movement replication (CHR-02) — predict client-side, validate server-side

- Client sends movement packets (state flags, position, orientation, timestamp, move-counter) at up to 10/s plus on state change (jump, turn, stop).
- Server validation per packet: **speed check** (distance/Δt vs server-known speed + tolerance for the active move mode), **teleport/ack counter** (client can't skip forced-move acks), **bounds/geometry sanity** (inside map bounds, z within navmesh/heightfield tolerance — full collision honesty is client-side, server checks plausibility), **flag legality** (can't claim swim on dry land, can't fly). Violations: snap-back correction first, escalate to kick/flag per OPS-03 policy.
- Server rebroadcasts movement to AoI observers with server timestamps; remote entities are interpolated client-side (~100–200 ms buffer — Client track contract).
- Mob movement is fully server-authoritative via navmesh spline paths; clients receive spline packets, never mob position deltas per tick.
- Transport is TCP at M0–M1; TD-07 permits moving movement to UDP no earlier than M2 (open question, §10).

### 3.3 Interest management / AoI

- Per-player AoI = visibility radius (default **90 m**, per-map tunable) evaluated via grid-cell visitors; entity **create/destroy** messages on AoI enter/leave, delta **field updates** while visible.
- Update batching: all field changes for a tick are coalesced per observer into one update packet per tick.
- Priority/decimation: beyond 50 m, non-target entity movement updates decimate to 4/s; combat-relevant events (casts on you, your target's health) never decimate.
- Caps: see §7 for AoI entity budgets. Under overload the server sheds farthest non-group, non-combat entities first ("crowd fade" — Client track renders placeholder density if desired).

---

## 4. Gameplay Systems by Milestone (server-owned)

### M0 — Foundation
- **ACC-01 / ACC-02:** `authd` SRP6a auth + realm list; session grant handoff into `worldd`.
- **ACC-03 (basic):** account creation via a CLI/admin tool (`meridian-account create`) writing SRP verifier+salt to the auth DB — the test-realm onboarding path; public web signup is the M3 half.
- **CHR-01 (stub):** character create/select with fixed race/class list; characters DB schema v1.
- **CHR-02 (basic):** movement intake, validation v0 (speed + bounds), AoI relay so two clients see each other move — the substance of **IT-M0**.
- **TLS-01 (consume):** `worldd` boots from compiler-produced world DB; content-hash logged and reported to clients on connect.
- **OPS-01:** config file format, Dockerfile + compose (authd, worldd, mariadb), structured logs, `/metrics` endpoint stub.

### M1 — Greybox Vertical Slice
- **CMB-01 Combat core:** server-resolved tab-target combat — target validation, range/LoS checks, GCD enforcement, cast-time spells with interrupt/pushback, instants, attack tables (hit/miss/crit/dodge/parry), damage/heal application. All rolls server-side.
- **CMB-02 Mob AI:** threat table per mob, aggro radius by level delta, leash-to-home with evade/full-heal, respawn timers, patrol paths from NPC-01 data; AI parameters authored via TLS-03.
- **CMB-03 Death/resurrect:** death state machine, corpse object, release-to-graveyard (nearest graveyard from world data), corpse-run resurrect, resurrection sickness knobs from content data.
- **CMB-04 (basic) Auras:** buff/debuff container — periodic ticks, stat modifiers, stacking rules v1, dispel types; full aura effect matrix deferred to M2.
- **CHR-02 (full), CHR-03:** swim, fall damage; XP awards (kills, quests), level-up stat application from class/level tables (world DB).
- **WLD-01 / WLD-03:** zone/area tables, area-trigger volumes (server-checked for discovery XP and quest objectives), POI data served for the map.
- **NPC-01 / NPC-02:** spawn system from compiled spawn tables, waypoint patrols; gossip menus, vendor inventories, class trainers.
- **QST-01:** quest state machine (available → active → objectives complete → rewarded), kill/collect/deliver objective tracking, quest giver/receiver validation, reward grant (XP/money/items) — all server-side; supports the 10-quest chain of IT-M1.
- **ITM-01 / ITM-02:** item instances vs item templates, bags/inventory ops with server-side validation (no client-trusted moves), equip rules (class/level/slot); loot generation from loot tables at kill time, loot window sessions, quest-item drop rates conditioned on quest state.
- **ECO-01:** money as int64 copper, vendor buy/sell/buyback, repair hooks (durability model lands with ITM-03 at M2 — see Open Questions).
- **SOC-01:** chat router — say/yell (AoI-scoped), whisper (cross-map via message bus), general zone channel; profanity/moderation hooks.
- **OPS-02 (basic):** GM levels on account, in-game commands (`.tele`, `.summon`, `.additem`, `.godmode`, `.kick`, `.ban`, `.gps`, `.respawn`), all GM actions audit-logged.
- **OPS-03 (start):** movement validation v1, per-opcode rate limits, server-side range/LoS/cooldown enforcement (inherent in CMB-01), economy action sanity checks.

### M2 — Systems Depth
- **CHR-04 Talents:** talent trees from content data, point spend/reset server-validated, talent effects as aura/spell modifiers.
- **CMB-04 (full):** complete aura effect set (CC, absorbs, procs, auras-affecting-spells), stacking/exclusivity groups.
- **ITM-03 Itemization:** rarity tiers, stat-budget formulas evaluated at compile time (TLS-01) but enforced at load (validation hook, TLS-07); random-suffix and proc support.
- **GRP-01 Groups:** party of 5, leader/roles, group loot methods (round-robin, need/greed rolls server-side), XP split rules.
- **GRP-02 Instancing:** InstanceMap lifecycle, group-to-instance binding, instance reset rules, per-instance threat/boss state (threat mechanics: taunt, threat-modifiers land here per Baseline M2).
- **QST-02:** quest chains/prerequisites, class/level/reputation conditions, scripted event objectives (escort, use-object) via data-driven event scripts (TLS-05).
- **ECO-02 Crafting/gathering:** profession skill tracking, gathering nodes as respawning world objects, recipes with material consumption — transactional (all-or-nothing inventory ops).
- **ECO-03 Auction house:** listing/bid/buyout with escrow, deposit/cut sinks, expiry sweeps; delivery via mail. **ECO-04 Mail:** attachments (items/money), COD, expiry and return-to-sender; both are coordinator-scoped managers (see 2.6) and DB-transactional — the IT-M2 economy loop (gather → craft → auction → mail) must survive a `worldd` crash mid-flow without item duplication or loss.
- **SOC-03 Trade window:** player-to-player trade sessions — server-side dual-accept lockstep state machine, escrowed item/gold swap committed as a single characters-DB transaction (same integrity bar as mail/AH; the IT-M2 forced-crash test covers a mid-trade kill).
- **ECO-05 Bank storage:** per-character bank slots/tabs, slot-purchase money sink, deposit/withdraw as server-validated inventory ops (shares the ITM-01 validation path).
- **WLD-02:** server world-clock authority for day/night phase and weather state broadcast per zone (weather patterns from content data) — realm-global authority in `servicesd`, identical across shards of a zone.
- **OPS-04 (phase 1):** gateway split (`gatewayd` owns client connections + session crypto; IF-3 gateway-scoped) and `servicesd` extraction; `save_epoch` ownership fence on character writes lands here (see 2.6).
- **TLS-06 (serve):** live-preview hot reload — see §5.
- **OPS-03 (deepen):** statistical anomaly detection (XP/gold per hour outliers → GM report queue), trade/economy audit trail.

### M3 — Alpha World
- **OPS-04 (full):** sharded realm live — realm coordinator, 5–6 shard workers, dynamic zone-shard spin-up/drain, coordinator warm-standby failover; architecture rated 3000+ CCU per realm, IT-M3 proves 1500 CCU across ≥ 3 zone shards; see 2.6 and §7.
- **WLD-04:** player-visible shard mechanics — party auto-merge to leader's shard, "join friend" same-zone transfer, transfer eligibility/cooldown enforcement, AoI-refresh protocol, shard co-presence data for the client's social UI (client owns the UX per D-23/A-13).
- **CHR-05 Ground mounts:** mount/dismount state machine (cast time, combat/indoor restrictions from content data), mounted movement as a validated move mode in the §3.2 envelope, mount ownership from item/spell data in the world DB.
- **ACC-03 (full):** public registration path — a rate-limited web-signup service writing SRP verifiers to the auth DB (email verification, IP throttles), replacing the M0 CLI-only flow for public realms.
- **SOC-02:** friends/ignore lists, guilds (roster, ranks/permissions, guild chat via coordinator, MOTD).
- **GRP-03 LFG:** role-based queue (tank/heal/dps), cross-map group formation, teleport-to-dungeon.
- **PVP-01:** duel flow (flag, arena-less bounds circle, no-death finish), PvP flagging rules (toggle, guard/zone rules, flag timers), server-resolved player-vs-player combat legality.
- **PVP-02 Battleground:** 10v10 CTF-style — BG instance type with team logic, queue system (shares LFG queue infrastructure), flag object mechanics, score/win conditions, deserter handling.
- **TLS-08 (serve):** community content pack loading — see §5.
- **OPS-02 (extended):** moderation at scale — mute/silence durations, chat log search tooling for GMs, ticket queue stub for M4.

### M4 — direction only (Baseline §3)
Hardening, raid-scale encounter support (larger groups, boss scripting depth), moderation tooling maturity, launcher/patcher integration points. Scope fixed at end of M2.

---

## 5. Content Ingestion (Tools track contract)

- **TLS-01 — compiled content.** `worldd` never reads YAML. The content compiler emits world-DB SQL (plus navmesh/heightfield artifacts). Every compile embeds a **content hash + schema version** into a manifest table; `worldd` refuses to boot on schema-version mismatch and reports the hash to clients at handshake so client paks and server data can be verified as the same compile (mismatch = warning at M0–M1, hard fail on the test realm from M1).
- **TLS-07 — validation hooks.** The Server track ships `meridian-validate`, a headless library/binary (built from `libmeridian-game`) that loads a candidate world DB and runs server-side semantic checks the compiler can't do syntactically: dangling loot/quest/spawn references, unreachable spawn points vs navmesh, stat-budget violations (ITM-03), quest-chain cycles, orphaned gossip. Tools track runs it in content CI (Baseline §6); non-zero exit blocks the merge.
- **TLS-06 — hot reload for live preview (M2).** `worldd` exposes a local, GM-authenticated **editor channel** (separate port, off by default, never on public realms): the zone/data editors push a recompiled content delta; `worldd` applies it to a designated preview map — respawn affected creatures, swap templates, re-run spawn placement — without a restart. Scope: template/spawn/loot/quest data. Out of scope for hot reload: DB schema changes, navmesh geometry (requires map reload).
- **TLS-08 — community packs (M3).** A content pack = compiled SQL delta + client pak + signed manifest (pack ID, version, dependency hashes). `worldd` loads packs at boot from a config-listed directory into **namespaced ID ranges** (each pack gets an allocated ID band to prevent collisions with core content and other packs), runs TLS-07 validation on the merged set, and advertises the pack list to connecting clients so the client can verify it has matching paks. IT-M3 requires a community-made zone loading on an **unmodified** server — therefore zero server-code changes may be needed for a conforming pack.

---

## 6. Operations

- **Config (OPS-01):** layered — compiled defaults ← `/etc/meridian/*.toml` ← env-var overrides (12-factor for containers). Every knob in this PRD (tick rate, AoI radius, rate limits, map-process assignment) is config, hot-reloadable where safe (`SIGHUP` / GM command).
- **Docker:** official images for `authd` and `worldd` (Ubuntu LTS base, multi-stage build); `docker compose up` gives a full local realm (both daemons + MariaDB + migrations job) in one command — this is also the contributor onboarding path and the Tools track's local preview server.
- **Packaging & CD (D-30):** every push to `main` that passes CI **autopublishes** the daemon images to GHCR (SHA + `latest` tags; semver on releases), with digests recorded in the nightly build manifest — CI is the only image producer. Compose consumes published images by default (build-from-source one flag away). Images are multi-stage, non-root, healthchecked, with SBOM + signing at publish (M1). **Kubernetes is a supported option, not the reference:** a versioned Helm chart mirrors compose (single-realm v0; the sharded-topology chart lands with OPS-04 at M3); charts are config-only consumers of the same images and 12-factor env overrides — no K8s-specific code in the daemons. Semver tags produce GitHub Releases with pinned digests; the nightly test realm tracks `main`, community realms track releases.
- **Logging:** structured JSON logs (spdlog) with category levels; separate append-only audit streams for GM actions, economy transactions, and anti-cheat flags.
- **Metrics:** Prometheus `/metrics` on both daemons — tick duration histogram per map, CCU, sessions by state, opcode rates, DB queue depth/latency, AoI entity counts, active grids/instances, memory. Reference Grafana dashboards live in `/server/ops/grafana`. Alert rules: tick p99 over budget, DB queue depth, CCU saturation.
- **Telemetry & observability (OPS-05, D-29):** player experience is measured **server-side first** — per-session action-RTT histograms, movement correction/snap-back rates, disconnect reasons + reconnect outcomes, per-opcode error/drop rates. Export is **OpenTelemetry-compatible via the collector pattern**: in-process instrumentation stays Prometheus-style; an OTel Collector in the compose stack scrapes `/metrics` and ingests OTLP logs/traces, so operators pipe to any backend without daemon changes. The reference stack (otel-collector + Prometheus + Loki + Grafana) ships in compose with **provisioned dashboards** (Realm health / Player experience / Errors — versioned JSON in `/server/ops/grafana`) and alert rules. Session-flow trace spans (auth → grant → handshake → enter-world) from M0; the bus envelope reserves trace context for cross-process spans at the M2 gateway split. Daemons degrade gracefully with no collector configured (metrics endpoint + local JSON logs only).
- **Test realm (Baseline §6):** stands up at end of M0; **nightly redeploy** from `main` — CI builds server, pulls latest compiled content (post TLS-07 validation) and latest client build, runs DB migrations, redeploys via compose/systemd, then runs the smoke suite (login → enter world → move → kill mob → complete quest, via headless bot). Characters DB persists across redeploys; world DB is replaced wholesale each night. A failed smoke test rolls back to the previous image and pages the on-call track owner.
- **GM/moderation (OPS-02):** GM level model (player < helper < GM < admin), all commands permission-gated and audit-logged; account/character/IP bans enforced in `authd` and `worldd`; chat moderation (mute) at the chat router.

---

## 7. Performance & Scale Targets

| Metric | Target | Notes |
|--------|--------|-------|
| World tick rate | **20 Hz** (50 ms) | fixed; per-map budget 40 ms, overrun logged; p99 tick < 50 ms is a release gate each milestone |
| Movement intake | ≤ 10 packets/s/client | validated per §3.2 |
| CCU — M0 | 10 | IT-M0 scale |
| CCU — M1 | **50+** on one `worldd`, one zone | IT-M1: 50+ concurrent on test realm |
| CCU — M2 | 150 realm-wide **through the gateway**; 20 concurrent 5-player instances | dungeon + economy load; gateway split transparent (< 1 ms p99 added) |
| CCU — M3 | **1500 sustained across ≥ 3 zone shards** (IT-M3); architecture rated **3000+ per realm** | 3000+ CCU full load test is an **M4 gate** (bot fleets) |
| CCU — per shard worker | **500–750** per `worldd` process | 5–6 workers per 3000-CCU realm; < 8 GB RSS each |
| Zone shard population | band **80–150** players; hard cap 165 | coordinator hysteresis: spin-up > 140 avg (5 min), drain < 60 avg (10 min); SAD §2.3 |
| Gateway | 5,000 connections; **< 1 ms p99 added forwarding latency**; < 2 GB RSS | single gateway at M3 |
| Shard transfer (WLD-04) | **p95 < 500 ms** request → AoI-stable; p99 < 1 s | ≤ 5 transfers/s per shard; 30 s per-character cooldown |
| Coordinator failover | new leader < 10 s, zero data-plane interruption | warm standby + journal replay (SAD §8.6) |
| Chat fan-out | ≤ 20k deliveries/s realm-wide at 3000 CCU | slow-mode + per-channel caps |
| AoI budget | ≤ 120 visible entities/player typical; hard cap 250 with priority shed (§3.3) | town-square worst case |
| Entities per shard worker | 5,000 active (ticking) creatures/objects | inactive grids don't tick |
| Downstream bandwidth | ≤ 20 kB/s/client typical, ≤ 60 kB/s burst (crowded hub) | drives update decimation tuning |
| DB — character save | dirty-flag batch flush every 30 s + on logout/zone/trade; crash loses ≤ 30 s | economy ops (AH, mail, trade, craft) are immediate transactions, never batched |
| DB budget | async queue p99 < 100 ms; zero sync queries in tick path; login char-list query < 50 ms | Prometheus-alerted |
| Boot time | world DB full load < 60 s at M3 content volume | nightly redeploy window |
| Memory | shard worker < 8 GB RSS at 750 CCU / M3 world; `servicesd` < 4 GB at 3000 CCU | commodity VM target |

---

## 8. Testing

### 8.1 Unit & simulation tests
- `libmeridian-game` is linkable without network or DB (interfaces mocked) — combat formulas, threat, aura stacking, loot rolls (statistical tests over 10⁵ draws), quest state machine, inventory transactions, and movement validation all get deterministic unit tests (seeded RNG). CI (Linux) runs them per PR.
- **Sim harness:** headless `worldd` with an in-memory DB fixture runs scripted scenarios ("5 bots pull boss, tank taunts at 3 s…") as golden-outcome tests; used for regression on CMB-01..04 and GRP-02 threat.

### 8.2 Bot-based load testing
- `meridian-bot`: a headless client built on the same generated protocol lib — logs in through the real `authd`/`worldd` path, executes behavior scripts (roam, chat, grind mobs, quest loop, AH churn). Milestone load profiles are versioned in `/server/test/load/`. Load runs execute nightly against the test realm at 2× the current milestone CCU target and publish tick/latency/bandwidth reports; a regression > 15% on tick p99 fails the run.

### 8.3 Integration tests — Server contribution & done-criteria

| IT | Server contributes | Server "done" means |
|----|--------------------|---------------------|
| **IT-M0** | `authd` login + realm list; grant handoff; character stub; movement intake/validation v0; AoI relay; Dockerized deploy | Two real clients on the test realm: both log in via `authd`, enter world on `worldd`, and each sees the other move with < 250 ms visible latency; server survives 10 bots for 1 h with zero crashes |
| **IT-M1** | All M1 systems in §4; 50+ CCU stability; nightly redeploy live | A fresh player completes the 10-quest chain to level 5 with 50+ concurrent (bots+humans); zero server-authority violations reproducible from the client (speedhack/teleport/item-dupe attempts from an instrumented cheat-client are all rejected and logged); tick p99 < 50 ms throughout |
| **IT-M2** | Instancing, groups, threat; crafting/AH/mail transactions; talent server logic; hot-reload channel for TLS-06; **gateway split + `servicesd` extraction (OPS-04 phase 1)** | 5-player group clears Dungeon-01 in an isolated instance (no cross-instance leakage); economy loop gather→craft→auction→mail completes; a mid-loop forced `worldd` kill produces neither item loss nor duplication on restart; editor hot-reload round-trip < 10 s; **all of the above pass through `gatewayd` with < 1 ms p99 added latency; a worker restart holds client TCP at the gateway (character select, no re-login); epoch-fenced saves reject a duplicate writer** |
| **IT-M3** | OPS-04 full sharding: coordinator + 5–6 shard workers + shard lifecycle; WLD-04 transfers; BG + LFG + guilds/friends; community pack loader | **1500 CCU sustained ≥ 2 h across ≥ 3 zone shards live**, tick p99 in budget on every worker; party auto-merge and join-friend transfers exercised throughout at p95 < 500 ms with zero character-state loss or duplication; cross-shard whisper, realm-wide zone chat, guild/mail all functional; coordinator failover drill passes (< 10 s, no data-plane interruption); a community-built zone pack loads on an **unmodified** release server binary and players enter it. (Full 3000+ CCU load is an M4 bot-fleet gate.) |

---

## 9. Traceability Table

Every feature where Server = ● in the Baseline matrix.

| Feature ID | Server deliverable(s) | Milestone |
|-----------|------------------------|-----------|
| ACC-01 | `authd`: TLS + SRP6a auth, realm list, auth DB, bans | M0 |
| ACC-02 | Session-grant handoff, world handshake, per-session keys | M0 |
| ACC-03 | Account creation: CLI/admin tool (M0); rate-limited web-signup service writing SRP verifiers (M3) | M0 basic / M3 |
| CHR-01 | Character CRUD + validation, characters DB schema (stub M0; race/class/appearance rules M1). **Appearance is persisted as a versioned, extensible appearance record** (preset IDs + optional morph values behind a schema-version tag, A-03 / D-32) — additive post-1.0, no breaking character-record migration | M0 stub / M1 |
| CHR-02 | Movement intake/validation, AoI replication, swim/fall (basic M0; full M1) | M0 basic / M1 |
| CHR-03 | XP/leveling engine, class/level stat tables from world DB | M1 |
| CHR-04 | Talent trees, point spend/reset, talent-driven spell/aura modifiers | M2 |
| CHR-05 | Mount/dismount state machine, mounted-speed movement validation, mount data from world DB | M3 |
| WLD-01 | Zone/area tables, grid activation, area-trigger volumes | M1 |
| WLD-02 | World-clock authority, per-zone weather state machine + broadcast | M2 |
| WLD-03 | POI/discovery triggers, discovery XP, map data service | M1 |
| WLD-04 | Shard transfer protocol (eligibility, pre-spawn, gateway reroute, AoI refresh), party auto-merge + join-friend brokering, transfer cooldown/locks, co-presence data feed for client UI | M3 |
| CMB-01 | Server combat resolution: targeting, range/LoS, GCD, casts, attack tables, damage/heal | M1 |
| CMB-02 | Threat tables, aggro/leash/evade, respawn, patrol AI | M1 |
| CMB-03 | Death state machine, corpse objects, graveyards, resurrect | M1 |
| CMB-04 | Aura container, periodic effects, stacking (basic M1; full effect matrix M2) | M1 basic / M2 |
| NPC-01 | Spawn system, waypoint patrols from compiled spawn tables | M1 |
| NPC-02 | Gossip engine, vendor inventories, trainer services | M1 |
| QST-01 | Quest state machine, objective tracking, reward grants | M1 |
| QST-02 | Chains/prerequisites/conditions, scripted event objectives | M2 |
| ITM-01 | Item instances/templates, inventory + equip validation | M1 |
| ITM-02 | Loot table evaluation, loot sessions, quest-drop conditions | M1 |
| ITM-03 | Rarity/stat-budget enforcement at load, procs, random suffixes | M2 |
| ECO-01 | Currency, vendor buy/sell/buyback/repair transactions | M1 |
| ECO-02 | Professions, gathering nodes, transactional crafting | M2 |
| ECO-03 | Auction manager: escrow, bids/buyout, sinks, expiry | M2 |
| ECO-04 | Mail manager: attachments, COD, expiry/return | M2 |
| ECO-05 | Bank storage: slots/tabs, purchase sink, validated deposit/withdraw | M2 |
| SOC-01 | Chat router: say/yell/whisper/general, moderation hooks | M1 |
| SOC-02 | Friends/ignore, guild manager (roster/ranks/chat) | M3 |
| SOC-03 | Trade sessions: dual-accept lockstep, transactional escrowed item/gold swap | M2 |
| GRP-01 | Party manager, group loot rolls, XP split | M2 |
| GRP-02 | InstanceMap lifecycle, group binding, resets, instance threat/boss state | M2 |
| GRP-03 | Role-based LFG queue, group formation, dungeon teleport | M3 |
| PVP-01 | Duel flow, PvP flagging rules, PvP combat legality | M3 |
| PVP-02 | BG instance type, 10v10 queue, CTF objective + scoring logic | M3 |
| TLS-01 | World-DB consumption contract, content-hash manifest, boot-time load | M0 |
| TLS-03 | Server-side schema + runtime semantics for NPC/mob data (AI params, loot links) consumed by editor output | M0 alpha / M1 |
| TLS-04 | Server-side schema + runtime semantics for item data | M0 alpha / M1 |
| TLS-05 | Quest data runtime (chains, scripted events) consumed from editor output | M1 |
| TLS-06 | GM-authenticated editor channel, live content-delta hot reload on preview maps | M2 |
| TLS-07 | `meridian-validate` semantic validation library/binary for content CI | M1 |
| TLS-08 | Community pack loader: namespaced IDs, signed manifests, merged validation | M3 |
| OPS-01 | Config system, Docker images/compose, logging, Prometheus metrics, nightly redeploy pipeline; GHCR autopublish on green main + release workflow + Helm chart option (D-30) | M0→ |
| OPS-02 | GM levels, command set, audit logging (basic M1; moderation-at-scale M3) | M1 basic |
| OPS-03 | Movement validation, rate limits, server-side rule enforcement, anomaly detection, audit trails | M1→ |
| OPS-04 | Sharded realm: gateway + realm coordinator + shard workers + realm-global services, dynamic zone-shard spin-up/drain, 3000+ CCU per realm rating (1500 CCU IT-M3 proof; 3000+ at M4) | M2 gateway / M3 full |
| OPS-05 | OTel-compatible telemetry (collector in compose), player-experience metrics (RTT, corrections, disconnects, errors), structured-log pipeline (Loki), session trace spans, provisioned Grafana dashboards + alert rules, client ERROR/CRITICAL ingest endpoint | M0→ |

---

## 10. Risks & Open Questions

### Risks
1. **Clean-room discipline.** Highest legal risk. Mitigation: written specs before implementation, contribution policy in `/server/CONTRIBUTING.md`, review checklist, provenance statements on architecture-heavy PRs.
2. **Single-thread-per-map hotspots.** One crowded hub map can blow the tick budget regardless of core count. Mitigation: AoI decimation/shedding (§3.3), grid deactivation, profiling gates each milestone; worst case, dynamic cell-level parallelism is a known (complex) escape hatch — decide by end of M2 based on M1/M2 load data.
3. **TCP-only movement feel.** Head-of-line blocking may make combat/movement feel poor at real-world latencies before the (optional, ≥M2) UDP move. Mitigation: measure at IT-M1 with latency injection in bot tests; keep transport abstracted in `libmeridian-proto`.
4. **Hot reload (TLS-06) vs authority.** Live-mutating world data risks state corruption (e.g., reloading a template mid-combat). Mitigation: preview-map-only scope, whitelisted mutable data classes, never enabled on public realms.
5. **Economy transactional integrity at M2.** Mail/AH/craft dupes are the classic MMO exploit class. Mitigation: DB-transaction-first design, forced-crash tests in IT-M2 done-criteria, economy audit stream from day one.
6. **OPS-04 retrofit risk.** If M0–M2 code bypasses the message bus for cross-map effects, the M3 process split becomes a rewrite. Mitigation: bus-only rule enforced by architecture tests (no direct cross-map pointers) from M0.
7. **Nightly redeploy churn.** World-DB replacement each night requires strict separation of player state (characters DB) from world state; any feature that writes player progress into world-DB-adjacent state breaks. Enforced in schema review.

### Resolved since v0.1 (via [Sync Decisions](../01-SYNC-DECISIONS.md) — folded into this revision)
- **FlatBuffers vs protobuf (TD-07):** FlatBuffers, decided Baseline v0.2 (**D-01**).
- **UDP for movement:** Server track owns the go/no-go gate at end of M2, driven by IT-M2 latency data (**D-03**).
- **Trade, banks, registration, mounts:** added as **SOC-03** (M2), **ECO-05** (M2), **ACC-03** (M0 basic / M3), **CHR-05** (M3) in Baseline v0.2 (**D-04**) — carried in §4 and §9 as of this revision.
- **Durability/repair timing:** repair moves to M2 with ITM-03; ECO-01 at M1 is currency + buy/sell only (**D-02**).
- **Navmesh generation ownership:** a TLS-02 deliverable — Forge exports, server consumes (**D-15**).
- **GM tooling UI split:** Client owns the in-game GM UI, Server owns execution/permissions, Codex owns offline moderation/data-inspection tooling (**D-16**).

### Open questions (flagged, not invented)
1. **World-DB delivery to the server** at scale (TLS-08 packs are defined, but core content: does the compiler push SQL migrations, or full-DB image swap?). This PRD assumes nightly full world-DB rebuild + swap; fine until community servers want incremental updates. (Also server SAD §10(f).)
2. **CHR-01 appearance data** — **RESOLVED (A-03 / D-32, [Sync Decisions](../01-SYNC-DECISIONS.md) §7.2):** customization is presets-first (discrete preset IDs), plus an optional 1–2 continuous morphs if the Art min-spec budget allows. The server persists appearance as a **small, versioned, extensible appearance record** — a bounded typed set of preset IDs (+ optional morph values) with a schema-version field, **not** a fixed column set — so future presets/morphs are added post-1.0 **additively, without a breaking migration** to the character record. The concrete column/format lands when CHR-01 is built at M1 (character-DB schema, Server SAD §4.2); this decision fixes the shape, not the wire format.
3. **A-14 — per-shard resource farming (owner: Server + Design, due M2).** Per-shard gathering nodes and rare spawns multiply availability by shard count; the WLD-04 transfer cooldown alone won't stop coordinated shard-hop farming. Mitigation candidates: node/rare tagging, zone-shared spawn state, diminishing returns. The SAD (§4.7) keeps node/rare state behind a location seam so any candidate lands without redesign; farm telemetry ships at M3 regardless.

---

*End of Server PRD v0.7. Changes to feature scope or milestones must go through Baseline cross-track review (Baseline §0 status note).*
