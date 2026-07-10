# Character/Equipment/Appearance Data Contract (①) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Orchestration:** each Task below maps 1:1 to a GitHub story issue under the contract epic; subagents implement, the lead verifies and merges (CLAUDE.md / AGENTS.md rules apply).

**Spec:** [2026-07-09-character-equipment-data-contract-design.md](../specs/2026-07-09-character-equipment-data-contract-design.md)

**Goal:** Implement the data contract — skeleton/socket/geoset vocabulary, `item@2` `visual.worn`, appearance catalog + per-character record (schema, DB, wire), dye palette + reserved instance state, NPC reserve comment — with validation and tests. No engine/renderer code (that is spec ②).

**Architecture:** Pure data-layer work in four surfaces: (1) `schema/content/*.yaml` JSON-Schema envelopes merged by `tools/validate_content.py`, (2) new lints in that validator, (3) a chardb migration + `world.fbs` additive tables + `meridian-characters` CRUD plumbing, (4) `mcc` carry-through so `item@2` compiles into the pack artifacts.

**Tech Stack:** JSON Schema draft 2020-12 (YAML), Python 3.12 + jsonschema + pytest, MariaDB SQL migrations, FlatBuffers (`schema/net/world.fbs`), C++20 (`server/characters`, `tools/mcc`).

## Global Constraints

- Content schemas use `additionalProperties: false` and `$defs` merged from `schema/content/common.defs.yaml` by validators (existing pattern).
- Geoset regions (8, coarse): `head, hands, forearms, torso, waist, hips_legs, lower_legs, feet` — additive evolution only within `@1`.
- Attach sockets: `main_hand, off_hand, ranged, back, hip_l, hip_r, shield`. Dye channels (max 3): `primary, secondary, accent`.
- Invisible slots `neck, finger, trinket, bag` never carry `visual.worn`.
- Race ids/names are roster-owned (`server/characters/src/roster.h`, append-only, 1-based; race 1 = `ardent`). Content-side race names must mirror roster.h.
- Server treats the appearance record as opaque-but-bounded; it is never gameplay-authoritative (spec §5.2/§9). Absent appearance ⇒ server default `{v:1, hair:1, face:1, skin:1}`.
- Characters DB migrations are reversible and numbered; next free number is `0003`. World DB (`schema/sql/world/`) is untouched — worn/dye/catalog data is client/pck-side, not world-SQL (spec §8; server never reads visuals).
- FlatBuffers changes are additive-only (append fields; never renumber).
- Git: branch from `dev`, PR into `dev`; python tests `uv run pytest -q`; full suite must stay green.
- New lint ids: L080–L084 (block reserved for this contract; L001–L072 are taken).

---

### Task 1: `meridian/skeleton@1` vocabulary defs + validator merge

**Files:**
- Create: `schema/content/skeleton.defs.yaml`
- Modify: `tools/validate_content.py` (`load_schemas` / defs-merge path, around `tools/validate_content.py:173`)
- Modify: `schema/content/common.defs.yaml` (extend `contentId` + add `dyeRef`, `appearanceRef` — needed by Tasks 2–4)
- Test: `tests/test_validate_content.py`

**Interfaces:**
- Produces: schema `$defs` usable in every content schema: `geosetRegion` (enum of the 8 regions), `attachSocket` (enum of 7 sockets), `dyeChannel` (enum of 3), `raceName` (enum, mirrors roster.h names, `[ardent, veilborn, cindral, thornek]` — **verify against roster.h before hardcoding; the enum comment must cite roster.h**), plus `dyeRef` / `appearanceRef` string patterns in common.defs.
- Produces: `contentId` pattern accepts new type segments `dye` and `appearance`.
- Consumed by: Tasks 2, 3, 4 (`$ref: "#/$defs/geosetRegion"` etc.).

- [ ] **Step 1: Write failing tests** — extend `tests/test_validate_content.py` (existing fixture-pack pattern, see file header) with: (a) a fixture content file using an unknown geoset region fails schema validation once Task 2 wires it — for this task, test the defs merge directly:

