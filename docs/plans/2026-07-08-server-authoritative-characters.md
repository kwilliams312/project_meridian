# Server-Authoritative Characters Implementation Plan

Created: 2026-07-08
Status: PENDING
Approved: Yes
Iterations: 1
Worktree: No
Type: Feature

## Summary

**Goal:** Make worldd the definitive source of truth for characters — a client enters the world only as a **real, server-persisted character it owns**; worldd never fabricates one.

**Architecture:** Split the world session into two phases (WoW-style): `HANDSHAKE_OK` = *authenticated, at character-select* (no spawn); then an explicit, validated `ENTER_WORLD(character_id)` spawns the player. worldd validates the character is owned by the session's grant-derived account via the existing `meridian-characters` CRUD (`WHERE id=? AND account_id=?`), replies with a typed `ENTER_WORLD_RESPONSE`, and only spawns on OK. The fabricated "Placeholder" is removed. worldd is wired to `meridian_characters`. The Godot client and the headless `meridian-bot` both drive `CharList → CharCreate-if-empty → ENTER_WORLD`. Characters therefore persist (#341) and unknown/unowned characters are rejected (the reported server-authority violation).

**Tech Stack:** C++20 (worldd, meridian-bot, characters CRUD), FlatBuffers (`schema/net/world.fbs`), Godot 4.7 GDScript (client), Helm/ArgoCD (worldd char-DB wiring), MariaDB (`meridian_characters`).

---

## Scope

