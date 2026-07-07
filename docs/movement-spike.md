# Movement-controller spike — locked shared constants + query method

**Issue:** #101 (M0 movement-controller spike). **Unblocks:** #102 (kinematic movement controller), #86 (server movement validation v1).
**Status:** DECISION. **Date:** 2026-07-06.

This spike discharges the M0 deliverable *"movement-controller spike locks shared
constants"* (Client SAD §7 M0 row) and the mitigation for **Risk R3** — *"Kinematic
controller must match server simulation across slopes/stairs/swim → M0 spike locks
shared movement constants + physics-query method"* (Client SAD §10, Client PRD §12 risk 6).

It does **two** things and nothing more (it is a spike + decision, not the controller):

1. **Locks the shared movement constants** that client prediction (CHR-02) and server
   validation (OPS-03) must agree on — a single authoritative definition, every value
   traced to a SAD/PRD number or explicitly marked as a spike decision.
2. **Decides the query method** — how the kinematic controller determines ground
   height / collision each tick.

The constants live in `client/gdextension/meridian/src/movement_constants.h`; the
query seam in `client/gdextension/meridian/src/movement_query.h`.

---

## 1. Context (why these two things, why now)

"Server is law" (Baseline Pillar 3): the client is a *predictor and presenter* — it
predicts **only** its own movement (CHR-02) and reconciles on correction (Client SAD
§2.2 (b), §3.3). For prediction to (almost) always agree with the authoritative
server, three things must be identical on both sides:

- the **constants** (speeds, tick, gravity, jump, tolerances),
- the **integration cadence** (fixed 20 Hz step),
- the **world query** (how "where is the ground / am I in bounds" is answered).

The client (this Godot GDExtension, C++) and the server (`worldd`, plain C++/Linux,
**no Godot** — Server SAD §1) are **separate build trees** (Client SAD §9.1/§9.2).
There is no shared compile-time header between them at M0, so keeping the two copies
identical is a **process/CI** problem, addressed in §4.

**M0 reality (D-19):** M0 runs on a **flat bootstrap test map** with **bounds-only**
validation — no heightfield/navmesh consumption (Sync Decisions §5, D-19). The
heightfield query path is therefore *designed and stubbed* now (flat plane, y = 0) and
wired to real terrain at **M1** with the Zone-01 greybox. Designing it now is the point
of the spike: #102 builds the controller against the final seam, not a throwaway.

---

## 2. Locked shared movement constants

Every value is traced. **[LOCKED]** = a hard number already authored in a SAD/PRD.
**[SPIKE-LOCKED]** = no document authored a number (the docs deliberately deferred
concrete speeds to "shared content data, not duplicated magic numbers" — Client PRD
§3.3), so **this spike** locks the M0 value; it is the decision-of-record #102 and #86
build against until `mcc` owns it at M1 (§4).

| Constant | Value | Kind | Source / trace |
|---|---|---|---|
| Server tick / client sim step | **20 Hz (50 ms, dt = 0.05 s)** | [LOCKED] | Server SAD §3.2 "One 50 ms map tick"; Server PRD §3.1 "fixed 20 Hz (50 ms budget)"; client fixed-tick sim Client SAD §2.2, §6.1 |
| Movement-intent send rate | **≤ 10/s + on state change** | [LOCKED] | Server SAD §5.5 "≤ 10/s/client + state changes"; Server PRD §3.2 |
| Run speed | **6.0 m/s** | [SPIKE-LOCKED] | No doc number. WoW-lineage ref (≈ 7 yd/s ≈ 6.4 m/s), CMaNGOS/TrinityCore are the acknowledged architecture refs (Baseline §2). Rounded for M0. |
| Walk speed | **2.5 m/s** | [SPIKE-LOCKED] | As above (walk ≈ 2.5 m/s). |
| Backpedal speed | **2.5 m/s** | [SPIKE-LOCKED] | WoW-style backward penalty (≈ 4.5 yd/s). |
| Strafe speed | **6.0 m/s** | [SPIKE-LOCKED] | == run at M0 (no strafe penalty until polish, M1). |
| Ground acceleration | **0 (instant / step)** | [SPIKE-LOCKED] | Not authored. Instant-accel keeps reconciliation math + the §5.5 speed check simplest and robust; matches WoW-lineage feel. Finite ramp is an M1 polish option. |
| Gravity | **20.0 m/s²** | [SPIKE-LOCKED] | Not authored. Snappier than 9.81 for MMO hop feel; policed loosely by the ±4 m z-tolerance. |
| Jump impulse (initial +y) | **6.3 m/s** (apex ≈ 0.99 m) | [SPIKE-LOCKED] | Not authored. Jump IS in M0 scope ("predicted walk/run/jump", Client PRD §7 / SAD §7 M0). Tuned to a ~1 m readable hop. |
| Terminal fall speed | **60 m/s** | [SPIKE-LOCKED] | Safety clamp; well inside the per-tick z-tolerance envelope. |
| Speed tolerance (per packet) | **× 1.15** | [LOCKED] | Server SAD §5.5 "server_speed(active_mode) × Δt × 1.15" |
| Speed sliding window | **2 s** | [LOCKED] | Server SAD §5.5 "over a sliding 2 s window" |
| Height (z) tolerance | **± 4 m** of heightfield sample | [LOCKED] | Server SAD §5.5 "z within ±4 m of heightfield/navmesh sample" |
| Heightfield grid | **129×129, 1 m spacing, f32, row-major** | [LOCKED] | Tools SAD §3.3 (chunk payload) / §5.2 `export_heightfield → f32[129×129]`; Sync Decisions §11 |
| Chunk size | **128 m** (129th sample = shared edge) | [LOCKED] | IF-6 working baseline, D-20 / Sync Decisions §5 |