```python
def test_skeleton_defs_merged_into_schemas():
    """skeleton.defs.yaml $defs are available to content schemas like common.defs."""
    from validate_content import load_schemas
    validators = load_schemas(SCHEMA_DIR)
    # any loaded validator's schema resolves the merged defs
    merged = validators["item"].schema["$defs"]
    assert set(merged["geosetRegion"]["enum"]) == {
        "head", "hands", "forearms", "torso", "waist",
        "hips_legs", "lower_legs", "feet"}
    assert "main_hand" in merged["attachSocket"]["enum"]
    assert merged["dyeChannel"]["enum"] == ["primary", "secondary", "accent"]

def test_contentid_accepts_dye_and_appearance_segments():
    import re
    from validate_content import load_schemas
    defs = load_schemas(SCHEMA_DIR)["item"].schema["$defs"]
    pat = re.compile(defs["contentId"]["pattern"])
    assert pat.match("core:dye.russet")
    assert pat.match("core:appearance.ardent.male")
```

- [ ] **Step 2: Run to verify failure** — `uv run pytest tests/test_validate_content.py -q -k skeleton_defs or contentid_accepts` → FAIL (no skeleton.defs.yaml; pattern rejects `dye`).
- [ ] **Step 3: Implement** — write `schema/content/skeleton.defs.yaml`:

```yaml
# meridian/skeleton@1 — character vocabulary (spec: 2026-07-09 data-contract design §3).
# Additive evolution ONLY: append entries; never rename/remove. A geoset region may
# gain `children:` when split; hide-masks naming the parent hide all children.
$schema: "https://json-schema.org/draft/2020-12/schema"
$id: "meridian://schema/content/skeleton.defs.yaml"
$defs:
  geosetRegion:
    description: Coarse ×8 hideable body regions (owner decision 2026-07-09).
    enum: [head, hands, forearms, torso, waist, hips_legs, lower_legs, feet]
  attachSocket:
    description: Bone-attachment mount points (weapons/shields/sheaths).
    enum: [main_hand, off_hand, ranged, back, hip_l, hip_r, shield]
  dyeChannel:
    description: Max 3 == RGB dye-mask texture channels (spec §6).
    enum: [primary, secondary, accent]
  raceName:
    description: MUST mirror server/characters/src/roster.h names (append-only).
    enum: [ardent, veilborn, cindral, thornek]   # verify vs roster.h in review
  boneName:
    description: >-
      Canonical humanoid bone names (≤120 incl. face, art-prd §2.1). POPULATED BY
      the rig work (spec ④) — empty enum is valid for @1-draft; the import
      validator treats empty as "no bone-name check yet".
    type: string
```

Then in `tools/validate_content.py`, merge these `$defs` exactly the way `common.defs.yaml` is merged in `load_schemas` (same mechanism, second defs file), and extend `common.defs.yaml`: `contentId`/type-segment alternation gains `|dye|appearance`; add:

```yaml
  dyeRef:
    type: string
    pattern: "^([a-z][a-z0-9_]{1,31}:)?dye\\.[a-z0-9_]+(\\.[a-z0-9_]+)*$"
  appearanceRef:
    type: string
    pattern: "^([a-z][a-z0-9_]{1,31}:)?appearance\\.[a-z0-9_]+(\\.[a-z0-9_]+)*$"
```

- [ ] **Step 4: Run full validator tests** — `uv run pytest tests/test_validate_content.py -q` → all PASS (existing tests must not regress; the defs merge is additive).
- [ ] **Step 5: Run repo validation over real content** — `uv run python tools/validate_content.py content/ --schemas schema/content` (use the invocation documented at the top of `tools/validate_content.py`) → exit 0, existing content unaffected.
- [ ] **Step 6: Commit** — `git commit -m "feat(schema): meridian/skeleton@1 vocabulary defs + dye/appearance refs (contract ①)"`

---

### Task 2: `meridian/item@2` — `visual.worn` block, lints, core-content migration

