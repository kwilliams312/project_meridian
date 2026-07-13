<!-- SPDX-License-Identifier: Apache-2.0 -->
# Design — Sub-project 1: Pack contract + content schema + catalogs

- **Status:** DRAFT (2026-07-13)
- **Parent:** [Moddable Theme Platform](2026-07-13-moddable-theme-platform-design.md) — sub-project 1 (the foundation).
- **Scope:** the **data model** only. Add/extend the `schema/content/*.schema.yaml` schemas and
  `mcc`/`validate_content` rules that let a pack carry a fully data-driven roster (races, classes),
  the rules-data catalogs (spells, armor/weapon types, attributes, talents), and the trust/compat
  machinery (`content_hash`, append-only + breaking-change gate). **No kernel behavior** (that is
  sub-project 2) and **no authoring tool** (sub-project 3).

## 1. What already exists (build on, don't reinvent)

- `schema/content/ability.schema.yaml` (`meridian/ability@1`) — spells/abilities as **primitive
  recipes**: `effects[]` of `{kind: damage|heal|…}` with `target`, `school`, `cast`, `cooldown_ms`,
  `resource`, `triggers_gcd`. This is the Tier-1 ability model; we **extend its effect palette**.
- `schema/content/pack.schema.yaml` (`meridian/pack@1`) — `namespace`, `version` (semver),
  `content_schema_version`, `engine.godot`, `dependencies`, `authors`, `license`. We **add**
  `compatibility_version` (+ optional theme metadata).
- `schema/content/item.schema.yaml`, `asset.schema.yaml`, `npc.schema.yaml`, `quest.schema.yaml`,
  `spawn.schema.yaml`, `loot.schema.yaml`, `dye.schema.yaml`, `appearance.schema.yaml`,
  `common.defs.yaml` ($defs: `contentId`, `displayName`, …).
- `validate_content.py` (schema + L0xx lints) and `mcc` (C++ validator/packer, emits pack
  `content_hash`) — the two validators kept in parity by the `mcc-parity` CI job.
- `server/characters/src/roster.h` — the hardcoded enum to **retire** (its consumption moves to
  sub-project 2; this sub-project provides the data schemas that replace it).

Convention for all new schemas: JSON-Schema-draft-2020-12 YAML, `additionalProperties: false`,
`schema: { const: "meridian/<name>@1" }`, ids via `common.defs.yaml#/$defs/contentId`, names via
`displayName`. Each schema is self-contained (local `$defs`); cross-entity references are by id
string and validated at the content level (below), not by file imports — so the schema files can be
authored independently.

## 2. New & extended schemas

### 2.1 `equip_type.schema.yaml` — `meridian/equip_type@1` (NEW)
The armor-type and weapon-type catalogs. One schema, a `category` field distinguishes them.
- Fields: `schema`, `id`, `name`, `description?`, `category: { enum: [armor, weapon] }`,
  `slot_class?` (armor: helm/chest/… ; weapon: main/off/two-hand — informational).
- **Seed catalog** (data, editable): armor = Cloth, Leather, Mail, Plate; weapon = Two-Hand,
  One-Hand, Wand, Staff.
- **Item integration:** `item.schema.yaml` gains an `equip_type` field referencing an
  `equip_type` id (reconcile with any existing item type field; keep back-compat via an additive
  optional field). Class `usable_*_types` (§2.4) and item `equip_type` share this catalog.

