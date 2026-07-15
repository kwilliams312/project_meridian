<!-- SPDX-License-Identifier: Apache-2.0 -->
# Build the Chibi MVP with Meridian Codex and Forge

This is the executable authoring contract for the Chibi creator-tools demo
([#774](https://github.com/kwilliams312/project_meridian/issues/774)). It shows how an operator
must create a linked, playable level 1–3 experience under `content/chibi` without bypassing the
tools. The final proof is [#783](https://github.com/kwilliams312/project_meridian/issues/783).

This guide is intentionally stricter than the tools that exist today. It distinguishes the
completed-tool experience from temporary developer fallbacks so an unfinished editor is never
mistaken for accepted UX.

## Status legend

| Label | Meaning |
|---|---|
| **Available** | Works on the current `dev` branch through the named UI or command. |
| **Partial** | A safe subset works, but the completed workflow still depends on an open story. |
| **Temporary fallback** | Useful for developing the demo now, but may require raw YAML or a CLI. It is not acceptable evidence for #783's no-raw-YAML authoring proof. |
| **Target** | Required operator experience after the owning story lands. |
| **Blocked** | No truthful end-to-end path exists yet. Do not substitute a manual action and call it complete. |

The final acceptance run must use **Target** paths only. Temporary fallbacks exist to unblock
parallel content and art development, not to lower the product bar.

## The linked Chibi slice

The creator builds one graph, not a folder of unrelated records:

```text
six races -> appearance catalogs -> body/material/dye asset IDs
                                    |
four classes -> four abilities each |-> character presentation and combat kit
      |                             |
      +-> allowed equip types -> starter weapons and armor <- Tink's vendor

Pippa -> quest 1 -> delivery token -> Tink -> free starter purchase
Pippa -> quest 2 -> Dewdrop Slime -> loot -> quest collect -> consumable reward
Pippa -> quest 3 -> Sproutcap Scamp -> loot -> quest collect -> Meadow Medal

Sprout Meadow -> Forge-authored spawn placements -> Pippa, Tink, slimes, scamps
```

The approved demo contains:

- the six color races and their existing male/female appearance catalogs;
- Warrior, Mage, Rogue, and Priest, with four tuned abilities each;
- Pippa Patch (guide/quest giver) and Tink Tender (free starter outfitter);
- Dewdrop Slime and Sproutcap Scamp as distinct hostile mobs;
- **A Pack for the Path**, **A Little Less Goo**, and **Caps Off!** as one chain;
- class-compatible starter weapons and armor, a delivery token, collection items, a healing
  consumable, and the equippable **Meadow Medal** reward; and
- reachable placements in `chibi:zone.sprout_meadow` supporting level 1–3 play without waiting
  for respawns or involuntary multi-pulls.

The following IDs are the **approved proposed contract** from #780. They are included so parallel
stories converge, but #780 has not merged into `dev` yet; this table is not a landed registry.
#780 becomes authoritative only when it merges:

| Kind | Canonical IDs |
|---|---|
| Friendly NPCs | `chibi:npc.pippa_patch`, `chibi:npc.tink_tender` |
| Hostile mobs | `chibi:npc.dewdrop_slime`, `chibi:npc.sproutcap_scamp` |
| Vendor | `chibi:vendor.tink_starter_outfitter` |
| New items | `chibi:item.meadow_token`, `chibi:item.gleam_gel`, `chibi:item.sunny_sip`, `chibi:item.pippas_meadow_ribbon`, `chibi:item.meadow_medal` |
| Consumable ability | `chibi:ability.sunny_sip` |
| Loot | `chibi:loot.dewdrop_slime`, `chibi:loot.sproutcap_scamp` |
| Quests | `chibi:quest.a_pack_for_the_path`, `chibi:quest.a_little_less_goo`, `chibi:quest.caps_off` |
| Forge spawn groups | `chibi:spawn.sprout_meadow_guides`, `chibi:spawn.sprout_meadow_dewdrop_slimes`, `chibi:spawn.sprout_meadow_sproutcap_scamps` |
| Zone | `chibi:zone.sprout_meadow` |

The existing eight class-gear IDs remain unchanged. While #780 is pending, do not allocate a
competing alias in another story. After #780 merges and these IDs are allocated, treat them as
permanent and route any contract change through the tracker rather than silently renaming them.

## 1. Prepare and open the workspace

Run all commands from the repository root unless a step says otherwise.

```bash
uv sync --locked
dotnet restore tools/codex/Meridian.Codex.sln
dotnet build tools/codex/Meridian.Codex.sln --no-restore -m:1 --disable-build-servers
tools/codex/check-warning-clean.sh
dotnet run --project tools/codex/Meridian.Codex/Meridian.Codex.csproj
```

On the supported macOS development host, build `mcc` and the local server before using later CLI
and runtime steps:

```bash
scripts/dev/build.sh
```

In Codex, choose **File -> Open pack**, select `content/chibi`, and verify the Pack page shows
namespace `chibi`, version `0.1.0`, and the current aggregate validation state.

The current manifest displays Godot `4.6`, while both the game-client and Forge projects declare
Godot `4.7`. #787 owns this verified compatibility gap; it is not an instruction to edit the pin
casually. #787 must first establish whether the manifest is an exact pin or another compatibility
contract, then make the pack, build, Forge, and client pass that supported rule before #783 package
or runtime evidence is accepted.

**Available:** native pack picker, pack manifest editing, recent workspace history, dirty-state
tracking, conflict protection, validation, and Save. NPC and Item are the only normal content
editors currently wired into the left rail.

**Target (#670, #671, #681):** opening the pack loads its dependency-aware ID index, typed
references, backlinks, workspace diagnostics, lifecycle actions, and build/check/diff/package
status. No operator enters file paths or invokes a separate terminal during the final #783 run.

Capture:

- the pack header and clean aggregate status;
- the selected `content/chibi` path;
- the Codex commit SHA; and
- a clean `git status --short` before authoring.

## 2. Stable IDs, typed references, and lifecycle safety

Every content ID is a permanent public key. Files may move, but an allocated or released ID must
not be renamed, reused, or silently deleted. Race and class `roster_id` values and appearance
preset IDs are separately append-only because characters persist them directly.

**Target (#670):** every reference control filters to the required type and shows its owning pack.
Selecting a reference creates a navigable relationship; **Find usages** shows backlinks and the
field paths that consume the selected ID. A wrong-type or dangling reference is an inline error,
not a free-form string that fails later.

**Target (#671):** use **New** for a new permanent ID, **Duplicate** for a new entity with a new
ID, **Move draft file** to reorganize source without changing the ID, and
**Deprecate / supersede** for a released entity. Delete is available only for an unpublished,
unallocated draft. Codex previews compatibility and `idmap.lock` consequences before saving.

**Available CLI evidence:** the underlying index, picker, and backlink operations already exist:

```bash
build/mcc/mcc index content --json
build/mcc/mcc pickable ability content --json
build/mcc/mcc pickable item content --json
build/mcc/mcc refs chibi:class.warrior content --json
```

These commands are temporary inspection aids. They do not satisfy the final UI acceptance for
#670 or #671. `mcc idmap verify` is currently a stub; use full validation plus a read-only link as
the current objective ID-map gate:

```bash
build/mcc/mcc link content --no-allocate-ids --report
```

## 3. Author races and appearances

Open **Races**, create or open a color race, and edit the race and its male/female appearance
catalogs as one workflow.

**Target (#672):** the race editor must support:

1. Permanent race ID, append-only `roster_id`, display name, description, and optional attribute
   modifiers.
2. A typed appearance-catalog reference.
3. Sex, skeleton, body model, body material, dyes, hair, face, skin, and up to two morphs.
4. Stable append-only preset IDs with duplicate and renumber diagnostics.
5. Resolved previews and missing-art navigation for body, hair, face, skin, and material assets.
6. A completeness summary covering both catalogs for a playable race.

For the Chibi pack, confirm all 24 race/class combinations remain allowed, the races are cosmetic,
and each race resolves through its appearance catalog to the shared Chibi body plus its color dye.
The final evidence must create or edit a race without raw YAML and show the saved race, both sexes,
typed asset links, and a clean full-workspace validation.

**Temporary fallback:** no Race rail entry or race/appearance schema-form registration exists.
Developers may inspect and carefully edit `content/chibi/races/*.race.yaml` and
`content/chibi/appearance/*.appearance.yaml`, then run the gates in §12. This is not #672 or #783
acceptance.

## 4. Author abilities

Open **Abilities**, choose a physical-ability or spell template, and build its effect recipe.

**Target (#674):** author target, school, range, cast/channel timing, cooldown, global-cooldown
behavior, mana/rage/energy cost, audiovisual references, and an ordered list of typed Tier-1
effects. Supported effects are damage, heal, aura, threat, damage/heal over time, buff, debuff,
shield, crowd control, resource grant/drain, movement, and summon. Switching effect kinds must
preserve valid shared values, confirm destructive changes, and never expose scripting.

For each Chibi class, show that all four referenced abilities resolve and meet the #776 tuning
contract. At minimum the data proof records total four-ability resource cost, damage/healing,
shield size, crowd-control duration, cooldowns, and effect kinds.

**Partial:** Codex currently exposes an internal schema-form preview for an existing ability file:

```bash
dotnet run --project tools/codex/Meridian.Codex/Meridian.Codex.csproj -- \
  --schema-form ability content/chibi/abilities/warrior_crushing_blow.ability.yaml
```

The normal **Abilities** rail page is a placeholder. The preview can edit supported schema shapes
and saves through the CST layer, but it is not the specialized recipe editor, typed-reference
experience, template/create flow, or relationship summary required by #674. Treat it as a
temporary fallback only.

## 5. Author classes

Open **Classes**, select Warrior, Mage, Rogue, or Priest, and assemble the playable kit from typed
relationships.

**Target (#673):** the class editor must support permanent ID, append-only `roster_id`, name,
description, ability list, usable armor and weapon equip types, role XOR hybrid roles, attribute
modifiers, optional race limits, and talent-tree reference. Pickers must reject wrong reference
types and armor/weapon category mismatches. A relationship summary shows races, abilities,
equipment, talents, trainers, and other consumers.

Chibi evidence must show:

- all six races can select all four classes;
- each class links exactly its intended four-ability kit;
- starter weapon and armor equip types are compatible with that class;
- role/hybrid exclusivity is clean; and
- the class completeness panel has no missing relationships.

**Temporary fallback:** there is no Class rail entry or class schema-form registration. Developers
may inspect/edit `content/chibi/classes/*.class.yaml`, validate the full graph, and use `mcc refs`
for relationship evidence. That is not #673 or #783 acceptance.

## 6. Author items by purpose

Open **Items** and create each item from the correct purpose template. All records use
`meridian/item@2`; purpose is expressed by `item_class`, equipment fields, effects, price, and the
relationships that consume the item.

| Purpose | Required acceptance behavior |
|---|---|
| Weapon | `item_class: weapon`, compatible weapon `equip_type`, hand slot, damage/speed, level, zero-cost vendor availability, icon, and worn attachment model. |
| Armor | `item_class: armor`, compatible armor `equip_type`, armor/stat budget, body slot, level, zero-cost vendor availability, icon, and valid worn/skinned or shield attachment model. |
| Consumable | `item_class: consumable`, stack behavior, an `on_use` ability, icon, and quest reward or loot source. |
| Quest item | `item_class: quest`, no equipment slot, correct stack behavior, icon, and quest/loot relationship. It must not be accidentally sellable or wearable. |
| Reward/trinket | Equippable `item_class: armor` with `slot: trinket`, level/stat budget, icon, and the final quest reward backlink. |
| Other | `item_class: trade_good` or `container` as appropriate; only fields valid for that purpose are shown. |

**Available:** the normal Item editor can create, open, edit, validate, and save `item@2`, including
visual and worn-model data. It does not yet provide the pack-wide typed pickers/backlinks from
#670 or generic coverage/completeness guarantee from #677.

For the starter outfitter, each class-compatible weapon and armor item must have a buy price of
zero or an explicit zero `price_override` in Tink's inventory, as permitted by the schema and
runtime. The item editor must not confuse a zero price with a missing required price.

Capture the saved weapon, armor, consumable, quest item, and Meadow Medal, plus backlinks from the
vendor, loot table, quest objective/reward, class equip types, and audiovisual assets.

## 7. Author NPCs and mobs as distinct modes

NPCs and mobs share the single runtime schema `meridian/npc@1`; they are not duplicate records or
incompatible entity types. **NPC mode** and **Mob mode** are different authoring and readiness
views over that same entity.

### NPC mode: Pippa and Tink

**Target (#676):** choose **New NPC**. The form prioritizes friendly/passive disposition, identity,
dialogue, quest giver/turn-in relationships, vendor/trainer services, interaction prompts, and
backlinks. Combat fields remain available under advanced details but do not dominate readiness.

- Pippa is interaction-ready when her dialogue and all giver/turn-in backlinks resolve and she has
  a spawn backlink. She does not need hostile AI or loot.
- Tink is interaction-ready when dialogue, the typed starter-vendor reference, and a spawn backlink
  resolve. She does not need combat completeness.

### Mob mode: Dewdrop Slime and Sproutcap Scamp

**Target (#676):** choose **New Mob**. The form prioritizes level/rank, health, damage, armor,
attack speed, hostility, AI, aggro/leash/flee/help behavior, movement, typed ability rotation,
loot/money, model/sound, encounter role, and spawn readiness.

- Each mob is combat-ready only when hostile behavior, required combat stats, movement/leash,
  expected ability rotation, loot, model, and spawn backlinks resolve.
- Dialogue, vendor, trainer, or quest-giver services are not required for Mob readiness.

Switching modes must preserve every field and must never rewrite runtime classification data.
Navigation provides separate NPC and Mob filters, create flows, help, saved views, and readiness
summaries. The missing-art audit includes both.

**Partial:** the existing **NPCs** editor can create/open/edit/save the shared NPC schema. It has no
separate NPC/Mob flows, filters, readiness rules, typed reference controls, or backlinks. Use it as
a temporary fallback for basic fields only; it does not satisfy #676 or #783.

## 8. Link vendors, loot, and quests

### Free starter vendor

**Target (#684):** in Tink's NPC view, follow the vendor reference or choose **Create linked
vendor**. Add every class starter weapon and armor through typed item pickers. Show default versus
override price, unlimited stock, buy categories, duplicate detection, item backlinks, and Tink's
NPC backlink. Every starter item must cost zero copper for this demo.

**Temporary fallback:** vendor records have no normal editor. #677 must first provide complete
generic editing; raw YAML under `content/chibi/vendors/` is temporary and cannot satisfy #684.

### Mob loot

**Target (#683):** create the slime and scamp loot tables with typed item and quest references.
Show independent chances, exclusive groups where used, quantity ranges, quest conditions, nested
table depth, expected value, and a reproducible seeded 1,000-kill simulation. Navigate from each
mob to its loot and back.

**Temporary fallback:** loot records have no normal editor. #677 supplies generic coverage; the
specialized probability/simulation proof remains blocked on #683.

### Three-quest chain

**Target (#682):** create the three quests in graph order:

1. **A Pack for the Path** — Pippa gives Meadow Token; deliver it to Tink. Acquiring the
   class-compatible free starter weapon and armor is the next E2E action, not a modeled quest
   objective in the current quest schema.
2. **A Little Less Goo** — defeat three Dewdrop Slimes and collect three quest-gated drops; reward
   a healing consumable.
3. **Caps Off!** — defeat four Sproutcap Scamps and recover Pippa's ribbon; reward the equippable
   Meadow Medal.

Use typed giver, turn-in, NPC target, item, zone, prerequisite, and reward references. Graph and
form views edit the same CST-backed documents. Run **Walk chain** for each level-1 race/class
combination and prove no cycle, unreachable node, impossible gate, or dead end. Quest giver and
turn-in backlinks must appear on Pippa and Tink.

**Blocked in normal Codex:** Quests is currently a placeholder rail page. #677 provides generic
coverage; #682 provides the graph and simulation. Raw quest YAML is a temporary development
fallback and is forbidden in the #783 authoring proof.

## 9. Place the experience in Forge

Codex owns the spawn table's **what**; Forge owns concrete **where** placement. Do not conflate a
weighted spawn-table editor (#685) with positioned instances in the world.

**Target (#26, executed for Chibi by #779):** open the dedicated Forge Godot project, load Sprout
Meadow, and place Pippa, Tink, slime instances, and scamp instances through typed NPC/mob IDs.
Use viewport gizmos to set position, facing, respawn range, wander radius or patrol, aggro and leash
context. Export deterministic `meridian/spawn@1` source and validate it with the zone.

Acceptance requires:

- Pippa and Tink near the entry but separated enough to teach movement and interaction;
- a safe first slime area and a distinct second scamp area;
- enough live mobs for the approved quest counts without waiting;
- paths and terrain that make every NPC/mob reachable;
- no invalid placement, excessive overlap, involuntary multi-pull, or leash outside navigable
  space; and
- identical export from identical Forge input.

**Current reality:** Forge is only the M0 skeleton: one dock, one
`ForgeZoneMarker`, one 128 m bounds gizmo, and a `forge_core` bridge check. It cannot author spawn
placements. Opening it today is useful only to verify the skeleton:

```bash
git submodule update --init --recursive client/godot-cpp
cmake -S client/gdextension/forge_core \
  -B client/gdextension/forge_core/build \
  -DGODOTCPP_TARGET=editor
cmake --build client/gdextension/forge_core/build -j
```

Then open `client/forge/project` in the pinned Godot 4.7 editor. The current dock displays
**Meridian Forge** and **Zone dock — M0 skeleton (#134)**. A raw
`content/chibi/spawns/*.spawn.yaml` record is the temporary fallback for parallel data work, not
Forge or #779 acceptance.

## 10. Audit and generate missing art

All art is referenced by stable asset ID, never by source path. The asset sidecar owns the source,
class, license, provenance, budget, import hints, and restyle state.

### Target Codex flow

1. **Audit (#678):** open **Assets -> Missing art**. Filter Chibi findings by entity, field,
   expected asset class, and readiness. Resolve wrong classes, dangling IDs, missing sidecars or
   source files, budget failures, and pending restyles. Icons, textures, VFX, and SFX are not
   falsely offered to a 3D generator.
2. **Generate (#680):** from an eligible `character_model`, `creature_model`, `weapon_model`,
   `armor_model`, `kit_piece`, or `prop` field, choose **Generate with Meshy**. Review the exact
   original prompt, budget, pinned model release, credits, and current commercial terms. Prompt
   hygiene must reject franchise names and artist imitation.
3. **Quarantine:** the candidate lands with `.glb`, `meridian/asset@1` sidecar, exact prompts file,
   task IDs/origin, AI provenance, budget declaration, and `restyle_status: pending`. Codex must not
   wire or package it as final.
4. **Restyle:** hand the candidate to the human art workflow. Record meaningful transformation,
   complete review/provenance, pass import and asset budgets, and set `restyle_status: done` only
   after the human gate.
5. **Wire (#778 for the Chibi demo):** return to the original missing-art finding, select the
   accepted final Chibi asset ID, inspect its backlinks, and rerun the pack audit. #778 owns wiring
   finalized, provenance-complete Chibi artwork into the demo records; it does not bypass #678 or
   #680. Never replace an ID reference with a file path.

### Available temporary CLI intake

Codex does not yet expose the audit/generation UI. The policy-compliant CLI can create a
quarantined candidate for the parallel art agent:

```bash
export MESHY_API_KEY=...  # operator environment only; never commit or pass as an argument
PYTHONPATH=tools uv run python -m meshy generate \
  --text "an original cheerful rounded meadow slime creature" \
  --ns chibi --name dewdrop_slime --class creature_model \
  --terms-verified --model-version meshy-5 --json-events
```

The operator must review and replace the example prompt; it is not pre-approval to spend credits.
Never run the live endpoint in automated tests. Cancellation or failure evidence must retain known
remote task IDs so the job can be reconciled without duplicate submission.

`restyle_status: pending` is deliberately rejected by the hard asset gate. That failure proves
quarantine is working. Raw Meshy output must never be changed to `done` merely to make validation
green.

## 11. Diagnostics and relationship review

**Target (#670, #681):** Codex displays single-file diagnostics while editing and automatically
runs full-workspace validation before Build or Package. Clicking a diagnostic selects the exact
field. Deferred single-file reference notes disappear only after full linking proves the target.
The relationship inspector must verify at least:

- race -> appearance -> body/material/dye assets;
- class -> abilities, equip types, attributes, talent tree, and allowed races;
- Tink -> vendor -> every starter item -> class-compatible equip type;
- Pippa/Tink -> quests as giver/turn-in and -> Forge spawn placement;
- mobs -> rotations, loot, quest targets, and spawn placement;
- loot -> quest-gated collection items;
- quests -> prerequisites, NPCs, items, zone, and rewards; and
- every art reference -> sidecar -> source/provenance, plus backlinks to consumers.

**Available CLI parity checks:** after building `mcc`, run:

```bash
build/mcc/mcc check --file content/chibi/classes/warrior.class.yaml content \
  --diag-format=json
build/mcc/mcc check content --diag-format=json
uv run tools/validate_content.py --assets error
build/mcc/mcc fmt --check content/chibi
build/mcc/mcc link content --no-allocate-ids --report
```

Single-file mode reports cross-file checks as informational/deferred. Only the full `check`, link,
and reference validator establish graph integrity. Codex, `mcc`, and `validate_content.py` must
agree on both the valid workspace and a crafted-invalid test fixture; never damage the real Chibi
pack to demonstrate an error.

On this guide's verification baseline, the Warrior single-file check exits clean with deferred
`L002`/`L010` notes and one informational `L011` per reference; the full `mcc check`, read-only
link, and `validate_content.py --assets error` are clean. `mcc fmt --check content/chibi` currently
reports canonical-format drift in existing Chibi pack, zone, item, and asset/provenance files.
That failure is a known current-tree condition, not the expected final result: #783 must finish
with formatter exit `0` without using `mcc fmt` to rewrite unrelated human formatting blindly.

## 12. Diff, deterministic build, database load, and runtime

### Diff and build target

**Target (#681):** Codex invokes `mcc` and shows structured diagnostics, additive/breaking diff,
content hash, elapsed time, and artifact locations. Codex does not write SQL, client packs, or
`.mcpack` files itself. Pending/missing provenance blocks packaging with navigation to the asset.

From a clean copy of the pre-change pack, the CLI equivalents are:

```bash
build/mcc/mcc diff /tmp/chibi-before content/chibi --json
build/mcc/mcc build content --no-allocate-ids --diag-format=json
build/mcc/mcc content-hash content --json
build/mcc/mcc emit-sql content --pack chibi --out build/chibi-world.sql
build/mcc/mcc emit-pck content --out build/chibi-pck
```

`mcc pack`, `install`, `uninstall`, `migrate`, and `idmap verify` currently print stubs. Do not use
their zero exit status as packaging, installation, migration, or ID-map evidence. #681 must expose
only genuinely implemented operations and label anything else blocked.

Run the build twice from identical source with the fixed default timestamp. Compare every Chibi
artifact byte-for-byte and confirm the SQL `world_manifest` hash equals the Chibi client manifest
hash. A wall-clock timestamp is not allowed in determinism evidence.

For database evidence, always emit one explicit selected pack. #790 tracks five legacy integration
tests that omit `--pack`, produce multi-pack SQL, and fail when separate themes reuse valid
per-pack roster IDs. A focused `emit-sql content --pack chibi` result remains valid. Until #790
lands, a failure in those five full-DB tests is a known harness regression, not permission to omit
the selected-pack proof or to weaken roster isolation.

### Database and playable target

**Target (#786, consumed by #783):** the supported local workflow must select `chibi`, emit/load its
single-pack SQL before `worldd` verifies the manifest, and mount or expose the matching client pack.
#786 owns the local selected-pack load mechanism and its objective boot/hash evidence. #783 consumes
that mechanism for the player proof; it does not own repairing the loader itself. The final E2E is:

1. Create any of six races with any of four classes.
2. Enter Sprout Meadow in the base cloth outfit.
3. Meet Pippa and Tink; deliver the token.
4. Buy and equip compatible starter weapon and armor for zero copper; prove wrong-class gear is
   rejected.
5. Use all four class abilities against both mob types.
6. Complete the three quests, including kill, loot, collect, remote turn-in, consumable use, and
   equipping the Meadow Medal.
7. Prove mob population, aggro/leash, death/loot, and respawn support the quest counts.

The standard runtime commands are:

```bash
scripts/dev/build.sh
scripts/dev/run-local.sh
scripts/dev/build-client.sh
scripts/dev/run-client.sh
```

**Current blocker (#786):** `scripts/dev/run-local.sh` creates the world schema but does not load
`build/chibi-world.sql`. It is not currently a selected-pack Chibi runtime. #786 must make pack
selection and data loading explicit and verify the running `worldd` booted the same content hash
the client mounted. #783 may proceed only by consuming that completed path.

**Current combat blockers:** schema/build success is not playable balance evidence. #784 owns
executing creature basic attacks from NPC damage/attack speed and applying NPC armor. #785 owns
deriving player combat stats from equipped items and applying authored ability coefficients.
Until both land, survival, gear-impact, and coefficient-based tuning claims are blocked. Record raw
data checks separately; never report them as runtime results.

## 13. Acceptance matrix

| Workflow | Owner | Current path | Completed-tool objective evidence |
|---|---|---|---|
| Open/create Chibi pack | #667 (closed) | Normal Pack page; available | Native folder selection, manifest edit/save, reopen, no conflict or lost formatting |
| Workspace diagnostics, typed refs, backlinks | #670 | CLI `index`/`pickable`/`refs`; UI blocked | Typed picker rejects wrong type; inline full-graph diagnostics; find-usages navigates every Chibi consumer |
| Entity lifecycle and permanent IDs | #671 | Manual discipline + read-only link | Released rename/delete refused; draft delete distinct; duplicate gets new ID; breaking impact matches diff |
| Race + appearance | #672 | Raw YAML fallback | Create/edit race and both catalogs without YAML; preset stability, preview, completeness, missing art |
| Class | #673 | Raw YAML fallback | Assemble all four class kits without YAML; typed refs, role XOR hybrid, equip compatibility, backlinks |
| Spell/ability | #674 tooling; #775 Sunny Sip | Experimental existing-file schema form | Recipe templates and every Tier-1 effect; author the approved consumable ability with safe oneOf switching, tooltip, and typed audiovisual refs |
| Item variants | #677 tooling/current Item editor; #775 demo items | Item editor available; typed relations partial | Weapon, armor, consumable, quest item, reward, and other templates save the approved `item@2` dataset without YAML |
| NPC vs Mob | #676 tooling; #781 Pippa/Tink; #775 demo mobs | Shared NPC alpha only | Separate create/filter/layout/readiness modes over one entity; author the approved friendly NPC and hostile mob records without YAML |
| Vendor | #684 tooling; #781 Tink inventory | Raw YAML fallback after generic coverage | Tink inventory, zero price, stock validation, item/NPC backlinks, no YAML |
| Loot | #683 tooling; #775 demo loot tables | Raw YAML fallback after generic coverage | Author the approved typed tables; probability preview, deterministic seeded simulation, and mob/quest backlinks pass |
| Quest chain | #682 tooling; #782 Chibi chain | Quests rail placeholder; raw YAML fallback | Graph/form parity, approved three-quest data, typed edges, cycle/reachability checks, 24 race/class chain simulations |
| Spawn-table data | #685 | Raw YAML fallback | Typed weighted entries and stable Forge-facing IDs; constraints and backlinks |
| Concrete world placement | #26 / #779 | Forge M0 skeleton; raw spawn fallback | Viewport placement/export, reachable layout, deterministic spawn/zone validation |
| Missing-art inventory | #678 | Validator and manual search | Pack-wide categorized audit with navigation, class checks, provenance/budget/restyle status |
| Meshy handoff | #680; CLI protocol #679 closed | `python -m meshy`; no Codex UI | Prompt review/terms/credits, mocked UI tests, live operator run, safe resume, pending quarantine |
| Final Chibi art wiring | #778, consuming #678/#680 | Manual asset-ID wiring | Every demo art ref selects finalized provenance-complete/restyled Chibi art; audit and backlinks clean |
| Check/diff/build/package | #681 | CLI check/diff/build; package-related commands partly stubbed | Integrated truthful status, provenance gate, artifact paths, two-build/hash proof |
| Selected-pack local load | #786, consumed by #783 | `run-local.sh` loads DDL only | Explicit Chibi selection, single-pack SQL load before boot, and matching world/client manifest hash |
| Database regression coverage | #790 | Five legacy tests omit `--pack` | Every affected DB test emits an explicit pack and no longer fails on valid per-theme roster overlap |
| Runtime/player proof | #783, consuming #786 and dependencies #784/#785/#787 | Local load and combat gaps above | Matching selected-pack runtime plus complete level 1–3 human E2E |

Any failed row stays failed. A temporary fallback cannot be used to turn a Target row green.

## 14. Current gap register

These gaps were verified while authoring this guide. Keep them visible until their owning story
lands; if a new gap is found during #783, add it here and to the tracker.

| Verified gap on current `dev` | Owning story or dependency |
|---|---|
| No Race rail entry, create flow, or race/appearance schema-form registration | #672 |
| No Class rail entry, create flow, or class schema-form registration | #673 |
| Abilities rail is a placeholder; only an internal existing-file schema preview is available | #674 |
| NPC editor does not distinguish NPC and Mob create flows, layouts, filters, or readiness | #676 |
| Quests rail is a placeholder; vendor, loot, spawn, and asset-sidecar forms are not registered in the normal shell | #677, then #682–#685 |
| Typed pickers/backlinks exist in `mcc` but are not integrated into Codex workspace forms | #670 |
| Released-ID lifecycle actions and compatibility previews are absent | #671 |
| Missing-art audit and Meshy handoff are CLI/manual rather than a Codex workflow | #678, #680 |
| Forge is an M0 marker/gizmo/bridge skeleton and cannot place or export NPC/mob spawns | #26 / #779 |
| Integrated Build/Check/Diff/Package UI is absent; `mcc pack`, install/uninstall/migrate, and `idmap verify` are stubs | #681 and later packaging work |
| Existing Chibi pack/zone/items and several asset/provenance files fail `mcc fmt --check` | #774 content stories must clear drift before #783 |
| Chibi manifest pins Godot 4.6 while the client and Forge projects declare 4.7 | #787 |
| `run-local.sh` creates the world schema but does not select/load the Chibi single-pack SQL before `worldd` boot | #786; #783 consumes the completed path |
| Five legacy DB integration tests emit multi-pack SQL without explicit selection and fail on valid per-theme roster overlap | #790; focused `--pack chibi` evidence remains valid |
| Runtime does not execute NPC basic attacks from authored damage/speed or apply NPC armor | #784 |
| Runtime does not derive player combat stats from equipment or apply ability coefficients | #785 |

## 15. Evidence template

Copy this block into the #783 run record and attach logs/screenshots rather than replacing objective
evidence with a written claim.

```text
Chibi creator-tools acceptance run
Date / operator:
OS / architecture:
Repository commit:
Codex version/commit:
Forge/Godot version:
mcc --version:
Pack path and namespace:
Pre-run git status:

Authoring (no raw YAML)
[ ] Race + male/female appearance
[ ] Four-ability class relationship
[ ] Weapon, armor, consumable, quest item, reward
[ ] Pippa and Tink in NPC mode
[ ] Dewdrop Slime and Sproutcap Scamp in Mob mode
[ ] Free starter vendor
[ ] Mob loot + seeded simulation
[ ] Three-quest graph + 24 race/class walk simulations
[ ] Forge spawn placement and deterministic export

Safety and art
[ ] Typed picker negative case and diagnostic
[ ] Backlinks/find-usages for one entity of every type
[ ] Released-ID rename/delete refusal
[ ] Missing-art audit clean or explicitly quarantined
[ ] Meshy prompt/terms/provenance evidence (if generated)
[ ] Human restyle/import/budget evidence; no pending asset packaged

Build
validate_content command / exit / summary:
mcc check command / exit / summary:
crafted-invalid parity fixture / expected diagnostic:
mcc diff classification:
build 1 artifact hashes:
build 2 artifact hashes:
world_manifest == client manifest hash:
selected-pack local load command/evidence (#786):
database row and manifest checks:

Runtime
[ ] 6 races x 4 classes selectable
[ ] Base cloth entry state
[ ] Free compatible gear purchase/equip
[ ] Wrong-class gear rejection
[ ] Four abilities used against both mobs
[ ] Quest 1 complete
[ ] Quest 2 complete; consumable used
[ ] Quest 3 complete; Meadow Medal equipped
[ ] Mob attack/armor/gear/coefficient behavior proven
[ ] Population, aggro, leash, loot, and respawn proven

Automated suite:
Independent QA:
Human UI/client E2E:
Known blockers (must be empty to pass):
Final verdict: PASS / FAIL
```

## 16. Definition of done

The guide passes only when a clean-worktree operator can follow it without raw YAML, direct
database authoring, untracked IDs, unreviewed AI art, or undocumented repair steps. Codex owns all
non-spatial content; Forge owns concrete spatial placement; `mcc` is the only runtime artifact
producer. The same Chibi graph must validate in Codex, `mcc`, and `validate_content.py`, build
deterministically, load as a single pack, and produce the approved playable experience.

If reality differs from this document, record the gap against the owning story and update the
guide. Do not hide the difference with a manual workaround.
