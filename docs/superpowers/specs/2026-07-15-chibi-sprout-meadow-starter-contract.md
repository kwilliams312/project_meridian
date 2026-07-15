<!-- SPDX-License-Identifier: Apache-2.0 -->
# Chibi Sprout Meadow Starter content and art contract

**Status:** binding MVP contract  
**Stories:** #780, part of #774 and follow-on to #753  
**Pack:** `content/chibi` (`chibi` namespace)  
**Zone:** `chibi:zone.sprout_meadow`

This contract freezes the narrative, permanent IDs, reference graph, ownership,
and art handoff for the level 1–3 Sprout Meadow creator-tools acceptance demo.
Implementations may tune values only where this document assigns that ownership.
They must not rename a listed ID. Once an ID ships, retirement uses
`deprecated: true` plus `superseded_by`; filenames, source paths, and remote job
IDs are never content references.

## 1. Scope and verified baseline

The demo extends the self-contained Chibi pack delivered by #753. At the time of
this contract, `dev` contains six color races, twelve appearance catalogs, four
classes, sixteen class abilities, eight class starter items, the base cloth wrap,
and Sprout Meadow. Those existing IDs remain permanent.

The player flow is:

1. Create any Chibi race and class and enter Sprout Meadow in the base cloth wrap.
2. Meet Pippa Patch and deliver her Meadow Token to Tink Tender.
3. Purchase the class-compatible weapon and chest item from Tink for zero copper.
4. Defeat Dewdrop Slimes, collect Gleam Gels, and receive a Sunny Sip.
5. Defeat Sproutcap Scamps, recover Pippa's ribbon, and receive and equip the
   Meadow Medal.

The current `meridian/npc@1` schema represents friendly service NPCs and hostile
mobs with the same entity type and `npc` ID grammar. They are nevertheless
different authoring roles:

- **NPC mode** is interaction-first: friendly faction, dialogue, quests, vendor
  or trainer services, and interaction readiness.
- **Mob mode** is combat-first: hostile faction, combat stats, AI/rotation,
  movement, loot, and spawn readiness.

Codex must expose those as distinct workflows under #676 even while both emit
`meridian/npc@1`. A mob is not a dialogue-less NPC preset in the user experience.

## 2. Canonical content manifest

All references below are same-pack `chibi:` references. The four class gear pairs
already exist and keep their current IDs.

| Role | Permanent ID | Display name | Owner |
|---|---|---|---|
| NPC | `chibi:npc.pippa_patch` | Pippa Patch | #781 |
| NPC | `chibi:npc.tink_tender` | Tink Tender | #781 |
| Mob | `chibi:npc.dewdrop_slime` | Dewdrop Slime | #775 |
| Mob | `chibi:npc.sproutcap_scamp` | Sproutcap Scamp | #775 |
| Vendor | `chibi:vendor.tink_starter_outfitter` | Tink starter outfitter | #781 |
| Quest item | `chibi:item.meadow_token` | Meadow Token | #775 |
| Quest item | `chibi:item.gleam_gel` | Gleam Gel | #775 |
| Consumable | `chibi:item.sunny_sip` | Sunny Sip | #775 |
| Quest item | `chibi:item.pippas_meadow_ribbon` | Pippa's Meadow Ribbon | #775 |
| Reward | `chibi:item.meadow_medal` | Meadow Medal | #775 |
| Ability | `chibi:ability.sunny_sip` | Sunny Sip | #775 |
| Loot | `chibi:loot.dewdrop_slime` | Dewdrop Slime drops | #775 |
| Loot | `chibi:loot.sproutcap_scamp` | Sproutcap Scamp drops | #775 |
| Quest | `chibi:quest.a_pack_for_the_path` | A Pack for the Path | #782 |
| Quest | `chibi:quest.a_little_less_goo` | A Little Less Goo | #782 |
| Quest | `chibi:quest.caps_off` | Caps Off! | #782 |
| Spawn group | `chibi:spawn.sprout_meadow_guides` | Pippa and Tink placements | #779 |
| Spawn group | `chibi:spawn.sprout_meadow_dewdrop_slimes` | Slime placements | #779 |
| Spawn group | `chibi:spawn.sprout_meadow_sproutcap_scamps` | Scamp placements | #779 |

Existing starter inventory, stocked by Tink without renaming:

