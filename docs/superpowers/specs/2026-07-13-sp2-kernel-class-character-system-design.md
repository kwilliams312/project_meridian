<!-- SPDX-License-Identifier: Apache-2.0 -->
# Design — Sub-project 2: Kernel class/character system

- **Status:** DRAFT (2026-07-13)
- **Parent:** [Moddable Theme Platform](2026-07-13-moddable-theme-platform-design.md) — sub-project 2.
- **Depends on:** SP1 (content schemas + catalogs), now merged. This sub-project is the KERNEL
  that *consumes* SP1's data.
- **Scope:** the server/kernel behavior that turns SP1's pack data into a running game: the
  attribute framework, the Tier-1 ability engine, equip-gating, talent/role handling, the roster
  loaded from pack data (retiring `roster.h`), the create-time membership check, and the boot-time
  compat/migration gate. **No authoring tool** (SP3), **no chibi content** (SP5), **no Tier-2
  scripting** (SP6).

## 1. Existing consumption points (build on these)

- `server/worldd/ability_store.{h,cpp}` — in-memory `AbilityStore`; `AbilityEffect`/`EffectKind`
  (damage/heal/aura/threat) + `StatKey` mirror the DDL. `combat_resolver` consumes it.
- `server/worldd/db_content_store.cpp` — loads content from the world DB into the stores;
  `ability_effect_kind_from_db()` maps DB strings → `EffectKind` (4 kinds only).
- `schema/sql/world/30_ability.sql` — `ability_effect.kind ENUM('damage','heal','aura','threat')`
  + per-kind columns. **This relational shape is the bottleneck** the SP1 seed pack exposed.
- `server/characters/src/roster.h` — hardcoded race/class enum; consumed by `characters.cpp`
  (create validation), `db_content_store`, `world_dispatch`, `server/tools/meridian-character`.
- `server/worldd/world_boot.{h,cpp}` — worldd boot/content-load sequence.
- `mcc` emit-sql — content YAML → `world.sql` → MariaDB (the established content path; worldd loads
  from the DB, not from the pack directly).

## 2. Architecture decisions

### 2.1 Ability representation — generic effect payload (NOT per-kind relational)
**Decision:** abilities carry their effect recipe as a **generic structured payload**, not exploded
into per-kind relational columns. The kernel consumes the full effect-primitive palette via one
extensible representation.

- **DDL:** replace the per-kind `ability_effect` columns with an `ability` row carrying an
  `effects_json` payload (the ordered `effects[]` recipe, canonical JSON) — plus the small stable
  header columns already used (school/target/cast/cooldown/resource). Keep `ability_effect_stat_mod`
  only if still needed; prefer folding stat mods into the effect payload. `ability_effect.kind`'s
  restrictive ENUM goes away.
- **mcc emit-sql:** emit the `effects[]` recipe as canonical JSON into `effects_json` (deterministic
  key order, so `content_hash`/golden stay stable), instead of per-kind INSERTs.
- **`db_content_store`:** deserialize `effects_json` into a runtime `AbilityEffect` tagged union
  covering the FULL palette (dot/hot/buff/debuff/shield/cc/resource/movement/summon + the originals).
- **`AbilityStore`/`combat_resolver`:** extend `EffectKind` + `AbilityEffect` to the full palette;
  the Tier-1 engine interprets each primitive server-authoritatively.

*Rationale:* a new effect kind becomes a schema+engine change, never a DDL migration; the DB stays a
transport for pack data rather than a rigid second schema; consistent with "pack = content" and the
umbrella §4 Tier-1 engine. This directly resolves the SP1.8 world-DB gap.

### 2.2 Roster from pack data (retire `roster.h`)
The valid race/class set comes from the **loaded pack**, not a compiled enum. SP1 already defines
`race`/`class` schemas; SP2 loads them (via mcc emit → DB, mirroring items/abilities) into a runtime
`Roster`. `characters.cpp` create validation queries the loaded `Roster` (chosen race/class exist;
race ∈ class `race_limits`), replacing `is_valid_race`/`is_valid_class`. The numeric-id + append-only
discipline (SP1) preserves persisted `character.race/class`. `roster.h` is deleted; its 4
placeholders live on only as the seed pack's data (already added in SP1.8).