**Files:**
- Modify: `schema/content/item.schema.yaml` (envelope const → `meridian/item@2`; `visual` block)
- Modify: `tools/validate_content.py` (lints L080, L081)
- Modify: `content/core/items/rusty_pickaxe.item.yaml` (+ bump the other four items' `schema:` line to `@2` — no other change)
- Test: `tests/test_validate_content.py`

**Interfaces:**
- Consumes: Task 1 `$defs` (`geosetRegion`, `attachSocket`, `dyeChannel`, `raceName`, `artRef`).
- Produces: the `visual.worn` shape every downstream consumer (mcc Task 6, client spec ②) reads:
  `worn.models[] {model: artRef, mirror: none|x}`, `worn.hides[]: geosetRegion`, `worn.attach {socket, sheath_socket?}: attachSocket`, `worn.dye_channels[]: dyeChannel`, `worn.race_overrides.<raceName> {models[], hides[]?}`.

- [ ] **Step 1: Write failing tests** — fixture items in the existing test-pack style:

```python
WORN_OK_WEAPON = """\
schema: meridian/item@2
id: tp:item.sword
name: Sword
item_class: weapon
slot: main_hand
rarity: common
weapon: { damage: { min: 1, max: 2 }, speed_ms: 2000 }
visual:
  icon: art.ui.icon.sword
  worn:
    models: [{ model: art.weapon.tp.sword, mirror: none }]
    attach: { socket: main_hand, sheath_socket: hip_l }
"""

def test_item2_weapon_with_attach_passes(tmp_path): ...        # 0 errors
def test_item2_armor_visible_slot_without_worn_fails_L080(tmp_path): ...
def test_item2_ring_with_worn_fails_L080(tmp_path): ...        # invisible slot
def test_item2_armor_with_attach_fails_L081(tmp_path): ...     # attach XOR skinned
def test_item2_weapon_without_attach_fails_L081(tmp_path): ...
def test_item2_unknown_geoset_region_fails_schema(tmp_path): ... # enum via $ref
def test_item2_race_override_unknown_race_fails_schema(tmp_path): ...
```

(Write each fixture fully in the test file — copy `WORN_OK_WEAPON` and mutate one field per negative case, per the one-negative-fixture-per-lint convention at `tests/test_validate_content.py:1`.)

- [ ] **Step 2: Verify failure** — `uv run pytest tests/test_validate_content.py -q -k item2` → FAIL (schema still `@1`).
- [ ] **Step 3: Implement schema** — in `item.schema.yaml`: `schema: { const: "meridian/item@2" }`; extend `visual`:

```yaml
  visual:
    type: object
    additionalProperties: false
    required: [icon]
    properties:
      icon: { $ref: "#/$defs/artRef" }
      model: { $ref: "#/$defs/artRef" }
      worn:
        type: object
        additionalProperties: false
        required: [models]
        properties:
          models:
            type: array
            minItems: 1
            items:
              type: object
              additionalProperties: false
              required: [model]
              properties:
                model: { $ref: "#/$defs/artRef" }
                mirror: { enum: [none, x], default: none }
          hides:
            type: array
            uniqueItems: true
            items: { $ref: "#/$defs/geosetRegion" }
          attach:
            type: object
            additionalProperties: false
            required: [socket]
            properties:
              socket: { $ref: "#/$defs/attachSocket" }
              sheath_socket: { $ref: "#/$defs/attachSocket" }
          dye_channels:
            type: array
            uniqueItems: true
            maxItems: 3
            items: { $ref: "#/$defs/dyeChannel" }
          race_overrides:
            type: object
            additionalProperties: false
            propertyNames: { $ref: "#/$defs/raceName" }
            patternProperties:
              "^[a-z][a-z0-9_]*$":
                type: object
                additionalProperties: false
                required: [models]
                properties:
                  models: ...   # same shape as worn.models — repeat inline
                  hides:  ...   # same shape as worn.hides — repeat inline
```

- [ ] **Step 4: Implement lints** in `tools/validate_content.py` (register in the header comment block too):
  - **L080** — `visual.worn` REQUIRED when (`item_class == weapon`) or (`item_class == armor` and `slot` not in `{neck, finger, trinket, bag}`); FORBIDDEN when slot ∈ that set or `item_class` ∉ {weapon, armor}.
  - **L081** — `worn.attach` REQUIRED when `item_class == weapon`; FORBIDDEN when `item_class == armor`; `worn.hides` FORBIDDEN when `item_class == weapon` (attach XOR skinned, spec §4/§9). Add matching negative fixture `test_item2_weapon_with_hides_fails_L081`.
  Follow the existing `check_provenance`/`check_budget` function style: `def check_worn(doc, rel_path) -> list[str]` returning `"L080 {rel_path}: …"` strings, called from `validate()`.
- [ ] **Step 5: Migrate core content** — bump all 5 `content/core/items/*.item.yaml` to `schema: meridian/item@2`; add to `rusty_pickaxe.item.yaml` (the only weapon):

```yaml
visual:
  icon: art.ui.icon.weapon.rusty_pickaxe      # keep existing icon line as-is
  worn:
    models: [{ model: art.weapon.zone01.rusty_pickaxe, mirror: none }]
    attach: { socket: main_hand, sheath_socket: back }
```

⚠️ L020 requires `art.weapon.zone01.rusty_pickaxe` to resolve to an IF-8 sidecar. Check `content/core/assets/**` for the existing pickaxe sidecar and reuse its id; if none exists, add a minimal `weapon_model` sidecar following an existing sidecar file as template (class `weapon_model`, provenance per its siblings). Do NOT invent a second icon id — reuse whatever `visual.icon` the file already has.
- [ ] **Step 6: Verify** — `uv run pytest tests/test_validate_content.py -q` → PASS; `uv run python tools/validate_content.py content/ --schemas schema/content` → exit 0.
- [ ] **Step 7: Commit** — `git commit -m "feat(schema): item@2 visual.worn — modular gear contract + L080/L081 (contract ①)"`

---

### Task 3: `meridian/appearance_catalog@1` schema + lints

**Files:**
- Create: `schema/content/appearance.schema.yaml`
- Modify: `tools/validate_content.py` (file-type mapping for `.appearance.yaml` in `file_type()` at `tools/validate_content.py:188`; lints L082, L083)
- Test: `tests/test_validate_content.py`

**Interfaces:**
- Consumes: Task 1 (`raceName`, `artRef`, `contentId` with `appearance` segment).
- Produces: catalog shape consumed by spec ⑤ content and spec ② client: `presets.hair[]/face[]/skin[]` entries `{id: int 1–255, model|texture|palette: artRef}`, `morphs[]` `{id, name, blendshape, min, max}`, plus `race`, `sex: [male, female]`, `skeleton: artRef`, `body_model: artRef`.

- [ ] **Step 1: Failing tests** — valid catalog fixture (race `ardent`, 1 hair/1 face/1 skin, `morphs: []`); negatives: duplicate preset id within `hair` (L082), two catalog files with same (race, sex) (L083), preset id 0 (schema `minimum: 1`), unknown race (schema).
- [ ] **Step 2: Verify FAIL**, then **Step 3: Implement** the schema exactly as spec §5.1 (envelope `meridian/appearance_catalog@1`; `required: [schema, id, race, sex, skeleton, body_model, presets]`; preset entry ids `type: integer, minimum: 1, maximum: 255`; `morphs` maxItems 2 — A-03/D-32 cap; catalog `id` uses `appearanceRef` self-id convention like other envelopes use `contentId`).
- [ ] **Step 4: Implement lints** — **L082**: preset ids unique per preset list; **L083**: one catalog per (race, sex) across the content tree (track in `validate()` the way L010 duplicate-id tracking works).
- [ ] **Step 5: Verify** — targeted pytest, then full file, then real-content validation (all green; no real catalog content lands in this task — that is spec ⑤).
- [ ] **Step 6: Commit** — `git commit -m "feat(schema): appearance_catalog@1 + L082/L083 (contract ①, A-03/D-32)"`

---

### Task 4: `meridian/dye@1` schema + starter palette

**Files:**
- Create: `schema/content/dye.schema.yaml`
- Create: `content/core/dyes/russet.dye.yaml`, `content/core/dyes/slate.dye.yaml`, `content/core/dyes/bone.dye.yaml`
- Modify: `tools/validate_content.py` (file-type mapping `.dye.yaml`)
- Test: `tests/test_validate_content.py`

**Interfaces:**
- Consumes: Task 1 (`contentId` with `dye` segment).
- Produces: dye shape consumed by the item-instance dye state (Task 5 reserved column) and client ②: `{schema, id: contentId, name: displayName, color: "#rrggbb", rarity}`.

- [ ] **Step 1: Failing tests** — valid dye; negatives: bad hex (`color: red`), missing rarity.
- [ ] **Step 2–3: Implement** schema per spec §6:

```yaml
# meridian/dye@1 — curated dye palette (contract ① §6). Dyes are content-defined
# colors, NOT free RGB pickers — the painterly art direction stays coherent.
# Acquisition/economy (dye items, vendors) is ITM/ECO scope, deliberately absent.
$schema: "https://json-schema.org/draft/2020-12/schema"
$id: "meridian://schema/content/dye.schema.yaml"
type: object
additionalProperties: false
required: [schema, id, name, color, rarity]
properties:
  schema: { const: "meridian/dye@1" }
  id: { $ref: "#/$defs/contentId" }
  name: { $ref: "#/$defs/displayName" }
  color: { type: string, pattern: "^#[0-9a-f]{6}$" }
  rarity: { enum: [poor, common, uncommon, rare, epic, legendary] }
```

and the three starter dyes (`core:dye.russet` `#8a4b2d` common, `core:dye.slate` `#5a6672` common, `core:dye.bone` `#d9cfc0` common).
- [ ] **Step 4: Verify** — pytest + real-content validation green (dyes reference no assets, so L020 is inert).
- [ ] **Step 5: Commit** — `git commit -m "feat(content): dye@1 schema + starter palette (contract ①)"`

---

### Task 5: Appearance record — chardb migration 0003, wire tables, create/list plumbing

**Files:**
- Create: `server/db/characters/migrations/0003_appearance_dyes.up.sql` / `.down.sql`
- Modify: `schema/net/world.fbs` (`CharCreateRequest` at `schema/net/world.fbs:403`, `CharacterSummary` near `schema/net/world.fbs:208`; new `Appearance`/`MorphValue` tables)
- Modify: `server/characters/src/characters.h` / `characters.cpp` (`CreateRequest`, `CharacterSummary`, `create_character` at `server/characters/src/characters.h:146`, `list_characters`)
- Modify: worldd session char-create/char-list handlers (the call sites of `create_character`/`list_characters` — locate with `probe search "create_character" server/`) — encode/decode the new field only; no behavior change otherwise
- Test: `server/characters/test/characters_test.cpp`; the FlatBuffers golden message corpus (extend the existing corpus tests added by the golden-message-corpus work — find via `probe search "golden message corpus" tests/ server/`)

**Interfaces:**
- Consumes: nothing from Tasks 1–4 (server treats appearance as opaque-but-bounded; NO catalog dependency — spec §5.2/§9).
- Produces: wire tables (additive):

```fbs
/// Versioned per-character appearance record (contract ① §5.2 / A-03 D-32).
/// Opaque to gameplay; additive evolution only. Absent ⇒ server default.
table MorphValue { id:uint8; value:uint8; }
table Appearance {
  version:uint8 = 1;
  hair:uint8 = 1;        // preset ids from the race/sex catalog; 1-based
  face:uint8 = 1;
  skin:uint8 = 1;
  morphs:[MorphValue];   // empty at M1 (A-03: ≤2 morphs, budget-gated)
}
// appended fields (additive — never renumber):
table CharCreateRequest { name:string; race:uint8; char_class:uint8; appearance:Appearance; }
// CharacterSummary gains: appearance:Appearance;
```

- Produces: DDL —

```sql
-- 0003 UP: contract ① §5.2 (appearance record) + §6 (reserved dye state).
-- appearance: versioned JSON record {"v":1,"hair":1,"face":1,"skin":1,"morphs":[]}
--   — opaque to gameplay; additive keys need no future migration (A-03/D-32).
-- item_instance.dyes: {"<channel>":"<dye content id>"} — RESERVED; written when
--   dye acquisition lands (M2); NULL = authored default colors.
ALTER TABLE `character` ADD COLUMN appearance JSON NULL AFTER `class`;
ALTER TABLE item_instance ADD COLUMN dyes JSON NULL AFTER suffix_id;
```

(DOWN: the two matching `DROP COLUMN`s.)
- Produces: C++ — `struct AppearanceRecord { std::uint8_t version{1}, hair{1}, face{1}, skin{1}; };` with `to_json()/from_json()` (nlohmann or the repo's existing JSON helper — match whatever `character_quest.objective_counts` handling uses); `CreateRequest` gains `std::optional<AppearanceRecord> appearance;`; `CharacterSummary` gains `AppearanceRecord appearance;`.

- [ ] **Step 1: Failing DB/CRUD tests** in `server/characters/test/characters_test.cpp` (follow the existing create/list test pattern in that file): create with explicit appearance → list returns it; create WITHOUT appearance → list returns defaults `{1,1,1,1}`; version ≠ 1 → clamped to 1 (bounds rule; no new failure taxonomy — spec §9 validation is bounds-only, and preset id 0 clamps to 1).
- [ ] **Step 2: Verify FAIL** — build + run the characters test target per `server/characters/CMakeLists.txt` (ctest name discoverable via `ctest -N` in the build dir).
- [ ] **Step 3: Implement** — migration files; regenerate FlatBuffers per the repo's codegen path (`feat/proto-codegen` — follow `schema/net/README.md`); `AppearanceRecord` + CRUD plumbing (INSERT stores JSON; SELECT parses, absent/NULL ⇒ defaults); worldd session handlers pass the table through.
- [ ] **Step 4: Verify GREEN** — characters tests + full server test suite + golden message corpus additions (a `CharCreateRequest` with and without `appearance` round-trips; conformance stays green).
- [ ] **Step 5: Run it** — `scripts/dev/run-local.sh`, create a character through the real client-or-bot path (the char-select bot flow from `feat/bot-client-v0` / two-bot E2E is the existing harness), confirm the `character.appearance` column is populated: `SELECT name, appearance FROM \`character\`` against the `.dev-run` MariaDB.
- [ ] **Step 6: Commit** — `git commit -m "feat(server): versioned appearance record — chardb 0003, wire, CRUD (contract ① §5.2)"`

---

### Task 6: mcc `item@2` carry-through + golden corpus

**Files:**
- Modify: `tools/mcc/src/stages/validate.cpp` (schema-envelope acceptance: `meridian/item@1` → `@2` — find the envelope-const/type map; parse is generic YAML→doc so `worn` likely flows through untouched)
- Modify: `tools/mcc/golden/*` (regenerated: `pack.contents.jsonl`, `world.sql`, `index.json`, `pack.manifest.json`)
- Test: `tools/mcc/tests/` (existing golden/round-trip test harness) + `tools/mcc/libmccore/tests` if the envelope map lives there

**Interfaces:**
- Consumes: Task 2 (`item@2` files in `content/core`).
- Produces: `mcc` full pipeline over `content/core` exits 0 with `item@2`; `pack.contents.jsonl` carries the full `visual.worn` block byte-faithfully (client pck is the consumer); `world.sql` output is UNCHANGED except item envelope version (worn/dye data never reaches world SQL — spec §8: server never reads visuals; `emit_sql.cpp`'s item mapping keeps emitting only `visual_icon_id`/`visual_model_id`).

- [ ] **Step 1: Failing test** — add an `item@2` fixture with a `worn` block to the mcc test content set; assert (a) pipeline exit 0, (b) the emitted `pack.contents.jsonl` entry contains `worn.models[0].model`, (c) emitted SQL for the item is identical to an `@1`-shaped item apart from nothing at all (no new columns).
- [ ] **Step 2: Verify FAIL** — build mcc per `tools/mcc/CMakePresets.json`, run its ctest suite → the envelope-version check rejects `@2`.
- [ ] **Step 3: Implement** — bump the accepted item envelope const to `meridian/item@2` (single source if the map is shared with `check.cpp`; do NOT keep accepting `@1` — all core content migrated in Task 2, and the nightly world DB is rebuilt wholesale so no back-compat window is needed).
- [ ] **Step 4: Regenerate golden corpus** via the documented regeneration flow in `tools/mcc/README.md`; inspect the diff — only item envelope strings and the pickaxe's new `worn` block may change.
- [ ] **Step 5: Verify GREEN** — full mcc ctest suite + `uv run pytest -q` (repo-wide) green.
- [ ] **Step 6: Commit** — `git commit -m "feat(mcc): accept item@2, carry visual.worn into pack contents (contract ①)"`

---

## NPC reserve note (no task)

Spec §7's only M1 action is a comment. Fold into Task 2's commit: add to `schema/content/npc.schema.yaml` above `visual:`:

```yaml
  # RESERVED (contract ① §7): visual becomes a oneOf in @2 — monolithic `model`
  # OR assemble-like-a-player {appearance, worn_items}. Do not design against
  # "NPC visual is always one mesh".
```

## Task dependencies / story mapping

```
Task 1 (defs) ──► Task 2 (item@2) ──► Task 6 (mcc)
            ├──► Task 3 (catalog)
            └──► Task 4 (dye)
Task 5 (server record) — independent; parallel with 2–4, 6
```

Out of scope (owned by follow-on specs): client assembler & spawn-payload appearance broadcast (②), rig/bones/import-validator (④), real catalog + gear content (⑤), dye acquisition (ITM/ECO M2), client char-create UI sending a non-default appearance (② or the CHR-01 UI story — server defaults make this a no-op until then).