| Class | Weapon | Chest |
|---|---|---|
| Warrior | `chibi:item.warrior_battleblade` | `chibi:item.warrior_platemail` |
| Mage | `chibi:item.mage_apprentice_staff` | `chibi:item.mage_apprentice_robe` |
| Rogue | `chibi:item.rogue_shiv` | `chibi:item.rogue_leathers` |
| Priest | `chibi:item.priest_prayer_wand` | `chibi:item.priest_vestments` |

Each vendor row uses `price_override: 0`. Class compatibility remains
server-authoritative through the existing item `equip_type` and class
`usable_*_types` references; the vendor does not duplicate class gates.

## 3. Binding narrative and quest graph

All text is original Meridian content. Pippa is an upbeat meadow guide. Tink is a
practical quartermaster. The tone is warm and lightly playful; danger is clear but
not grim.

### 3.1 A Pack for the Path

- **ID:** `chibi:quest.a_pack_for_the_path`
- **Level / required level:** 1 / 1
- **Giver / turn-in:** `chibi:npc.pippa_patch` / `chibi:npc.tink_tender`
- **Objective:** deliver `chibi:item.meadow_token` to
  `chibi:npc.tink_tender`. The quest system provides the token on accept.
- **Prerequisite:** none
- **Reward:** 10 XP
- **Summary:** “Take Pippa's Meadow Token to Tink Tender at the supply cart.”
- **Offer text:** “Oh, good—you’re awake! Sprout Meadow is gentle, but it still
  pays to be prepared. Take this token to Tink by the supply cart. Tink will help
  you choose gear that fits how you fight.”
- **Completion text:** “Pippa's mark—good. Pick one weapon and one chest piece
  suited to your class. No coin today; the meadow needs you ready, not rich.”

The zero-cost purchase is deliberately a vendor interaction after turn-in, not a
quest objective: `meridian/quest@1` has no purchase objective. #783 verifies the
purchase and equip step explicitly.

### 3.2 A Little Less Goo

- **ID:** `chibi:quest.a_little_less_goo`
- **Level / required level:** 1 / 1
- **Giver / turn-in:** Pippa / Pippa
- **Prerequisite:** `chibi:quest.a_pack_for_the_path`
- **Objectives:** kill 3 `chibi:npc.dewdrop_slime`; collect 3
  `chibi:item.gleam_gel`.
- **Reward:** 20 XP and 1 `chibi:item.sunny_sip`
- **Summary:** “Clear three Dewdrop Slimes and bring Pippa three Gleam Gels.”
- **Offer text:** “Those dewdrops have grown wobbly teeth and a taste for garden
  boots. Clear three from the near field and bring me their Gleam Gels. Your new
  kit should make the work quick.”
- **Completion text:** “Bright, bouncy, and safely in a jar. Well done! Take this
  Sunny Sip. If the next patch gets prickly, one drink will put some spring back
  in your step.”

`chibi:loot.dewdrop_slime` grants exactly one Gleam Gel at 100 percent while this
quest's collection objective is incomplete. Three eligible kills therefore
cannot dead-end the objective.

### 3.3 Caps Off!

- **ID:** `chibi:quest.caps_off`
- **Level / required level:** 2 / 2
- **Giver / turn-in:** Pippa / Pippa
- **Prerequisite:** `chibi:quest.a_little_less_goo`
- **Objectives:** kill 4 `chibi:npc.sproutcap_scamp`; collect 1
  `chibi:item.pippas_meadow_ribbon`.
- **Reward:** 30 XP and 1 `chibi:item.meadow_medal`
- **Summary:** “Drive off four Sproutcap Scamps and recover Pippa's meadow ribbon.”
- **Offer text:** “The Sproutcap Scamps saw all that splashing and stole my meadow
  ribbon in the fuss. They’re hiding beyond the slime patch. Chase off four and
  bring the ribbon home.”
- **Completion text:** “My ribbon—and not a nibble missing! You’ve crossed the
  whole meadow like a proper helper. Wear this Meadow Medal so everyone knows
  Sprout Meadow can count on you.”

`chibi:loot.sproutcap_scamp` grants one ribbon at 100 percent while its collection
objective is incomplete. The remaining kills still advance the kill objective.

The chain is linear and acyclic:

```text
Pippa --Meadow Token--> Tink --free starter gear--> player
  |
  +-- A Pack for the Path
        -> A Little Less Goo --3 Slimes/3 Gels--> Sunny Sip
             -> Caps Off! --4 Scamps/1 Ribbon--> Meadow Medal
```

