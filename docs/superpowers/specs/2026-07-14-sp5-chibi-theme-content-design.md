<!-- SPDX-License-Identifier: Apache-2.0 -->
# Design — Sub-project 5: Chibi theme content

- **Status:** DRAFT (2026-07-14)
- **Parent:** [Moddable Theme Platform](2026-07-13-moddable-theme-platform-design.md) — sub-project 5.
- **Depends on:** SP1 (content schemas) ✅ + SP2 (kernel that runs them) ✅. This sub-project is the
  first full **content payload** — the chibi theme the user pivoted to. It proves the platform by
  delivering a real, different game as data + art.

## 1. Vision

A bright **chibi mascot** world (per the user's reference art: rounded knights/wizards, big eyes,
**flat body colors + metallic armor**, a whimsical fairytale village with a castle, waterfalls,
mushrooms, lanterns, day/night). **6 races = colors** — Flat (Red, Green, Blue, Yellow) + Metallic
(Gold, Silver), purely cosmetic (one base + material swaps). **4 classes** — Warrior, Mage, Rogue,
Priest — as real, kernel-driven content.

## 2. The critical reality: data vs. art, and the human gates

SP1/SP2 were deterministic code/data the autonomous loop produced + merged via QA. **SP5 is art +
game-design and has binding HUMAN gates** the loop cannot substitute for:

- **Art restyle (binding, CONTRIBUTING.md / TD-09):** Meshy/AI meshes land `restyle_status:
  pending` — quarantined, **cannot merge as final** until a human restyle pass sets it `done`.
- **Aesthetic sign-off:** "does this read as chibi / look good" is a human judgment, not a QA gate.
- **UI E2E:** any client-facing skin work needs the human-gated E2E test.

So SP5 is delivered in two tracks:

- **AUTONOMOUS (data, no art dependency) — the loop drives, QA gates, merges:**
  the 4 classes + their abilities/talents/equip-proficiencies/starter items; catalogs (armor/weapon
  types already seeded in SP1); the Terrain3D material *authoring* + world-assembly scaffolding; and
  Meshy **intake candidates** (generated, quarantined, handed to the human).
- **HUMAN-GATED (art) — the loop assists (intake), the human finishes:**
  the chibi base character (skeleton + body model), the 6 race appearances, armor/weapon meshes,
  the built environment + foliage + terrain look, creatures/NPCs/mounts, VFX/skybox/weather, the UI
  skin, icons, map. Raw intake → **human restyle** → merge.

**The 6 races are art-gated:** a race needs an `appearance` (skeleton + body_model + presets). All 6
chibi races share ONE chibi base skeleton+body (color/material is the variation), so they unblock as
soon as that one base is restyled — then the 6 races land as data (each an appearance/material
variant), which also closes #708 (full `roster.h` deletion).

## 3. Autonomous-first slice: the 4 chibi classes (data)

The immediately-buildable, cleanly-mergeable slice — makes the game *mechanically chibi* on the SP2
kernel without waiting on any art. Default class designs (classic archetypes; tune later via SP3
tools or a follow-up — flagged for the user):

| Class | Role | Usable armor | Usable weapons | Starter kit (abilities) |
|-------|------|--------------|----------------|-------------------------|
| **Warrior** | tank | Plate | 2H, 1H (+shield) | melee strike (damage), a taunt/threat (role hook), a shield/absorb, a cc (stun) |
| **Mage** | dps_ranged | Cloth | Staff, Wand | a bolt (damage), a dot (burn), an aoe, a cc (root) |
| **Rogue** | dps_melee | Leather | 1H | a strike (damage), a bleed (dot), a movement (dash), a debuff |
| **Priest** | healer | Cloth | Wand, Staff | a heal, a hot, a shield, a debuff/dispel |