### 2.3 Attribute framework
The kernel ships the base attribute vocabulary (primary + derived, matching SP1's
`meridian/attribute@1` seeds). SP2 loads attribute definitions + per-class/per-race `attribute_mods`
and applies them to a character's effective stats, which the combat formulas + buff/debuff primitives
read/write. Adding brand-new attributes stays a later extension (umbrella §5).

### 2.4 Equip-gating (+ the deferred category-match)
On equip, the kernel checks the item's `equip_type` against the character's class
`usable_armor_types`/`usable_weapon_types`. This SP2 story ALSO closes the SP1-deferred **category
consistency** (an item in `usable_armor_types` must be `category: armor`) — now enforceable because
the kernel has cross-entity lookup at runtime (which the static validators lacked without a semantic
layer). Role hooks (e.g. Tank threat) and talent-tree effect application land here too.

### 2.5 Boot-time compat / migration gate
worldd records the pack `compatibility_version` it booted with (SP1 `mcc diff` classifies breaking
vs additive). On a breaking change vs the persisted realm state, worldd **refuses to boot** until an
operator migration runs (umbrella §6). Additive boots freely.

## 3. Story decomposition

| Story | Deliverable | Touches | Deps |
|-------|-------------|---------|------|
| **2.1** | Ability effects → generic `effects_json` (DDL + mcc emit-sql + `content_hash`/golden stable) | `30_ability.sql`, `tools/mcc`, golden | — |
| **2.2** | Full-palette runtime `AbilityEffect` + `db_content_store` deserialize + `AbilityStore` | `ability_store.{h,cpp}`, `db_content_store.cpp` | 2.1 |
| **2.3** | Tier-1 ability engine — `combat_resolver` executes all primitives server-authoritatively | `combat_resolver.{h,cpp}` | 2.2 |
| **2.4** | Attribute framework — load attribute defs + class/race `attribute_mods` → effective stats | worldd stats, `db_content_store` | 2.2 |
| **2.5** | Roster from pack data — load race/class into runtime `Roster`; retire `roster.h` | `characters.cpp/.h`, `db_content_store`, `world_dispatch`, tools | 2.1 (emit race/class) |
| **2.6** | Create-time membership check against loaded roster (chosen race/class valid; race ∈ class race_limits) | `characters.cpp` | 2.5 |
| **2.7** | Equip-gating + category-match + role/talent hooks | worldd equip path | 2.4, 2.5 |
| **2.8** | Boot-time compat/migration gate (record compatibility_version; refuse boot on breaking) | `world_boot.cpp` | 2.1 |

**Order:** **2.1 first** (unblocks the ability chain + emits race/class for the roster) → **2.2 → 2.3**
(ability engine spine) in sequence; **2.4, 2.5, 2.8** can parallelize after 2.1/2.2; **2.6** after 2.5;
**2.7** after 2.4+2.5. Serialize the DB-schema/emit-touching stories (2.1/2.2/2.5) — like SP1, they
share golden/emit-sql artifacts.

## 4. Verification (server, not just schema — this is live kernel work)

- **Unit:** `combat_resolver` executes each primitive correctly (deterministic); attribute mods
  applied; equip-gating accepts/rejects per class types + category.
- **DB-backed (MANDATORY — the SP1.8 lesson):** every content/DB-touching story runs the MariaDB
  `world.sql` load + the DB-backed worldd tests, not just the hash tie. QA for these stories MUST
  stand up MariaDB (`scripts/dev/run-local.sh`) and load `world.sql`.
- **Runtime:** worldd boots on the seed pack, a character is created against the loaded roster,
  equips gated gear, and casts an ability whose effects resolve — end-to-end, executed, not mocked.
- **Golden/determinism** + `mcc-parity` stay green; `content_hash` stable across the `effects_json`
  change.
- **UI/client:** none in SP2 (server/kernel only) → no UI E2E gate.

## 5. Risks

- **R1 — `effects_json` determinism.** Canonical key order is required so `content_hash`/golden stay
  stable. Mitigation: emit via the same canonicalizer mcc already uses for the pack hash.
- **R2 — roster retirement blast radius.** `roster.h` has ~7 consumers; retire behind the runtime
  `Roster` carefully, keeping numeric-id semantics. Mitigation: 2.5 updates all call sites in one
  story; DB-backed character tests prove create/load still work.
- **R3 — combat perf with data-interpreted effects.** Mitigation: deserialize once at load into a
  compact runtime form; benchmark against the existing determinism/replay harness.
- **R4 — migration gate UX.** Reuse SP1 `mcc diff`'s actionable report.