Kill XP uses the current clean-room leveling curve in addition to quest XP. The
three level-1 and four level-2 kills plus 60 quest XP take a new level-1 character
through levels 1, 2, and 3 under the current thresholds (50 XP and 120 XP). #782
must retain a runtime assertion for this result rather than relying on the prose.

## 4. Entity targets

These values are the shared input to #775 and the #776 benchmark. Changing a mob
profile requires both owners to update their evidence in the same integration
window.

| Field | Dewdrop Slime | Sproutcap Scamp |
|---|---:|---:|
| Level | 1 | 2 |
| Faction / AI | hostile / aggressive | hostile / aggressive |
| Health | 100 | 140 |
| Armor | 0 | 5 |
| Damage | 3–5 | 5–8 |
| Attack speed | 2400 ms | 2200 ms |
| Aggro radius | 8 m | 10 m |
| Leash radius | 18 m | 22 m |
| Call for help | 0 m | 0 m |
| Walk / run | 1.5 / 3.0 m/s | 2.0 / 4.0 m/s |
| Ability rotation | none | none |
| Loot | `chibi:loot.dewdrop_slime` | `chibi:loot.sproutcap_scamp` |

Pippa and Tink are level-1 friendly humanoids with passive behavior, 100 health,
1–1 required placeholder damage, and 2000 ms attack speed. Pippa carries guide
gossip; Tink carries outfitter gossip plus the canonical vendor reference.
Friendly NPC combat fields exist only because `meridian/npc@1` requires them.

The new item targets are:

- Meadow Token, Gleam Gel, and Pippa's Meadow Ribbon: `item_class: quest`, common,
  non-equippable, no buy/sell price; Gel stacks to at least 3.
- Sunny Sip: `item_class: consumable`, common, stack size 5, `effects.on_use` =
  `chibi:ability.sunny_sip`. The ability targets self, costs no resource, uses the
  nature school, and heals 30–40 with a 30-second cooldown.
- Meadow Medal: `item_class: armor`, `subclass: trinket`, `slot: trinket`, uncommon,
  required level 2, non-visible, and grants `stamina: 1`. It has no `visual.worn`.

### Verified runtime seam

At contract time, the world runtime does not yet feed authored NPC basic-attack
damage/speed/armor or equipped item stats into combat, Creature AI does not roll
basic attacks, and ability coefficients are parsed but not applied. Therefore the
survival/equipment half of #776's DB-backed benchmark is a real implementation
dependency, not evidence that can be fabricated by data-only tests. Data authors
still use the frozen values above; #776/#783 must either land the required runtime
seam under tracked scope or record the blocker before claiming that acceptance.

## 5. Canonical art handoff

Content records refer only to the IDs in this section. They never refer to a
source file, remote Meshy task ID, or temporary `core:` placeholder. `icon` fields
accept class `icon`; NPC `visual.model` accepts `creature_model`; worn weapon and
armor records accept `weapon_model` and `armor_model` respectively.

Every row is **planned** until its source and `meridian/asset@1` sidecar merge.
Every sidecar records the actual source tier and license. AI-assisted rows require
the named auditable prompt record, an origin URL in the sidecar, human restyling,
`restyle_status: done`, and the normal two-reviewer style gate before #778 may
wire them. If an art owner instead produces a genuinely original row, it records
`source_tier: original`, uses the same permanent ID, and marks restyle status
`not_applicable`; the prompt record is retained as the visual brief and must not
misrepresent provenance.

### 5.1 Characters and creatures

| Permanent asset ID | Class | Prompt record | Required readiness for wiring |
|---|---|---|---|
| `chibi:art.creature.pippa_patch` | `creature_model` | `pippa_patch.prompts.yaml` | sidecar merged; `done` if AI |
| `chibi:art.creature.tink_tender` | `creature_model` | `tink_tender.prompts.yaml` | sidecar merged; `done` if AI |
| `chibi:art.creature.dewdrop_slime` | `creature_model` | `dewdrop_slime.prompts.yaml` | sidecar merged; `done` if AI |
| `chibi:art.creature.sproutcap_scamp` | `creature_model` | `sproutcap_scamp.prompts.yaml` | sidecar merged; `done` if AI |

### 5.2 Existing outfit and starter gear

