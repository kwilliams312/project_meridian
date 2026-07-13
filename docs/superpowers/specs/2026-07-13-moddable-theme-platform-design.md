<!-- SPDX-License-Identifier: Apache-2.0 -->
# Design — Moddable Theme Platform ("the pack *is* the theme") + Chibi default theme

- **Status:** DRAFT (umbrella architecture; brainstormed 2026-07-13)
- **Type:** Architecture / program of work (decomposes into per-sub-project specs)
- **Supersedes/retires:** the realistic art direction (Ardent/Dolmen races, Warden's Kit, `art.char.human.*` base, the stone Zone-01 kit). See "Retirement" below.

## 1. Vision

Project Meridian is an **open-source MMO platform**. The engine (client + server binaries,
protocol, and gameplay *systems*) is fixed and reusable; **all game content lives in a
swappable pack**. Changing the pack changes the game — art, world, and rules — with no engine
rebuild. The first, default pack ("**Chibi**") replaces the retired realistic content and
proves the platform end-to-end.

Two servers running different packs are two different games sharing one engine.

## 2. The governing principle

> **The pack holds 100% of content. The kernel holds 100% of systems — and no content.**

- **Kernel (fixed, ships in binaries):** every *system* — combat + ability engine, roster,
  itemization/equip, attribute framework, mount system, weather + day/night, zone streaming +
  AoI, instance/dungeon system, quest engine, NPC/AI + spawns, map system, UI framework (+ icon
  wiring), audio system, social/economy. Each system is a **generic consumer** of pack content.
  The kernel ships with *no* real content — only inert dev fixtures.
- **Pack (swappable, one per realm):** every asset (characters, armor, weapons, creatures,
  NPCs, buildings, interiors, dungeon kits, props, foliage, mounts, landmarks), terrain
  materials, VFX, skybox & weather presets, map art, **every icon**, the UI skin, all audio,
  **plus** all rules-data (races, classes, spells, talents, items, attributes, catalogs), zones,
  quests, encounters, and strings/lore.

**The boundary test:** a pack may supply content for any system the kernel *has*. The only
thing requiring engine work is a genuinely new **content type no kernel system consumes yet**
(e.g. player housing, if no theme ever needed it before). The asset taxonomy is therefore
**open-ended within the set of kernel systems** — not a hardcoded enum to keep extending.

```
  ┌──────────────────────── PACK  (content/<ns>/ — the theme) ────────────────────────┐
  │  ART: character/armor/weapon/creature/npc/kit_piece/prop/foliage/landmark/mount/   │
  │       dungeon/interior · terrain materials · vfx · skybox/weather · icons · map    │
  │  DATA: races · classes(7-field) · spells(primitive recipes) · talents · items ·    │
  │        attribute tuning · armor-type/weapon-type catalogs · zones · quests · spawns │
  │  PRESENTATION: UI theme · audio (music/sfx/ambience) · strings/lore                 │
  └───────────────────────────────────────┬────────────────────────────────────────────┘
                    loaded by realm · mounted+hash-verified by client
  ┌────────────────────────────────────────▼────────────────────────────────────────────┐
  │  KERNEL (fixed): engine binaries · protocol · combat/AoI/movement/persistence ·      │
  │  ability engine (Tier-1 primitives; Tier-2 sandbox later) · attribute framework ·    │
  │  generic interpreters (equip-gating, roles, talents) · mount/weather/dungeon/map/    │
  │  quest/npc/ui/audio systems · numeric-ID + append-only discipline · content_hash     │
  └───────────────────────────────────────────────────────────────────────────────────────┘
```

## 3. Classes & characters (fully data-driven, operator-authored)

Classes are **not** baked archetypes. They are **pack data**, authored by server operators in an
external Codex-style tool, over global catalogs. `server/characters/src/roster.h`'s hardcoded
enum is **retired**; the roster loads from the pack.

**A class record has 7 fields:**
1. **Abilities** — selected from the pack's global **spell/ability catalog**.
2. **Usable armor types** — from the **armor-type catalog** (seed: Cloth, Leather, Mail, Plate).
3. **Role** — Healer / DPS-Melee / DPS-Ranged / Tank, or a **Hybrid** of those.
4. **Usable weapon types** — from the **weapon-type catalog** (seed: 2H, 1H, Wand, Staff).
5. **Attribute bonuses/penalties** — over the kernel's base attribute framework.
6. **Race limitations** — which races may pick this class (content/lore gate, not a stat gate).
7. **Talent tree** — built from the global **talent catalog**.

The 4 legacy classes (Vanguard/Runcaller/Warden/Mender) survive only as **seed templates**.

### Races
6 **cosmetic** races: one chibi base mesh + 6 material variants — Flat (Red, Green, Blue,
Yellow) and Metallic (Gold, Silver). No mechanical difference. The kernel *supports* per-race
attribute tuning as a capability, but the Chibi theme sets all racial modifiers to zero.

## 4. The two-tier ability engine

- **Tier 1 (default, tool-authored):** a spell is a **data recipe** — targeting + cost + cast
  time + cooldown + a list of **effect primitives** with parameters. The kernel ships a fixed,
  **extensible** primitive palette (damage, heal, DoT/HoT, stat buff/debuff, shield, CC
  [stun/root/silence], resource change, movement, summon, …). Deterministic, server-authoritative,
  safe to run untrusted. Covers WoW-like kits. Most "I need a script" cases are met by **adding a
  primitive** instead.
- **Tier 2 (opt-in, later):** sandboxed scripting for effects primitives can't express. Heavier —
  requires sandboxing untrusted operator code, determinism for replay/anti-cheat (OPS-03), and
  perf bounds under load. Designed as an extension on top of Tier 1, not a prerequisite.

## 5. Attribute framework

The kernel ships a **known base attribute/stat set** the combat formulas understand: primary
(Str/Agi/Int/Sta…) + derived (crit/haste/armor…). Operators **tune** per-class/per-race bonuses
& penalties and how items grant stats, but do not invent primary attributes the formulas can't
see. Adding wholly new attributes is a later extension (parallel to Tier-2 scripting).

## 6. Trust, validation & compatibility (the "checksum" model)

Validation is a **build-time** job; trust is a **runtime** job. This matches the existing
architecture (`mcc` validates + emits `content_hash`; `MeridianPackMount::set_expected_content_hash`).

1. **Build time —** `mcc` fully validates the pack (roster well-formed, class references resolve,
   spell primitives valid, catalogs consistent) *once* and stamps a **`content_hash`**.
2. **Join time —** client and server exchange the hash; **exact match = identical content.** One
   compare; the server never re-validates the pack's internal structure at runtime.
3. **Create time —** the one check a checksum can't do: a cheap membership lookup that a player's
   chosen race+class is in the loaded roster (and race allowed for that class). Validates *input*,
   not the pack.
4. **Save compatibility —** the exact hash is too strict to gate saved characters (a harmless
   additive edit changes it). Pair it with the **append-only ID discipline** already in the
   codebase: additive pack edits never invalidate existing characters.
5. **Breaking changes —** removing/renumbering an existing id is gated. A realm records its pack's
   **compatibility version**; a breaking change makes the server **refuse to boot until the operator
   migrates** (`mcc` diffs old→new and reports exactly what breaks). DB-migration discipline for
   content. Additive changes boot freely.

## 7. Build decomposition

Each sub-project gets its own spec → plan → implement cycle.

### Group A — Platform / engine (the reusable capability)

| # | Sub-project | Delivers | Deps |
|---|-------------|----------|------|
| **1** | **Pack contract + content schema + catalogs** | Schemas & `mcc` validation for all content categories **and** the rules-data (race, 7-field class, spell-primitive, armor/weapon-type, attribute, talent); `content_hash`; append-only + breaking-change compat rules; theme manifest. | — |
| **2** | **Kernel class/character system** | Attribute framework, Tier-1 ability engine, equip-gating, role hooks, talent execution, roster-from-pack-data (retire `roster.h`), create-time check, boot-time migration gate. | 1 |
| **3** | **Operator authoring tools** | Codex editors (class/spell/talent + data) and Forge (zones) emit valid pack data. | 1 |
| **4** | **Theming/pack seam** | Fold UI theme + audio + `theme.yaml` into the pack contract; realm pack-selection config; client mount UX. | 1 |
| **6** | **Tier-2 sandboxed scripting** | Opt-in scripting escape hatch + guardrails. | 2 |

### Group B — The Chibi theme (content payload = a whole world)

| # | Workstream | Asset classes / content |
|---|-----------|--------------------------|
| **5a** | Characters & gear | `character_model` base + 6 material races · `armor_model` (cloth/leather/mail/plate) · `weapon_model` (2H/1H/wand/staff) · class visual identity |
| **5b** | Built environment | `kit_piece` (houses, castle, walls, bridges, stairs, interiors, dungeon kits) · `prop` (lanterns, stalls, signs, crates) · `hero_landmark` (castle, vistas) |
| **5c** | Nature & terrain | `foliage` (trees, bushes, flowers, mushrooms) · natural rock/cliff/water · Terrain3D material set |
| **5d** | Creatures & NPCs | `creature_model` mobs · townsfolk/vendor/trainer NPC looks · mounts |
| **5e** | Mood layer | VFX · day/night skybox & **weather** · UI skin · `icon` set (ability/item/buff/map) · `map` art · audio (music/SFX/ambience) |
| **5f** | World assembly | Zones in Forge from the kits · quests · encounters · the class/spell/talent **data** authored in Codex |

**Order:** **1 → (2, 3, 4 in parallel)** → assemble Chibi (5a–5f), with **art (5a–5e) starting
immediately** (needs only the schemas from 1). **6** later.

## 8. Retirement of realistic content

Chibi is the sole direction. The realistic greybox work is retired, **not** maintained:
- Stone Zone-01 kit intake (PR #635) — **closed** (superseded).
- Art epics #14 (art bible + pipeline proof) and #591 (character visual coherence) — re-scoped to
  the chibi direction; the realistic beauty-shot/kit goals are dropped.
- `roster.h`'s Ardent/Dolmen/Vanguard/… survive only as seed templates for pack data.

Because Meridian is pre-launch/greybox with no live player data at stake, the Chibi pack defines a
fresh roster id-space; the append-only discipline applies going forward within a shipped theme.

## 9. Risks & open questions

- **R1 — Tier-1 primitive palette completeness.** If the palette is too small, operators hit the
  Tier-2 cliff early. Mitigation: seed the palette from the WoW-like kit needs of the Chibi
  starter classes; make it explicitly extensible.
- **R2 — Generic-executor performance.** A data-interpreted ability engine must hold the combat
  perf budget under 50+ player scenes. Mitigation: compile pack recipes to a compact runtime form
  at load; benchmark against the existing determinism/replay harness.
- **R3 — Migration UX.** "Refuse to boot until migrated" is only as good as the `mcc` diff report.
  Mitigation: make the breaking-change report actionable (names the exact ids/fields).
- **R4 — Scope creep across the two-tier engine.** Keep Tier 2 strictly out of the critical path.
- **Q1 — Client asset delivery:** MVP assumes the pack is part of the build/realm distribution;
  download-at-connect (à la TLS-08 community packs) is deferred.

## 10. Glossary

- **Pack / theme** — a self-contained `content/<ns>/` subtree; the unit a realm loads.
- **Kernel** — the engine binaries + systems; fixed, content-free.
- **Effect primitive** — a kernel-provided atomic spell effect a Tier-1 recipe composes.
- **content_hash** — build-time digest of a pack; the client↔server agreement token.
- **Compatibility version** — coarser than the hash; bumps only on breaking (non-additive) changes.