### 2.2 `attribute.schema.yaml` — `meridian/attribute@1` (NEW, kernel-blessed set)
Declares the base attribute vocabulary the kernel formulas understand. Entries are **kernel-blessed**
(the fixed base set); operators *reference & tune* them, they do not add primaries (adding new
attributes is a later extension, per the umbrella §5).
- Fields: `schema`, `id`, `name`, `kind: { enum: [primary, derived] }`, `description?`.
- **Seed:** primary = strength, agility, intellect, stamina; derived = crit, haste, armor
  (extensible in a follow-on; keep the seed minimal and honest to what sub-project 2's formulas use).
- Add a shared `$defs/attributeMods` to `common.defs.yaml` (a map/array of `{attribute: <id>,
  value: <int>}`) reused by race and class. **(This is the one shared-file edit — owned by the
  attribute story so parallel stories don't collide on `common.defs.yaml`.)**

### 2.3 `race.schema.yaml` — `meridian/race@1` (NEW)
- Fields: `schema`, `id`, `name`, `description?`, `appearance` (ref to an `appearance`/material —
  the cosmetic body/material variant), `attribute_mods?` (`$defs/attributeMods`; **zeroed/absent for
  the Chibi theme** — the capability exists, the theme doesn't use it).
- No mechanical fields beyond the optional tuning hook.

### 2.4 `class.schema.yaml` — `meridian/class@1` (NEW — the 7-field record)
- `schema`, `id`, `name`, `description?`
- `abilities`: array of `ability` ids (the class's spellbook).
- `usable_armor_types`: array of `equip_type` ids with `category: armor`.
- `usable_weapon_types`: array of `equip_type` ids with `category: weapon`.
- `role`: `{ enum: [healer, dps_melee, dps_ranged, tank] }` **or** `hybrid: [<role>, …]` (a class is
  a single role or a hybrid list; exactly one of `role`/`hybrid`).
- `attribute_mods?`: `$defs/attributeMods` (per-class bonuses/penalties).
- `race_limits?`: array of `race` ids (omitted/empty = all races; a content/lore gate, not a stat).
- `talent_tree?`: a `talent_tree` id (§2.5).

### 2.5 `talent.schema.yaml` — `meridian/talent@1` + talent tree (NEW)
- `talent`: `schema`, `id`, `name`, `description?`, `grants` (array of ability ids and/or passive
  `effects[]` reusing the ability effect-primitive `$defs`), `rank_max?`.
- `talent_tree`: `schema`, `id`, `name`, `tiers[]` of `{ required_points, talents: [<talent id>] }`
  — a simple tiered tree (row-unlock by points), no arbitrary graph in v1 (YAGNI).

### 2.6 Extend `ability.schema.yaml` — effect-primitive palette
Add effect `kind`s to the existing `effects[].oneOf` (keep `damage`/`heal`): `dot`, `hot`,
`buff`, `debuff` (stat mods referencing `attribute` ids), `shield`, `cc` (`{ kind: cc, type:
[stun|root|silence], duration_ms }`), `resource` (grant/drain), `movement` (knockback/pull/dash),
`summon`. Bump `effects.maxItems` if needed for realistic kits. **This is the Tier-1 palette
foundation; it must stay deterministic and server-authoritative.**

### 2.7 Extend `pack.schema.yaml` — theme + compatibility
- Add `compatibility_version: { type: integer, minimum: 1 }` — bumps **only** on breaking
  (non-additive) id changes (§3). Distinct from semver `version`.
- Add optional theme metadata (`theme: { display_name?, tagline?, preview_asset? }`) — thin; the
  full UI-theme/audio folding is sub-project 4.

## 3. Trust, `content_hash` & compatibility rules (mcc + validate_content)

- **`content_hash`:** `mcc` computes a pack-level digest over the canonicalized content set (extend
  the existing chunk-pack hashing to the whole pack). This is the client↔server agreement token.
- **Append-only discipline (lint):** within a namespace's `idmap.lock`, an id **never** changes
  meaning; ids are append-only. `validate_content`/`mcc` fail on a renumber/reuse. (Formalizes the
  existing `roster.h` rule as a content lint.)
- **Breaking-change diff:** `mcc` gains a `diff <old-pack> <new-pack>` mode that classifies changes
  as **additive** (new ids, new optional fields) or **breaking** (removed/renumbered id, removed
  required capability). Breaking → non-zero exit + an actionable report naming the exact ids/fields.
- **Boot gate contract (spec only here; enforced by sub-project 2):** a realm records the pack
  `compatibility_version` it booted with; a higher-with-breaking-diff pack refuses to boot until an
  operator migration step runs. Additive boots freely.
- **Cross-reference validation:** `validate_content`/`mcc` verify every reference resolves — a
  class's `abilities`/`usable_*_types`/`race_limits`/`talent_tree`, a race's `appearance`, a
  talent's `grants`, an item's `equip_type`. Dangling reference = error.

## 4. Non-goals (explicitly out of sub-project 1)

- Kernel consumption of any of this (attribute formulas, ability execution, equip-gating, roster
  loading, the boot-migration enforcement) — **sub-project 2**.
- The operator authoring UI — **sub-project 3**.
- UI-theme/audio manifest folding + realm selection — **sub-project 4**.
- Any actual Chibi content — **sub-project 5**.
- Tier-2 scripting effect kinds — **sub-project 6**.

## 5. Story decomposition (implementation)

Schema files are independent (self-contained `$defs`, cross-refs by id); the only shared-file edit
is `common.defs.yaml#/$defs/attributeMods`, owned by 1.2. Cross-reference *validation* lands with
the referencing schema's story or the seed-pack story.

| Story | Deliverable | Files (distinct) | Depends |
|-------|-------------|------------------|---------|
| **1.1** | `equip_type` schema + seed armor/weapon catalogs + item `equip_type` field | `schema/content/equip_type.schema.yaml`, `item.schema.yaml`, `content/<ns>/…` seed, lints | — |
| **1.2** | `attribute` schema + kernel-blessed seed set + `attributeMods` shared def | `schema/content/attribute.schema.yaml`, `common.defs.yaml` | — |
| **1.3** | Extend ability effect-primitive palette (dot/hot/buff/debuff/shield/cc/resource/movement/summon) | `schema/content/ability.schema.yaml`, tests | — |
| **1.7** | `pack.schema.yaml` `compatibility_version` + theme meta; `mcc diff` breaking-change classifier; append-only lint; pack `content_hash` extension | `pack.schema.yaml`, `tools/mcc/*`, `validate_content.py`, `mcc-parity` | — |
| **1.4** | `race` schema + validation | `schema/content/race.schema.yaml` | 1.2 |
| **1.6** | `talent` + `talent_tree` schemas | `schema/content/talent.schema.yaml` | 1.3 |
| **1.5** | `class` schema (7-field) + cross-ref validation | `schema/content/class.schema.yaml`, validators | 1.1,1.2,1.3,1.4,1.6 |
| **1.8** | Seed skeleton pack (races/class/abilities/talents) passing `mcc`+`validate_content`; fixture | `content/<ns>/…`, tests | 1.1–1.7 |

**Parallel wave 1 (independent, distinct files):** 1.1, 1.2, 1.3, 1.7.
**Wave 2:** 1.4, 1.6. **Wave 3:** 1.5. **Wave 4 (integration):** 1.8.

Each story: TDD (schema fixtures + `validate_content`/`mcc` cases), keep `validate`/`mcc`/`mcc-parity`
CI green, PR into `dev`, QA gate, lead review, merge.

## 6. Verification

- `validate_content.py` and `mcc` accept the seed skeleton pack (1.8) and reject crafted-invalid
  fixtures (dangling ref, renumbered id, unknown attribute, over-4 effects, etc.).
- `mcc-parity` stays green (both validators agree on the extended corpus).
- `mcc diff` reports a hand-authored breaking change as breaking and an additive one as additive.
- Round-trip: the seed pack's `content_hash` is stable across runs and changes iff content changes.