| Content item | Permanent icon ID (`icon`) | Permanent worn-model ID (class) | Prompt record stem |
|---|---|---|---|
| `chibi:item.base_cloth_wrap` | `chibi:art.icon.item.base_cloth_wrap` | `chibi:art.item.armor.base_cloth_wrap` (`armor_model`) | `base_cloth_wrap` |
| `chibi:item.warrior_battleblade` | `chibi:art.icon.item.warrior_battleblade` | `chibi:art.item.weapon.warrior_battleblade` (`weapon_model`) | `warrior_battleblade` |
| `chibi:item.warrior_platemail` | `chibi:art.icon.item.warrior_platemail` | `chibi:art.item.armor.warrior_platemail` (`armor_model`) | `warrior_platemail` |
| `chibi:item.mage_apprentice_staff` | `chibi:art.icon.item.mage_apprentice_staff` | `chibi:art.item.weapon.mage_apprentice_staff` (`weapon_model`) | `mage_apprentice_staff` |
| `chibi:item.mage_apprentice_robe` | `chibi:art.icon.item.mage_apprentice_robe` | `chibi:art.item.armor.mage_apprentice_robe` (`armor_model`) | `mage_apprentice_robe` |
| `chibi:item.rogue_shiv` | `chibi:art.icon.item.rogue_shiv` | `chibi:art.item.weapon.rogue_shiv` (`weapon_model`) | `rogue_shiv` |
| `chibi:item.rogue_leathers` | `chibi:art.icon.item.rogue_leathers` | `chibi:art.item.armor.rogue_leathers` (`armor_model`) | `rogue_leathers` |
| `chibi:item.priest_prayer_wand` | `chibi:art.icon.item.priest_prayer_wand` | `chibi:art.item.weapon.priest_prayer_wand` (`weapon_model`) | `priest_prayer_wand` |
| `chibi:item.priest_vestments` | `chibi:art.icon.item.priest_vestments` | `chibi:art.item.armor.priest_vestments` (`armor_model`) | `priest_vestments` |

Each stem expands to `<stem>.prompts.yaml`. Icon and model sidecars are separate
IF-8 records even if both are derived from one approved visual brief.

### 5.3 New item icons

| Content item | Permanent asset ID | Class | Prompt record | Required readiness |
|---|---|---|---|---|
| Meadow Token | `chibi:art.icon.item.meadow_token` | `icon` | `meadow_token.prompts.yaml` | sidecar merged; `done` if AI |
| Gleam Gel | `chibi:art.icon.item.gleam_gel` | `icon` | `gleam_gel.prompts.yaml` | sidecar merged; `done` if AI |
| Sunny Sip | `chibi:art.icon.item.sunny_sip` | `icon` | `sunny_sip.prompts.yaml` | sidecar merged; `done` if AI |
| Pippa's ribbon | `chibi:art.icon.item.pippas_meadow_ribbon` | `icon` | `pippas_meadow_ribbon.prompts.yaml` | sidecar merged; `done` if AI |
| Meadow Medal | `chibi:art.icon.item.meadow_medal` | `icon` | `meadow_medal.prompts.yaml` | sidecar merged; `done` if AI |

Icons are not Meshy candidates under the current item schema. Creature and worn
3D models are Meshy-eligible candidates, but generation is only intake: raw output
stays quarantined until provenance, budget, import, and human restyle gates pass.

## 6. Reference edges

The implementation must preserve these typed edges:

- Pippa and Tink `visual.model` -> their creature asset IDs after #778.
- Tink `interaction.vendor` -> `chibi:vendor.tink_starter_outfitter`.
- The vendor -> all eight existing starter item IDs, each at zero copper.
- Each mob `loot.table` -> its canonical loot ID; each mob `visual.model` -> its
  creature asset ID after #778.
- Slime loot -> Gleam Gel, gated by A Little Less Goo.
- Scamp loot -> Pippa's Meadow Ribbon, gated by Caps Off!.
- Sunny Sip item `effects.on_use` -> Sunny Sip ability.
- Each quest -> canonical giver, turn-in, zone, objective, prerequisite, and reward
  IDs from sections 2–3.
- Every spawn group `zone` -> Sprout Meadow and each spawn row `npc` -> the
  matching canonical NPC/mob ID.
- Every item `visual.icon` and visible gear `visual.worn.models[].model` -> the
  canonical Chibi art IDs in section 5.

