<!-- SPDX-License-Identifier: Apache-2.0 -->
# Design — Chibi theme pack + realm selection (first playable in dev)

- **Status:** DRAFT (2026-07-14)
- **Parent:** [Moddable Theme Platform](2026-07-13-moddable-theme-platform-design.md) — this is the
  Group-A **theming/pack seam** (sub-project 4) folded together with the **Chibi theme** (Group-B,
  sub-project 5) to the first end-to-end playable milestone.
- **Goal:** make a chibi character — **6 color races × male/female + the 4 chibi classes** over the
  shared `chibi_pill_body` base — **playable in the dev realm**: char-create shows the chibi roster,
  the assembled colored character previews correctly (no `catalog:` errors), and the player can
  create a character and **enter a chibi zone and walk**.

## 1. Motivation (what the dev E2E showed)

Connecting the client to dev today shows the **old realistic roster** (Ardent/Dolmen races, old
paperdoll art) and throws `AssembledCharacter: assembly failure — catalog:3|0` / `catalog:4|0`. Root
causes, confirmed in the tree:

- Everything lives in **one namespace, `core`**. The chibi work merged so far — the 4 classes (#732)
  and `chibi_pill_body` (#746) — landed **in `core`**, but nothing references the body (no appearance,
  no race), so it never renders.
- `server/worldd/world_boot.cpp` **hardcodes `kPrimaryNamespace = "core"`** — there is no seam to
  point a realm at a different theme pack.
- `server/characters/src/roster.h` declares four compiled races — Ardent(1), Dolmen(2),
  **Sylvane(3), Emberkin(4)** — but only Ardent/Dolmen have appearance catalogs; char-create previews
  every roster race, so 3 and 4 fail. A **pre-existing gap**, not a chibi regression.

Two operator decisions set the direction: **chibi lands in its own `chibi` namespace** (not `core`),
and **chibi replaces `core` as the realm's theme** (realistic content retired).

## 2. Approach (chosen)

**Self-contained chibi pack + configurable realm theme.** `content/chibi/` is a complete pack with
its own `idmap.lock` + IF-9 id-band carrying 100% of its content; the kernel is unchanged except for
one seam (§4). This is the literal "pack = theme" architecture and the only option that honors both
decisions (own namespace **and** replaces core). Rejected alternatives: a *minimal* chibi pack that
shares core's kernel catalogs (violates "the pack holds 100% of content"; forces core to stay loaded);
and renaming core→chibi in place (cosmetic rename, loses core as legacy, no real theme boundary).

## 3. End-to-end architecture

```
content/chibi/  ──mcc build──▶  world DB (world_manifest row: namespace=chibi, id-band, content_hash)
                                        │
        worldd boots with realm theme = "chibi"  ──▶ resolves chibi as the primary pack
                                        │
        client mounts chibi pack (content_hash handshake) ──▶ char-create + AssembledCharacter
                                        │                        read chibi's roster + appearance catalogs
        create chibi character ──▶ enter world ──▶ spawn in chibi's minimal zone
```

`core` stays in git as retired legacy; no active realm loads it.

## 4. Realm theme-selection seam (the one kernel change)

Replace the hardcoded `kPrimaryNamespace = "core"` with a **realm config value** — env
`MERIDIAN_REALM_THEME` (default `"core"`; the dev realm sets `chibi`). The primary-pack resolver in
`world_boot.cpp` picks the `world_manifest` row whose `pack_namespace == theme` (fallback to the
existing "first row" behaviour if absent). Two correctness points, verified during implementation:

- **Roster source.** The chibi realm's race/class list must come from the **pack roster** (chibi's 6
  races), not `roster.h`'s compiled fallback — that fallback is what threw `catalog:3|0`/`4|0`. The
  SP2 "roster-from-pack" path is authoritative when the pack ships a roster; `roster.h` remains the
  fallback only for a pack that omits one. (This removes the `catalog:3/4` errors for the chibi realm
  by construction — chibi ships exactly 6 races, each with a catalog.)