**M0 movement modes:** walk / run / jump only (CHR-02 basic). Swim, slope handling,
fall-damage presentation, turn-in-place are **M1** (Client PRD §7 M1; Baseline CHR-02
"M0 basic / M1"). `MoveMode::Swim` is reserved in the header, not implemented.

**Derived:** max plausible per-packet ground displacement the server accepts is
`run × (1/10) × 1.15 ≈ 0.69 m` — a client-side self-check for "would this be corrected?"

---

## 3. Query-method decision

**Decision: the kinematic controller resolves ground height / collision each tick by
sampling the per-chunk heightfield (`f32[129×129]` @ 1 m, bilinear between the four
surrounding lattice samples) — the SAME grid the server validates against. NOT a Godot
`PhysicsServer` raycast, and NOT `CharacterBody3D::move_and_slide`.**

### Options weighed

| Option | Determinism (re-sim N×/frame) | Client/server parity | Min-spec cost | Verdict |
|---|---|---|---|---|
| **A. Heightfield bilinear sample** (shared `f32[129×129]`) | ✅ Pure math on shared data — bit-reproducible under reconciliation re-stepping | ✅ Server samples the *identical* grid (§5.5); parity by construction | ✅ A few FLOPs — free vs. the ≤ 2 ms interp/prediction Low budget (Client SAD §6.3) | **CHOSEN** |
| B. Godot `PhysicsServer` raycast per tick | ❌ Frame-coupled, non-deterministic under N re-steps in one frame | ❌ Server has **no Godot** (Server SAD §1) — cannot run it; parity impossible | ⚠️ Per-tick query cost + jitter | Rejected |
| C. `CharacterBody3D::move_and_slide` | ❌ Node-frame-coupled — the SAD explicitly rejects it (Client SAD §2.2, §9.2) | ❌ Same Godot-only problem as B; server can't mirror | ⚠️ Engine-managed, opaque | Rejected |

### Why A wins (the three binding constraints)

1. **Determinism.** Reconciliation rewinds to the authoritative state and **re-simulates
   unacked inputs N times per frame** (Client SAD §2.2 (b), movement-frame view (b)).
   A pure heightfield sample of static data is bit-identical every re-step; Godot's
   physics is stepped by the engine loop and is not safe to re-run N times inside one
   frame — which is *exactly* why the SAD mandates a custom controller with
   "character-shape sweeps, *not* `CharacterBody3D`" (Client SAD §2.2) and rejects
   node-frame coupling (Client SAD §9.2).
2. **Client/server parity.** The server is **plain C++/Linux with no Godot** and
   validates z **against the heightfield sample** to ±4 m (Server SAD §5.5, §1). The
   only way client prediction and server validation can agree by construction is for the
   client to sample the *same* `export_heightfield` grid (Tools SAD §5.2). A raycast
   against Godot collision geometry would answer a *different question* than the server
   asks, guaranteeing corrections on every slope.
3. **Min-spec budget.** A bilinear sample is a handful of FLOPs — negligible against the
   Low-tier ≤ 2 ms interp/prediction sub-budget (Client SAD §6.3) — whereas per-tick
   `PhysicsServer` queries add both cost and non-determinism.

### Collision beyond terrain (kit props, walls)

At M0 there is none (flat bootstrap map, D-19). At M1, collision-relevant kit geometry
is enumerated into the **shared Recast/Detour navmesh** (Tools SAD §3.3 item 5 / §5.2
op 5; Server PRD §3.2 — the *same* Recast tree for `forge_core`/`mcc`/`worldd`, bit-
identical tiles). The controller's character-shape sweep resolves against that shared
navmesh/collision data, again shared with the server — same principle as the heightfield.
Detailed sweep/step-offset/slope-limit tuning is **#102 + M1 "movement polish"**, not
this spike.