### In Scope
- **Protocol:** `ENTER_WORLD_REQUEST`/`ENTER_WORLD_RESPONSE` opcodes + tables + a typed `EnterWorldStatus` in `schema/net/world.fbs`; golden conformance fixtures.
- **worldd:** two-phase session (char-select → entered); remove the placeholder fabrication; `ENTER_WORLD` handler with ownership validation + real-character spawn; gate movement/entity until entered.
- **Deploy:** chart knob wiring worldd → `meridian_characters` (`MERIDIAN_CHARDB_*`), applied to dev/ptr/prod overlays.
- **meridian-bot:** `CharList → CharCreate-if-empty → ENTER_WORLD` before moving (keeps the load harness working with real characters).
- **Godot client:** replace the `character_store.gd` in-memory stub with real `CharList/CharCreate/CharDelete` + the `ENTER_WORLD` flow over the net thread (#279). *Manual verification only (#283).*
- Tests at every server/bot layer; harness + ops docs updated.

### Out of Scope
- Multi-character rosters / character selection UI beyond one-per-account (#329 stays: 0 or 1 character).
- Appearance customization, races/classes beyond the M0-frozen roster.
- Retiring the dev **seed Job** (still creates the realm row; harmless — a follow-up when the admin portal / #339 lands).
- Any change to the auth/login (IF-1) or grant (IF-3) flow — only post-handshake world entry changes.

---

## Context for Implementer

> Written for someone who has never seen this codebase.

- **The world protocol** is `schema/net/world.fbs` (FlatBuffers). Opcodes are a `u16` enum; the 0x0xxx range is session/system + character management. Char CRUD (`CHAR_LIST/CREATE/DELETE_REQUEST/RESPONSE`, `0x0010`–`0x0015`) already exists (D-35/#286) and rides the authenticated session. Add the new `ENTER_WORLD` opcodes in the 0x0xxx range (next free: `0x0016`, `0x0017`).
- **worldd session/dispatch:** `server/worldd/world_dispatch.h` (the `Dispatcher` registers per-opcode handlers in its ctor; see `server/worldd/main.cpp:400`). The session lifecycle + grant consume + the current spawn live in `server/worldd/world_session.cpp` — `validate_and_consume_grant` (~227) and `load_placeholder_character` (~306, **the fabrication to remove**). The char CRUD handlers call `meridian::characters` (`server/characters/src/characters.h`: `list_characters`, `create_character`, `delete_character`, `CreateRequest{account_id,name,race,char_class}`, typed exceptions).
- **The account** for a session is grant-derived (`ConnCtx.account_id`), never client-supplied — the same rule the char CRUD already follows (world.fbs §char-mgmt comment). `ENTER_WORLD` validation MUST use this account_id, never a client field.
- **worldd DB config:** `server/worldd/main.cpp:~241` reads the **auth** DB from `MERIDIAN_DB_*` (→ `db.*`, grant validation) and an optional **characters** DB from `MERIDIAN_CHARDB_*` (→ `chardb.*`). The code path exists; it just needs the env set in the chart. There is also a separate world **content** DB (`MERIDIAN_WORLDDB_*`) — do not conflate.
- **Chart:** `deploy/helm/meridian/templates/_daemon.tpl` renders daemon env; `worldd.db.{enabled,name}` already sets `MERIDIAN_DB_NAME` + `MERIDIAN_DB_PASS` (see the grant-DB fix, #340). Mirror that for a new `worldd.chardb.{enabled,name}` → `MERIDIAN_CHARDB_*`. Realm overlays: `deploy/gitops/realms/{dev,ptr,prod}/values.yaml`.
- **meridian-bot:** `client/bot/bot_world_session.{h,cpp}` + `bot_core.cpp` drive the world session; today entry = `WorldHello → HandshakeOk` (the `on_entered_world` callback fires at handshake). ITs: `client/test/run_bot_client_it.sh`, `run_two_bot_it.sh`, `run_client_sees_bot_it.sh`.
- **Godot client:** `client/project/scenes/charselect/character_store.gd` is the in-memory stub (its header names #279 as the wiring task). The net thread (#97) is how the client speaks the world protocol; the char-select scene consumes the store.
- **Gotcha — coupling:** once worldd requires a real character to spawn, *every* entrant (bots + Godot client) must create/select first. Land the worldd change and the bot+client create-flow **together**; do not deploy the worldd reject to a realm before the bot/client are on the new flow (see Pre-Mortem).
- **Gotcha — Godot verification:** the M0 Godot client can't run headlessly here (MoltenVK/Apple-Silicon, #283) — Task 6's DoD is manual/on-device, not CI.
- **The harness pieces exist** (this session): `meridian-character` CLI + `deploy/gitops/loadtest/add-characters.job.yaml` + the 3-step `scripts/dev/loadtest.sh`. Bots currently enter via the placeholder; after this spec they enter as their pre-created characters.

## Runtime Environment

- **worldd (hosted realms):** `meridian-<realm>-worldd` in ns `meridian-<realm>` (dev/ptr/prod), ArgoCD-synced from the branch. Images built by `cd.yml` on push (per-branch `:dev`/`:ptr`/`:prod`). Restart: `cd.yml` auto-restart (dev/ptr) or `kubectl -n meridian-<realm> rollout restart deploy/meridian-<realm>-worldd`. Health: `kubectl -n meridian-<realm> get pods`; worldd logs show `grant_consumed` / entry events.
- **Local:** `scripts/dev/run-local.sh` brings up authd+worldd+MariaDB; `scripts/dev/build.sh` builds the server; the bot ITs run the full flow locally.
- **Do NOT deploy the worldd reject to a realm until the bot is on the new flow** — coordinate the push (Task 7 / spec-verify).

## Assumptions
- **[INVALIDATED 2026-07-08, Task 2 exploration]** The plan assumed #326 single-active-session admission could stay at `HANDSHAKE_OK`. Reality (`world_dispatch.cpp:588-625`): `active_sessions->admit()` registers a kick-closure capturing `ctx.egress`/`ctx.slot`/`ctx.entered`, which are only set by the AoI `enter()` (step 5). A char-select (not-yet-spawned) session has none. **Adaptation:** move single-active admission into the `ENTER_WORLD`-OK path alongside AoI `enter()` (semantically correct — #326 is a single *in-world* session). CCU/`sessions{active}` metrics still bump at `HANDSHAKE_OK`. Affects Tasks 2 + 3. **RESOLVED (user, 2026-07-08): single-active applies at ENTER_WORLD — a single *in-world* session; char-select does not hold the slot.**
- The `meridian-characters` CRUD (`create/list/delete_character`) is complete and correct (D-35/#286 landed) — supported by `server/characters/src/characters.h` + this session's `meridian-character` CLI exercising it against a live DB (30/30). Tasks 2, 3, 4 depend on this.
- worldd's `chardb.*` config path works when `MERIDIAN_CHARDB_*` is set — supported by `server/worldd/main.cpp:~241-244`. Task 4 depends on this.
- The `character.id` a client passes in `ENTER_WORLD` is the server-minted id from `CharListResponse`/`CharCreateResponse` — supported by `world.fbs` `CharListEntry.character_id` / `CharCreateResponse.character_id`. Tasks 1, 3, 4 depend on this.
- One-character-per-account (#329) holds, so "select" is trivial (0 or 1) — supported by `CharacterLimitReached` in the CRUD. Tasks 3, 4, 6 depend on this.
- The proto-conformance/golden gate (#68) is where wire changes are proven — supported by the D-35 fixtures pattern. Task 1 depends on this.

## Testing Strategy
- **Unit (worldd):** session state machine (post-handshake not spawned; movement/entity rejected before enter); `ENTER_WORLD` ownership check with a mocked/seeded CRUD (owned → spawn; unowned/absent → typed reject; no "Placeholder" string ever emitted).
- **Unit (protocol):** golden conformance fixtures for `EnterWorldRequest`/`Response` (decode + round-trip); existing IF-2 goldens stay byte-identical (additive).
- **Integration (worldd + real DB, the `worldd-session` CI job):** full flow against `meridian_characters` — create → enter as the real character (name/class from DB) → move; enter with an unowned id → typed reject, session survives; a fresh account cannot spawn until it creates.
- **Integration (bot):** `run_bot_client_it.sh` / `run_two_bot_it.sh` — each bot creates+enters as its own distinct character and sees the others.
- **Manual (Godot, #283):** on-device — create a character, restart the client, character persists; no-character prompts create; roster reflects the server.
- **E2E (harness):** `scripts/dev/loadtest.sh` against a char-DB-wired realm — worldd logs show each bot entered as `loadtest000N` (not `Placeholder`).

## Risks and Mitigations
| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Deploying worldd-reject before bot/client create-flow breaks all entry on a realm | Med | High | Land the coupled set together; Task 7 gates the realm deploy on the bot flow being present; do not push worldd-reject to a realm standalone (spec-verify coordinates). |
| Godot client change unverifiable in CI (#283) | High | Med | Server contract fully covered by worldd + bot ITs; Task 6 DoD is explicit manual on-device steps; the bot proves the wire flow the client mirrors. |
| Movement/entity opcodes arriving before `ENTER_WORLD` (client bug / race) | Med | Med | worldd rejects (ignores + audits) movement/entity before the entered state; unit-tested in Task 2. |
| Cross-DB reach: worldd's char_db connection can't see `meridian_characters` | Low | High | `meridian` DB user has grants on all three schemas (04-grants.sql); Task 4 verifies live that worldd loads a real character. |
| worldd chardb env incomplete (`chardb.user` empty ⇒ `char_db` null) → realm silently unenterable after Task 3, while a NAME-only grep still passes | Med | High | Task 4 renders ALL 5 `MERIDIAN_CHARDB_*` and its DoD asserts a count of 5; Task 7 gates the realm sync on the full chardb env being present (not just the bot image). |
| Client sends an unowned/forged `character_id` (server-authority probe) | Med | High | Validation is `WHERE id=? AND account_id=<grant account>`; unowned/absent → `NOT_FOUND`, never spawned; regression-tested in Task 3. |

## Pre-Mortem
*Assume this plan failed after full execution. Most likely internal reasons:*
1. **The spawn path was more entangled with `load_placeholder_character` than assumed** (Task 2/3) → Trigger: removing the fabrication leaves worldd unable to spawn even a valid character (the entity/movement init read fields the placeholder used to synthesize). Handle by threading the validated `CharacterSummary` through the existing spawn code, not deleting the load path.
2. **`HANDSHAKE_OK`-implies-spawned is baked into the bot/client** (Task 4/6) → Trigger: the bot's `on_entered_world` fires at handshake and movement starts before `ENTER_WORLD`, so the two-bot IT sees no entities. Handle by moving the "entered" signal to the `ENTER_WORLD_RESPONSE(OK)`.
3. **Coupling misjudged — a realm is left unenterable mid-implementation** (Task 7) → Trigger: worldd on a realm rejects while its bots/clients still enter at handshake. Handle by deploying the worldd + bot images from the same build and not syncing the realm until both are present.

## Goal Verification
### Truths
1. A client that creates a character, disconnects, and reconnects can enter the world as that same character (it persisted). (#341)
2. A client/bot that requests `ENTER_WORLD` with a `character_id` its account does not own receives a typed rejection and is NOT spawned.
3. An account with zero characters cannot spawn; it must `CharCreate` first (over the same session), then `ENTER_WORLD`.
4. worldd never emits a "Placeholder" character — the string/path is gone.
5. The concurrency harness runs N bots that each enter as their own distinct, server-persisted character (worldd logs name = `loadtest000N`, not `Placeholder`).
6. worldd on dev/ptr/prod is wired to `meridian_characters` (env present; loads real characters).

### Artifacts
- `schema/net/world.fbs` (ENTER_WORLD opcodes/tables/status) + generated code + golden fixtures.
- `server/worldd/world_session.cpp` / `world_dispatch.*` (two-phase session, validated `ENTER_WORLD`, no fabrication) + worldd tests.
- `deploy/helm/meridian/templates/_daemon.tpl` + `values.yaml` + `realms/{dev,ptr,prod}/values.yaml` (chardb wiring).
- `client/bot/bot_world_session.*` + bot ITs.
- `client/project/scenes/charselect/character_store.gd` + net-thread wiring.

### Key Links
- `ENTER_WORLD` handler → `meridian::characters` CRUD (`WHERE id=? AND account_id=?`) → spawn.
- worldd `chardb.*` config ← `MERIDIAN_CHARDB_*` env ← chart `worldd.chardb` values.
- bot/client `CharList/Create → ENTER_WORLD(character_id)` ← `CharListEntry.character_id`.

## Progress Tracking
- [x] Task 1: Protocol — ENTER_WORLD opcodes/tables/status + conformance fixtures
- [x] Task 2: worldd — two-phase session; remove fabrication; gate movement until entered
- [x] Task 3: worldd — ENTER_WORLD handler: validated ownership + real-character spawn
- [x] Task 4: Deploy — wire worldd → meridian_characters (chart + dev/ptr/prod)
- [x] Task 5: meridian-bot — CharList → CharCreate-if-empty → ENTER_WORLD before moving
- [~] Task 6: Godot client — real char CRUD + ENTER_WORLD over the net thread (#279) — **HANDED OFF to user (on-device, #283)**; change-spec in Task 6 section
- [x] Task 7: Coordinated deploy + E2E — harness bots enter as real characters; realm verify

**Total Tasks:** 7 | **Completed:** 6 (server/deploy/bot + live E2E) | **Remaining:** 1 — Task 6 (Godot client) handed off to user, on-device (#283)

## Implementation Tasks

### Task 1: Protocol — ENTER_WORLD opcodes, tables, status
**Objective:** Add the explicit enter-world message pair + typed status to the wire contract.
**Dependencies:** None

**Files:**
- Modify: `schema/net/world.fbs`
- Test/Create: golden conformance fixtures (follow the D-35 char-message fixtures; see `server/` proto-conformance gate #68 and existing `world.fbs` goldens)

**Key Decisions / Notes:**
- Add opcodes `ENTER_WORLD_REQUEST = 0x0016`, `ENTER_WORLD_RESPONSE = 0x0017` (next free in 0x0xxx).
- `table EnterWorldRequest { character_id:uint64; }` — the server-minted id from CharList/CharCreate. Account is grant-derived, never a field.
- `enum EnterWorldStatus : uint16 { OK=0, NOT_FOUND=1, NO_CHARACTER=2, INTERNAL=3 }` — `NOT_FOUND` covers "no such id OR not owned" (indistinguishable, avoids an ownership oracle); `NO_CHARACTER` = account has zero (hint to create).
- `table EnterWorldResponse { status:EnterWorldStatus = OK; }` (spawn state itself arrives via the existing self `ENTITY_ENTER`/movement flow; keep the response minimal).
- Additive only — existing IF-2 goldens must stay byte-identical.

**Key Decisions / Notes:**
- Generated FlatBuffers code is NOT checked in (flatc runs at build — `server/libs/proto/CMakeLists.txt`), so no generated-code commit.
- Goldens are NOT just `.bin` drops: register the new tables + their field expectations in `server/libs/proto/conformance/conformance.cpp`, then regenerate with the harness `--update` (writes the `.bin`s + `manifest.txt`) and commit both.

**Definition of Done:**
- [ ] `flatc` codegen succeeds; server + bot build against the new tables.
- [ ] `EnterWorldRequest`/`Response` registered in `conformance.cpp` with expectations; `--update` run; new `.bin`s + `manifest.txt` committed; they decode + semantically round-trip.
- [ ] Every pre-existing IF-1/IF-2 golden is byte-identical (additive-change proof).

**Verify:** `ctest -R conformance` in `server/build`.

---

### Task 2: worldd — two-phase session; remove fabrication
**Objective:** `HANDSHAKE_OK` no longer spawns; the session sits at character-select until an explicit enter; the "Placeholder" fabrication is deleted.
**Dependencies:** Task 1

**Files:**
- Modify: `server/worldd/world_session.cpp` (remove the placeholder branch), `server/worldd/world_dispatch.cpp` (session state; move the spawn/AoI `emplace` off the handshake path)
- Test (MIGRATE — these assume HandshakeOk⇒spawned and WILL break): `server/worldd/test/world_relay_test.cpp` (`do_handshake` then asserts `EntityEnter`/AoI at ~459-479) — add an `ENTER_WORLD` send to the handshake helper; audit `server/worldd/test/*session*`/`*char*` tests for post-handshake movement/entity assertions

**Key Decisions / Notes:**
- Introduce a session state: `CHAR_SELECT` (post-handshake) → `IN_WORLD` (post `ENTER_WORLD` OK). Only `CHAR_LIST/CREATE/DELETE`, `ENTER_WORLD`, `CLOCK_SYNC` valid in `CHAR_SELECT`. Movement gating is already enforced by `!ctx.movement` (`world_dispatch.cpp:~659`); make it automatic by moving the `ctx.movement.emplace(...)` (today at `~:493`, on the handshake path) into the `ENTER_WORLD`-OK path (Task 3). Same for the spawn/AoI block (`~:480-570`) which consumes `pc.char_guid`/`class_id`.
- Delete the placeholder branch of `load_placeholder_character`; keep a `load_owned_character(char_db, account_id, character_id) -> optional<CharacterSummary>` used by Task 3 (do NOT synthesize a default). Thread the real `CharacterSummary` through the existing spawn code — do not delete the spawn path (Pre-Mortem #1). Note `CharacterSummary` has no spawn position (spawn synthesizes zone-center today) and `race` is unused at spawn — both fine.
- **CCU / single-active-session decision (RESOLVED by user 2026-07-08):** MOVE the `meridian_ccu` + `meridian_sessions{state=active}` increments (`world_dispatch.cpp:~476-478`, guarded by `entered_metrics`), the `kSessionEnter` audit (`~:520-526`), and the #326 single-active-session admission (`~:588-625`, guarded by `admitted`) to the **ENTER_WORLD-OK path**. Per the user's decision, single-active is a single *in-world* session — a char-select session does NOT hold the account's slot and is NOT counted as an in-world CCU. Two clients may both sit at char-select for one account; the FIRST to `ENTER_WORLD` takes the in-world slot and a later `ENTER_WORLD` kicks it. The serve-loop teardown decrements are already independently flag-guarded (`entered`/`admitted`/`entered_metrics` at `~:1138-1170`), so moving the increment blocks keeps every decrement balanced (a session that disconnects at char-select set no flag → decrements nothing).

**Definition of Done:**
- [ ] Unit: a session is NOT spawned at `HANDSHAKE_OK` (no entity, no self `ENTITY_ENTER`, `ctx.movement` unset).
- [ ] Unit: `MOVEMENT_INTENT` before `ENTER_WORLD` is rejected (ignored + audited via the existing `!ctx.movement` path), not applied.
- [ ] `grep -rn "Placeholder" server/worldd` returns nothing in the spawn path (fabrication removed).
- [ ] `world_relay_test.cpp` migrated (sends `ENTER_WORLD` after handshake) and passes; ALL existing worldd tests pass.

**Verify:** `ctest` in `server/build` (worldd targets).

---

### Task 3: worldd — ENTER_WORLD handler: validated ownership + spawn
**Objective:** Validate the requested character is owned by the session's account, spawn the real character on OK, reply with a typed status, reject otherwise.
**Dependencies:** Task 1, Task 2

**Files:**
- Modify: `server/worldd/world_dispatch.cpp` (register + implement the `ENTER_WORLD_REQUEST` handler; move the spawn/AoI `emplace` here), `server/worldd/world_session.cpp` (`load_owned_character` + spawn the validated character), worldd `CMakeLists.txt` if a new test target is added
- Test: **extend the existing real-DB `worldd-char-mgmt-test`** (already built+run with `MERIDIAN_DB_*` in `.github/workflows/build.yml`'s `worldd-session` job) with the ENTER_WORLD ownership cases — do NOT add a brand-new test executable unless you ALSO add it to build.yml's `cmake --build --target …` list AND a `MERIDIAN_DB_*` run step (else it never executes in CI)

**Key Decisions / Notes (CI):** the `worldd-session` CI job runs an explicit target list (`.github/workflows/build.yml:~286`). A new `worldd-enter-world-test` target that isn't added there is built+run nowhere — the "real DB IT" DoD would silently never run. Prefer extending `worldd-char-mgmt-test`.

**Key Decisions / Notes:**
- Handler: `character_id` from the request; account_id from `ConnCtx` (grant). Query ownership via the CRUD/DB (`SELECT ... FROM character WHERE id=? AND account_id=?` — mirror `meridian::characters`). Found → spawn that character (feed its name/race/class/level into the existing spawn/entity path), set `IN_WORLD`, reply `ENTER_WORLD_RESPONSE(OK)`, emit the self `ENTITY_ENTER`. Not found/not owned → `ENTER_WORLD_RESPONSE(NOT_FOUND)`, no spawn, stay `CHAR_SELECT`. char_db unavailable → `INTERNAL`.
- Distinguish `NO_CHARACTER` (account has zero) from `NOT_FOUND` (bad id) if cheap — else `NOT_FOUND` suffices.

**Definition of Done:**
- [ ] IT (real DB): create a character, `ENTER_WORLD(id)` → spawned as that character (name/class from the row), not "Placeholder".
- [ ] IT: `ENTER_WORLD` with an id owned by a DIFFERENT account → `NOT_FOUND`, no spawn, session alive.
- [ ] IT: `ENTER_WORLD` with a random/nonexistent id → `NOT_FOUND`, no spawn.
- [ ] IT: fresh account (no character) `ENTER_WORLD(anything)` → `NOT_FOUND`/`NO_CHARACTER`; after `CharCreate`, `ENTER_WORLD(new_id)` → OK.

**Verify:** the `worldd-session` integration test job (real MariaDB) + `ctest`.

---

### Task 4: Deploy — wire worldd → meridian_characters
**Objective:** worldd on every realm can reach `meridian_characters` (the char store).
**Dependencies:** None (parallelizable; needed live for Task 3 IT to run against a realm, but the IT uses a local DB)

**Files:**
- Modify: `deploy/helm/meridian/templates/_daemon.tpl` (render `MERIDIAN_CHARDB_*` when `worldd.chardb.enabled`), `deploy/helm/meridian/values.yaml` (add `worldd.chardb`), `deploy/gitops/realms/{dev,ptr,prod}/values.yaml` (enable → `meridian_characters`)
- Test: `helm template` assertions

**Key Decisions / Notes:**
- ⚠️ **Render ALL FIVE `MERIDIAN_CHARDB_{HOST,PORT,USER,NAME,PASS}` — do NOT just mirror #340's NAME+PASS.** worldd enables its characters DB **only when `chardb.user` is non-empty** (`server/worldd/world_dispatch.cpp:~1043` → `if (!cfg_.char_db.user.empty())`), and its chardb path reads ONLY `MERIDIAN_CHARDB_*` (`main.cpp:~252`) — it does NOT reuse the `MERIDIAN_DB_*` host/port/user. The shared `configmap.yaml` supplies HOST/PORT/USER only under `MERIDIAN_DB_*`, and #340's env block adds only `MERIDIAN_DB_NAME`/`_PASS`. So mirroring `worldd.db` leaves `chardb.user=""` → `char_db == nullptr` → after Task 3 removes the placeholder, **every `ENTER_WORLD` returns INTERNAL and the realm is unenterable** (while a NAME/PASS-only grep would still pass — the trap).
- Render them in the worldd daemon env block (per-daemon, only when `worldd.chardb.enabled`): `MERIDIAN_CHARDB_HOST = include "meridian.db.host" .`, `_PORT = .Values.db.port`, `_USER = .Values.db.user`, `_NAME = .Values.worldd.chardb.name` (`meridian_characters`), `_PASS` via the same `secretKeyRef` as `MERIDIAN_DB_PASS`.
- Ephemeral dev DB already seeds all three schemas; the `meridian` user has cross-DB grants (04-grants.sql).

**Definition of Done:**
- [ ] `helm template` for each realm renders ALL of `MERIDIAN_CHARDB_HOST`, `_PORT`, `_USER`, `_NAME=meridian_characters`, `_PASS` for worldd (a count of 5), not just NAME/PASS.
- [ ] Live (dev, Task 7): worldd env shows all five chardb vars non-empty and its `char_db` is non-null (a real character loads on `ENTER_WORLD` — no `INTERNAL`).

**Verify:** `helm template meridian-dev deploy/helm/meridian --namespace meridian-dev -f deploy/gitops/realms/dev/values.yaml --show-only templates/worldd.yaml | grep -cE 'MERIDIAN_CHARDB_(HOST|PORT|USER|NAME|PASS)'` → expect `5`.

---

### Task 5: meridian-bot — create + enter as a real character
**Objective:** The headless bot drives the new flow so the load harness enters as real characters.
**Dependencies:** Task 1, Task 3

**Files:**
- Modify: `client/bot/bot_world_session.{h,cpp}`, `client/bot/bot_core.cpp` (post-handshake: `CharList`; if empty `CharCreate(name,race=1,class=1)`; then `ENTER_WORLD(character_id)`; fire "entered" on `ENTER_WORLD_RESPONSE(OK)`), and `bot_main.cpp` if a `--character-name` knob helps
- Test: `client/test/run_bot_client_it.sh`, `run_two_bot_it.sh`, `run_client_sees_bot_it.sh`

**Key Decisions / Notes:**
- Default character name = the bot's `--user` (unique per account, matches `add-characters`). The harness pre-creates via `meridian-character`, so `CharList` usually finds one → `ENTER_WORLD`; keep `CharCreate-if-empty` so a bare account self-provisions.
- Move the `on_entered_world` signal from handshake to `ENTER_WORLD_RESPONSE(OK)` (Pre-Mortem #2). Barrier/rendezvous in `bot_two_main.cpp` keys off the new signal.

**Definition of Done:**
- [ ] `run_two_bot_it.sh`: both bots create/enter as their own distinct characters and each sees the other's `ENTITY_ENTER`.
- [ ] A bot pointed at an unowned `character_id` (test hook) receives `NOT_FOUND` and does not spawn.
- [ ] All bot ITs pass.

**Verify:** `client/test/run_two_bot_it.sh` (local run-local realm).

---

### Task 6: Godot client — real character CRUD + ENTER_WORLD over the net thread (#279)
**Objective:** Replace the in-memory stub so the real client creates/selects/persists characters and enters as one.
**Dependencies:** Task 1, Task 3

**Files:**
- Modify: `client/project/scenes/charselect/character_store.gd` (real `CharList/CharCreate/CharDelete` over the net thread), the char-select scene/controller + the net-thread world-session GDScript (send `ENTER_WORLD`, handle `ENTER_WORLD_RESPONSE`)
- Test: manual (#283 — no headless verification)

**Key Decisions / Notes:**
- Map `CharCreateStatus`/`ENTER_WORLD` status onto the store's existing error taxonomy (the stub header documents the 1:1). The store was authored so ideally only this file + the net-thread send/handle change.
- On enter with no character → show create; on `NOT_FOUND` → return to select.

**Definition of Done (manual, on-device — #283):**
- [ ] Create a character → it appears in the server roster (visible to a second connect / the bot's `CharList`).
- [ ] Close + reopen the client → the character still exists (persisted, #341).
- [ ] Entering with no character prompts creation; after creating, enter succeeds.

**Verify:** manual on-device session against a char-DB-wired dev realm; cross-check the row in `meridian_characters`.

**STATUS: handed off to the user (on-device, 2026-07-08).** Per the stub's own policy ("unverified transport code must not land," #283) and this being the user's active client area, Task 6 is implemented by the user on-device. Everything it depends on is done + verified: the wire protocol (Task 1), the worldd `ENTER_WORLD` handler + typed statuses (Task 3), and a char-DB-wired realm (Task 4). The reference client implementation is `client/bot/bot_core.cpp` `run_world_session` (CharList → CharCreate-if-empty → ENTER_WORLD), which is proven against real worldd by `run_two_bot_it.sh`.

#### Implementer handoff (on-device change-spec)

**Opcodes (schema/net/world.fbs — already shipped):** `CHAR_LIST_REQUEST=0x0010`/`RESPONSE=0x0011`, `CHAR_CREATE_REQUEST=0x0012`/`RESPONSE=0x0013`, `CHAR_DELETE_REQUEST=0x0014`/`RESPONSE=0x0015`, `ENTER_WORLD_REQUEST=0x0016`/`RESPONSE=0x0017`. Tables: `CharListResponse{ characters:[CharListEntry{character_id,name,race,char_class,level}] }`, `CharCreateRequest{name,race,char_class}` → `CharCreateResponse{status:CharCreateStatus, character_id}`, `CharDeleteRequest{character_id}` → `CharDeleteResponse{status:CharDeleteStatus}`, `EnterWorldRequest{character_id}` → `EnterWorldResponse{status:EnterWorldStatus}` where `EnterWorldStatus{OK=0,NOT_FOUND=1,NO_CHARACTER=2,INTERNAL=3}`.

**Message flow (post-HandshakeOk, over the net thread — mirror `bot_core.cpp:run_world_session`):**
1. On entering char-select, send `CHAR_LIST_REQUEST` → render `CharListResponse.characters` into the roster (replaces `CharacterStore.list()`).
2. Create button → `CHAR_CREATE_REQUEST(name,race,char_class)`; map `CharCreateResponse.status` onto the store's existing taxonomy (the stub header documents the 1:1): `OK`→success, `INVALID_NAME`→`"invalid_name"`, `INVALID_RACE`→`"invalid_race"`, `INVALID_CLASS`→`"invalid_class"`, `DUPLICATE_NAME`→`"duplicate_name"`, `LIMIT_REACHED`→a new "limit_reached" (#329 — the stub currently doesn't cap; the server does).
3. Delete button → `CHAR_DELETE_REQUEST(character_id)`; `CharDeleteResponse.status` `OK`/`REFUSED`.
4. Enter button (currently `char_select.gd:_on_enter_pressed` → `enter_world_requested.emit(character)` at ~:214) → send `ENTER_WORLD_REQUEST(character_id)` and gate the scene switch on `ENTER_WORLD_RESPONSE`:
   - `OK` → proceed into the world scene (call `world_connect_router.note_handshake_ok()`-equivalent for the entered state; the world session is now spawned).
   - `NO_CHARACTER` → stay on char-select, prompt "create a character first".
   - `NOT_FOUND` → return to char-select / re-list (the selected id isn't owned — refresh the roster).
   - `INTERNAL` → error toast; do not enter.

**Integration seams found:** the roster/CRUD entry point is `client/project/scenes/charselect/character_store.gd` (replace the three in-memory methods with net-thread round-trips) and `char_select.gd` (which calls the store and emits `enter_world_requested`). The world entry gate is `client/project/scenes/world/world_connect_router.gd` (`note_handshake_ok()` / `has_entered_world()`), which should now flip to "entered" on `ENTER_WORLD_RESPONSE(OK)`, not on HandshakeOk. The actual frame send/recv is the existing world net transport (the same path that sends `WORLD_HELLO`/`MovementIntent` today) — add the four request sends + response handlers there.

**On-device DoD (unchanged, above):** create → appears in server roster (visible to a second connect / the bot's CharList); reopen client → character persists (#341); enter with no character prompts creation, then succeeds. Cross-check the row in `meridian_characters`.

---

### Task 7: Coordinated deploy + E2E verification
**Objective:** Land worldd + bot together on a realm and prove the server-authority end state, without leaving a realm unenterable.
**Dependencies:** Tasks 1–5 (6 verified separately/manually)

**Files:**
- Modify: `docs/ops/hosted-realms.md` / `dev-realm.md` (the new enter-world flow; remove any "placeholder" note), `scripts/dev/loadtest.sh` header if the flow note needs updating
- Test: live harness run

**Key Decisions / Notes:**
- The worldd (Task 3) and bot (Task 5) images are built from the same commit by `cd.yml`; sync the realm only after both `:dev` images are published (avoid Pre-Mortem #3). **Gate the realm sync on THREE things being live together: the new worldd image, the new bot image, AND the full `MERIDIAN_CHARDB_*` env (Task 4).** If the chardb env is missing when the worldd-reject lands, the realm is unenterable (must_fix / risk row) — verify the env FIRST (`kubectl exec` the worldd pod) before running the harness.
- Then run `scripts/dev/loadtest.sh --realm dev --count 5` (add-users → add-characters → bots) and confirm real-character entry.

**Definition of Done:**
- [x] dev realm: worldd shows all five `MERIDIAN_CHARDB_*` non-empty (`meridian-dev-mariadb:3306`, user `meridian`, `meridian_characters`, PASS len 8); pods Healthy. *(verified live 2026-07-08)*
- [x] `loadtest.sh --realm dev --count 5`: all 5 bots enter as `loadtest0001–0005` (worldd `ENTER_WORLD accepted -> spawned`, **zero `Placeholder`**), each saw 4 others (2427 EntityUpdates), 603 moves accepted. *(live)*
- [x] Unowned/foreign `ENTER_WORLD` → `NOT_FOUND` (proven by `worldd-char-mgmt` real-DB IT cases 2a/2b + bot unit test 5d — the identical worldd binary runs live). *(A dedicated live probe would need a `--force-character-id` bot flag; deferred as unnecessary.)*
- [x] Persistence: `loadtest0001–0005` still in `meridian_characters` after the run (#341 fixed). *(live)*

**RESULT (2026-07-08):** all live DoD met — CD run 28961200794 published the multi-arch images; ArgoCD rolled worldd with the chardb env; the 5-bot harness entered as real persisted characters with mutual AoI visibility. Also fixed a real deploy bug found here: the harness Jobs lacked `imagePullPolicy: Always`, so the first run used a stale cached `meridian-bot:dev` (old bot moved before ENTER_WORLD; new worldd correctly rejected it) — pinned Always.

**Verify:** `scripts/dev/loadtest.sh --realm dev --count 5` + `kubectl -n meridian-dev logs deploy/meridian-dev-worldd | grep -iE "entered|character"`.

## Open Questions
- None blocking. (`NO_CHARACTER` vs `NOT_FOUND` granularity is left to Task 1/3's discretion — both are acceptable; `NOT_FOUND` alone is sufficient.)

### Deferred Ideas
- Retire the dev **seed Job**'s account/realm seeding once the admin portal (#339) exists; the harness's `add-users`/`add-characters` remain the test path.
- Real TLS certs for PTR/Prod (separate from this spec).