Each class: a `class` record (SP1 7-field, roster_id appended after existing 4 — append-only), an
ability set (SP1 `ability` recipes using the SP2 Tier-1 palette — all executable today), a small
`talent_tree`, `usable_armor_types`/`usable_weapon_types` (SP1 equip_type catalog), `attribute_mods`,
and `race_limits` = all (chibi races are cosmetic, no gating). Starter items per class
(armor/weapon of the right equip_type). All DATA — validates via `validate_content`/`mcc`, runs on
the SP2 kernel, DB-round-trip provable (the SP1.8/SP2 discipline).

*This is where the platform pays off: a whole new class roster shipped as pack content, zero engine
change.*

## 4. Workstreams (the full chibi theme)

| # | Workstream | Track | Notes |
|---|-----------|-------|-------|
| **5-data** | 4 classes + abilities/talents/equip/items (§3); then 6 race records once §5a base exists | Autonomous (races gated on 5a) | first slice |
| **5a** | Chibi base character (skeleton + body model) + 6 material/color race appearances + hair/face presets | Meshy intake → **human restyle** | unblocks the 6 races + #708 |
| **5b** | Gear: `armor_model` sets (cloth/leather/mail/plate) + `weapon_model` (2H/1H/wand/staff), chibi-styled | Meshy intake → human restyle | |
| **5c** | Built env: `kit_piece` (houses/castle/walls/bridges/interiors) · `prop` (lanterns/stalls) · `hero_landmark` (castle) | Meshy intake → human restyle | |
| **5d** | Nature + terrain: `foliage` (trees/bushes/flowers/mushrooms) · rock/cliff/water · **Terrain3D material set** | intake + terrain authoring | material partly autonomous |
| **5e** | Creatures/NPCs/mounts: `creature_model` + NPC looks + mounts | Meshy intake → human restyle | |
| **5f** | Mood + client: VFX · day/night skybox + weather · **UI skin** · `icon` set · `map` · audio | art + **UI E2E (human)** | |
| **5g** | World assembly: zones in Forge from the kits · quests · encounters wiring the class/creature data | autonomous scaffolding + human world-building | |

## 5. Order

1. **5-data classes** (§3) — autonomous, now. (Ships the chibi combat roster.)
2. **5a chibi base character intake** — generate candidates; **hand to human for restyle**. Once the
   restyled base lands, the **6 race records** ship as data (§5-data races).
3. **5b–5e art intake** — generate chibi candidates per workstream, human restyle. Parallelizable
   intake; serialized human restyle.
4. **5d terrain material + 5g world assembly** — as kits land.
5. **5f UI skin + mood + client** — art + the human UI-E2E gate.

## 6. Verification

- **Data (5-data, 5g quests):** the SP1/SP2 gates — `validate_content`/`mcc` parity, DB-backed
  MariaDB load, and **runtime**: worldd boots the chibi pack, a character is created as a chibi
  race+class, equips class gear, and casts the class's abilities (the SP2 kernel executes them).
- **Art:** intake candidates pass the TD-09 provenance lints (`restyle_status: pending` expected);
  final art passes the import validator (LOD/pivots/budgets) after the **human restyle**; the human
  aesthetic sign-off + UI E2E are explicit gates.

## 7. Risks / open decisions

- **R1 — Restyle throughput is the bottleneck.** Raw chibi art can't ship without human restyle; the
  visible world advances only as fast as that. The autonomous loop maximizes *data* + *intake
  candidates* so the human work is restyle-only, not authoring-from-scratch.
- **Q1 — Class designs (§3) are defaults.** The user should confirm/tune the 4 classes' roles,
  proficiencies, and ability kits (this is game design). Surfaced, not assumed-final.
- **Q2 — Restyle actor.** Who does the restyle pass — the user, a contracted artist, or an
  AI-assisted restyle tool? Determines throughput; out of this spec's autonomous scope.
- **R2 — Retire the realistic `core` content.** As chibi content lands, the realistic seed
  (ardent/dolmen appearances, Warden's Kit) is retired per the umbrella §8; sequence carefully so the
  pack always validates.