- **No `"core"` literal in the client.** The client resolves content by the mounted pack's ids/hash;
  confirm char-create's race iteration is pack-driven, not compiled.

## 5. Chibi pack contents (`content/chibi/`)

- **`pack.yaml`** — `namespace: chibi`, `version`, `compatibility_version`, `content_schema_version`,
  theme metadata (`theme.display_name: "Chibi"`).
- **Rules-data** (each pack re-declares these — the "100% of content" contract): `attributes`
  (kernel-blessed seed: str/agi/int/sta + crit/haste/armor), `equip_types` (armor Cloth/Leather/Mail/
  Plate; weapon 2H/1H/Wand/Staff), the **16 abilities**, the **4 classes** (Warrior/Mage/Rogue/Priest),
  `talents` + `talent_trees`, `items` (per-class starters **+ the base cloth-wrap outfit** — the
  "wearing nothing" item baked into the base look).
- **Character/appearance:** **6 races** (Red/Green/Blue/Yellow/Gold/Silver — roster_ids 1–6 in the
  *chibi* roster), the shared `chibi_pill_body` body + **6 baked color materials**, and **12 appearance
  catalogs** (6 races × male/female) each referencing the shared body + its color material; presets
  (hair/face/skin) empty for now.
- **World (minimal, to satisfy enter-world):** one small `zone` (terrain + a spawn point). Encounters/
  NPCs kept to the minimum needed to spawn and walk.

## 6. Color-race model (per-race baked materials)

The shared grey `chibi_pill_body` mesh (grey ≈ RGB 128 by design, for recoloring) + **6 baked color
materials**: Red/Green/Blue/Yellow as flat recolors of the base texture; **Gold/Silver as metallic**
materials (metalness/roughness tuned). Each is an art asset in the chibi pack (produced by recoloring
the base texture / authoring the material — not necessarily a new Meshy generation).

**Wiring:** each race's appearance catalog points at its baked material, and the client
`AssembledCharacter` applies it to the body geosets (it already applies a body skin/material). The
plumbing choice — settled in the plan — is whether the material attaches via the existing `skin`-preset
slot (the race fixes skin preset 1 = its color) **or** a small dedicated `body_material` field added to
`appearance.schema.yaml`. Either is a thin, additive schema touch, not a new subsystem.

## 7. Migration (core→chibi) + old-roster retirement

- Re-author the already-merged chibi content (`#732` classes, `#746` body) **in the `chibi`
  namespace**: copy the YAMLs to `content/chibi/`, rewrite `core:` → `chibi:` id prefixes, let `mcc`
  allocate fresh ids in chibi's id-band (chibi `idmap.lock` starts clean; append-only applies within
  chibi from there). **Delete the `core` copies** so the content never lives in two places (no active
  realm loads `core`, so no careful retirement is needed).
- **Old roster:** the chibi realm uses the chibi pack roster (6 races, 4 classes); `roster.h`'s
  compiled `Ardent/Dolmen/Sylvane/Emberkin` is simply not consulted, so `catalog:3/4` disappears.
  `roster.h` stays untouched as the no-roster-pack fallback. Fresh realm, no existing chibi characters
  to preserve.

## 8. Client wiring (char-create + enter-world)

Char-create reads the chibi pack roster → shows **6 color races × male/female + 4 classes**; the race
picker *is* the color choice; the sex picker shows male/female over the shared body (differentiation via
hair/makeup is a later theme addition). The assembled preview applies the race's baked material → the
colored chibi previews (no capsule fallback, no `catalog:` errors). Create → persist (race 1–6, class
1–4) → **enter world** → spawn in the chibi zone rendering the colored body + base cloth outfit +
equipped starter gear. The assembler is already pack-driven; the main client work is verifying
char-create's pickers are data-driven and generalizing any lingering core/ardent assumptions.

## 9. Story decomposition & build order

**Epic: Chibi theme pack + realm selection (first playable in dev).** Nine stories, five waves.