Backlinks expected in Codex include Tink <- vendor/quest, each mob <- quest/loot/
spawn, each quest item <- quest/loot, each starter item <- vendor, and every art
asset <- its consuming NPC or item. #670 owns diagnostics/backlinks and #671 owns
safe rename/delete behavior; permanent-ID policy still applies if those tools are
not yet available.

## 7. Spatial boundary: Codex versus Forge

Codex owns non-spatial content authoring and reference integrity: NPC/mob
definitions, items, abilities, vendor inventory, loot, quests, validation, diff,
and package/build. It may show spawn-readiness diagnostics, but it does not choose
world coordinates.

Forge owns spatial placement: positions, orientations, wander radii, patrols, and
the three `meridian/spawn@1` files. #779 places Pippa and Tink near the entry, a
safe slime field next, and a distinct Sproutcap field beyond it. Forge exports
zone-local coordinates; it must not create or rename content entities.

This boundary is intentional. “Mob editor” means combat entity authoring in
Codex; it is not the future weighted spawn-table editor, and it is not Forge's
placement gizmo.

## 8. Ownership and dependency order

| Story | Exclusive file/entity ownership in this epic | Depends on |
|---|---|---|
| #780 | This contract only; no content data | merged #753 foundations |
| #776 | Existing Chibi class/ability tuning and benchmark fixtures only | #780 profiles; runtime seam noted above |
| #775 | Two mobs, five items, two loot tables, and the Sunny Sip ability | #780 |
| #781 | Pippa, Tink, vendor; no quest/spawn files | #780; merged classes/items |
| #782 | Three quest files and narrative implementation only | merged #775 and #781 |
| #768 + art-track PRs | IF-8 assets for the permanent section-5 IDs | #780 manifest; human provenance/restyle gates |
| #778 | Replace content visual placeholders/omissions with merged section-5 IDs | merged content plus approved art |
| #779 | Three spawn files and Forge placement evidence only | merged #775 and #781 |
| #777 | Executable Codex/Forge acceptance guide; no content | #780 terminology and graph |
| #783 | Deterministic build, DB/runtime proof, creator-tools proof, human E2E | #753 C9 and every story above |

#775 and #781 may run in parallel. #776 and art creation may also run in parallel
because they communicate only through this contract. Content branches do not
copy art binaries or invent temporary IDs. A branch may omit an optional NPC
visual until #778, but a required item icon must resolve before that content can
claim final pack acceptance.

The content-file ownership is exact:

- #775 owns `content/chibi/npcs/{dewdrop_slime,sproutcap_scamp}.npc.yaml`, the
  `content/chibi/items/{meadow_token,gleam_gel,sunny_sip,pippas_meadow_ribbon,meadow_medal}.item.yaml`,
  `content/chibi/abilities/sunny_sip.ability.yaml`, and
  `content/chibi/loot/{dewdrop_slime,sproutcap_scamp}.loot.yaml`.
- #781 owns `content/chibi/npcs/{pippa_patch,tink_tender}.npc.yaml` and
  `content/chibi/vendors/tink_starter_outfitter.vendor.yaml`.
- #782 owns
  `content/chibi/quests/{a_pack_for_the_path,a_little_less_goo,caps_off}.quest.yaml`.
- #779 owns
  `content/chibi/spawns/{sprout_meadow_guides,sprout_meadow_dewdrop_slimes,sprout_meadow_sproutcap_scamps}.spawn.yaml`.
- The art track owns IF-8 records and their sources/prompts. #778 owns only the
  content-record edits that wire approved asset IDs into consumers.

## 9. Validation and provenance gates

Every implementation PR must run the checks appropriate to its files, with fresh
output in the PR:

```bash
uv run tools/validate_content.py
uv run tools/check_traceability.py
uv run pytest -q
```

Content/build stories also run `mcc` validation/build, deterministic diff/package,
and their DB/runtime tests as assigned in the issue. #783 executes the completed
guide without raw YAML and performs the final human client E2E.

All code and content are clean-room originals designed from repository schemas,
PRDs/SADs, and this contract. No GPL source, third-party game data, franchise
names, extracted assets, or recognizable lookalikes may enter the pack. Content
data is Apache-2.0. Art is CC0-1.0 or CC-BY-4.0 and must carry a complete IF-8
sidecar. AI prompts contain no artist or franchise terms; remote generation task
IDs belong only in provenance records, never in gameplay data or this ID graph.