### What #102 implements (the seam this spike locks)

- `IWorldQuery::sample_ground(x, z) -> {height, walkable}` — the single call shape the
  controller depends on (`movement_query.h`). M0 = `FlatWorldQuery` (y = 0). M1 =
  `HeightfieldWorldQuery` (bilinear over IF-6 chunk data) — a drop-in behind the same
  interface, **no controller change**.
- A fixed-20 Hz kinematic integrator using the §2 constants: per tick, apply input →
  step horizontal velocity to `server_speed(mode)` (instant-accel) → integrate the
  jump/fall arc under `kGravity` → resolve y against `sample_ground` → clamp/land.
- The `(seq, input, state)` ring buffer + rewind/re-simulate reconciliation
  (Client SAD §2.2 (b)) driving MovementIntent/MovementState (`schema/net/world.fbs`).
- The golden cross-track fixture (§4) as the parity gate.

---

## 4. Keeping client and server in sync (single source of truth)

Because client and server are separate build trees with no shared link at M0, "single
source of truth" is a **documented sync point + a CI trip-wire**, not a shared `#include`:

- **M0 — documented sync point.** `movement_constants.h` is the client's authoritative
  copy. The **server movement validator (#86, Server SAD §5.5) MUST use byte-for-byte
  identical values**, each citing this spike and the SAD number it traces to. Any change
  is a **single PR touching both** the client header and the server validator,
  referencing #101. The header carries this contract in its top comment.
- **Golden cross-track fixture (the trip-wire).** Pin a set of
  `(start_state, input, dt, expected_state)` vectors that **both** simulations must
  reproduce exactly — Client SAD R5 ("golden fixtures shared with server track") and R3
  ("deterministic replay tests"). Client doctest (Client SAD §11) and the server's
  movement unit tests both load the fixture; drift fails **both** CIs loudly. (Authoring
  the fixture file is a #102/#86 task — this spike locks the values it will encode.)
- **M1+ — `mcc`-owned content data (the SAD end-state).** Client PRD §3.3 / Client SAD
  §2.2: the constants "live in shared content/schema data, not duplicated magic
  numbers." At M1 `mcc` compiles one authoritative constants record into **both** the
  client `.pck` (IF-5) **and** the world DB (IF-4); this header becomes the loader's
  typed view / compile-time default, no longer the authority. The **[SPIKE-LOCKED]**
  speeds are the first rows to migrate into that record.

**Recommendation:** land #86 (server validator) citing this header; author the golden
fixture with #102; open the `mcc` constants-record schema item for M1 so the SPIKE-LOCKED
values have a home before Zone-01 movement work starts.

---

## 5. Spike artifacts & verification

- `client/gdextension/meridian/src/movement_constants.h` — the locked constants (§2),
  `server_speed(mode)`, tolerances, heightfield geometry. Header-only, `constexpr`,
  engine-agnostic (no Godot types) so it links into the engine-free doctest/bot builds
  too (Client SAD §9.2).
- `client/gdextension/meridian/src/movement_query.h` — the `IWorldQuery` seam (§3) +
  the M0 `FlatWorldQuery` (D-19).
- `MeridianClient::get_movement_constants()` (bound method) — exercises the constants and
  the query seam so they are demonstrably compiled and linked into the GDExtension (the
  minimal spike proof). Not the controller.

Build/verify status is recorded in the PR description (#101).

### 5.1 What #102 delivered (the controller, on top of this spike)

#102 implemented the controller against the seam locked above, in an **engine-free core**
(Client SAD §9.2) that a plain-C++ test — and the server track (#86) — can replay:

- `client/gdextension/meridian/src/movement_controller.{h,cpp}` — the engine-free core:
  the fixed-20 Hz `integrate_tick()` (LOCKED §2 constants + `IWorldQuery::sample_ground`),
  and `PredictionReconciler` (the `(seq, input, predicted)` ring buffer + rewind/re-sim
  reconciliation + intent-rate gate). No Godot types.
- `client/gdextension/meridian/src/meridian_movement_controller.{h,cpp}` — the
  `MeridianMovementController` GDExtension node (thin Godot glue: `predict` / `reconcile`
  / `should_emit_intent`), registered in `register_types.cpp` alongside `MeridianClient`.
- `client/gdextension/meridian/test/movement_controller_test.cpp` — the engine-free unit
  test + the **golden cross-track reconciliation fixture** (§4 trip-wire): integrator
  advances at the locked speed exactly; a server correction re-simulates unacked inputs to
  server-authoritative + local residual (SAD R5/R3); intent emission respects the ≤ 10/s +
  on-state-change cap. Wired via `client/CMakeLists.txt -DMERIDIAN_CLIENT_TESTS=ON`
  (`ctest`, no Godot runtime) for the #61 client-test job.