| Story | Deliverable | Depends |
|-------|-------------|---------|
| **C1** | **Realm theme-selection seam** — `worldd` primary namespace = config (`MERIDIAN_REALM_THEME`, default `core`); resolver picks the theme's manifest row; roster-from-pack authoritative when a pack ships a roster. | — |
| **C2** | **Chibi pack scaffold** — `content/chibi/` + `pack.yaml` (namespace `chibi`, theme meta) + fresh `idmap.lock`; `mcc` builds it. | — |
| **C3** | **Chibi rules-data** — attributes, equip-types, 16 abilities, 4 classes (migrate #732), talents/trees, items (starters + base cloth outfit); delete `core` copies. | C2 |
| **C4** | **Chibi body + 6 color materials** — migrate `chibi_pill_body` (#746) into `chibi`; author 6 baked materials (4 flat + Gold/Silver metallic); delete `core` copy. | C2 |
| **C6** | **Minimal chibi zone** — one playable zone (terrain + spawn point) for enter-world. | C2 |
| **C5** | **6 races + 12 appearance catalogs + color wiring** — race records (roster 1–6), 12 catalogs (6×2 sex) → shared body + per-race material; the appearance-schema material touch; cross-ref validation. | C3, C4 |
| **C7** | **Char-create pack-driven for chibi** — 6 color races × male/female + 4 classes, assembled colored preview, no `catalog:` errors. *(human UI-E2E gate)* | C5 + built pack |
| **C8** | **Enter-world as chibi** — create → spawn/walk in the chibi zone as the colored character. *(human UI-E2E gate)* | C5, C6, C7 |
| **C9** | **Dev realm loads chibi** — content-build/CD emits the chibi pack to the dev realm's world DB; `worldd` theme=`chibi`; old core roster retired for that realm; **full in-dev E2E**. | C1, C7, C8 |

**Waves:** (1) C1, C2 · (2) C3, C4, C6 · (3) C5 · (4) C7, C8 · (5) C9.

## 10. Testing & verification

- `mcc` + `validate_content` + golden green on the chibi pack at every content story; `mcc-parity`
  maintained.
- **Kernel (C1):** a `worldd` boot integration test — `theme=chibi` resolves chibi primary,
  `theme=core` unchanged.
- **Client (C7/C8):** the headless assemble/char-select verify path **plus** a human in-client UI-E2E
  gate before merge (the pattern used for the character base).
- **Final (C9):** the full dev E2E — rebuild client, connect to dev, see 6 color races × sex + 4
  classes, create a colored chibi, enter world and walk — green, no `catalog:` warnings.

## 11. Non-goals (explicitly out)

- Per-sex body differences / hair / makeup / face customization (the sex axis exists but both sexes
  share the body for now).
- Rich chibi world content (multiple zones, quests, encounters, NPCs beyond the minimal spawn zone).
- Retiring `core` from the repository or migrating any live data (there is none; `core` stays as
  legacy in git).
- UI theme skin / audio folding into the pack (later theming work) beyond what char-create/enter-world
  need.
- The 33 parked Meshy art candidates' restyle (separate ongoing track).

## 12. Risks & open questions

- **R1 — roster-from-pack authority.** If the SP2 roster-from-pack path is not fully authoritative over
  `roster.h` when a pack ships a roster, the `catalog:3/4` errors could persist. C1 must verify and, if
  needed, make pack-roster precedence explicit. *(highest-risk item; front-loaded in C1.)*
- **R2 — appearance material plumbing.** Reusing the `skin`-preset slot vs. adding a `body_material`
  field: settle in the C5 plan; keep the schema change additive and mirrored in `mcc` + `validate_content`.
- **R3 — client core/ardent assumptions.** Any compiled `"core"`/ardent literal in char-create would
  break the theme swap; C7 audits for these.
- **Q1 — dev realm build/CD.** How the dev realm's world DB is built from the chibi pack (content-build
  target + `worldd` env) is settled in C9; MVP assumes the pack ships in the realm's build.
