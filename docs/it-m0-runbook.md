# IT-M0 Acceptance Runbook

**Status:** Draft for review (issue #149). **Executes:** issue #151. **Milestone:** M0 exit.
**Read with:** [Game-Design Baseline §3](00-GAME-DESIGN-BASELINE.md) · the five PRDs' IT-M0 rows
([Server](prd/server-prd.md) §8.3 · [Client](prd/client-prd.md) §10.3 · [Tools](prd/tools-prd.md) §11.3 ·
[Art](prd/art-prd.md) §8.3 · [Music](prd/music-prd.md) §8.3) · [Sync Decisions](01-SYNC-DECISIONS.md).

> This is a **planning document**. It is the executable script that the IT-M0 acceptance run (#151)
> follows. It does **not** require the software to already exist — several steps below reference
> components that are not yet built; each such step names its **gating issue** and stays in the script
> so the run is fully specified in advance. When a gate closes, the step becomes runnable unchanged.

---

## 1. Purpose & scope

**What IT-M0 proves** (Baseline §3): *log in through `authd`, enter world on `worldd`, move a
character, and see another connected client move* — end to end, across all five tracks, with real
content flowing through the real pipe. It is the M0 "it actually works" gate.

Consolidated from the per-track IT-M0 contribution + done-criteria rows, IT-M0 demonstrates:

1. **Auth** — two clients complete the IF-1 login flow against `authd` (SRP-6a over TLS 1.3), receive
   a realm list, select the realm, and obtain a single-use session grant. *(Server, Client)*
2. **Enter-world** — each client hands the grant to `worldd`, is admitted, and spawns the D-11
   character stub on the flat bootstrap test map. *(Server, Client)*
3. **Movement + AoI** — each client predicts its own movement locally and, via `worldd`'s AoI relay,
   **sees the other client move with < 250 ms visible latency** (client target: smooth interp at
   ≤ 150 ms simulated latency, no teleporting). *(Server, Client)*
4. **Reconnect** — a transient disconnect within the reconnect window resumes the session without a
   relog. *(Client, Server)*
5. **Content visibility** — the test NPC + starter items authored in Codex reach the world through the
   full `YAML → mcc → world DB (SQL) + client .pck` path; zero hand-written SQL. *(Tools, Server)*
6. **Art visible + animating** — `art.char.human.male.base` is visible and animating at 30 FPS on
   min-spec in the dressed test map; both clients see each other's character move. *(Art)*
7. **Adaptive audio** — both clients hear the one adaptive zone track; a debug state switch produces a
   bar-quantized layer change with no seam; asset IDs resolve from compiled content. *(Music)*

**Out of scope for IT-M0** (deferred by design): chunk streaming / real terrain (M0 runs the **flat
bootstrap map**, D-19); IF-6 heightfield/navmesh movement validation (M1, Zone-01 greybox); the
`gatewayd`/IF-2 encryption split (M2); CHR-01 appearance (M1 — M0 is the name+class D-11 stub).

---

## 2. Roles

| Role | Who / what | Drives |
|------|-----------|--------|
| **Operator** | One human (or the deploy pipeline) | Preconditions §4, service deploy, kicks off the run, records results in the §6 table, calls PASS/FAIL. |
| **Client A** | A real Godot client on a min-spec machine | Steps 1–7 as a first-class player: login, enter-world, move, reconnect, sees B, hears audio. |
| **Client B** | The **second client** — either a second real client on a second machine, or the **headless bot client v0** (#111, `--headless`, scripted login/move) | The "other connected client": logs in, enters world, moves on a script so A observes it; also serves as the soak driver for the zero-crash criterion. |
| **Content author** | Content team, via **Codex** authoring + the **`mcc`** compiler | Pre-run: authors the test NPC + starter items in Codex, compiles with `mcc`, pushes the world DB delta + client `.pck` (Precondition §4.3). No live role during the run. |
| **Bot fleet** | 10 bots (bot client v0, #111) | The **soak** driver — 10 bots on the realm for ≥ 1 h to exercise the server zero-crash criterion (Server done-criterion) and the 30-min client soak (Client done-criterion). |

The minimal run needs **one operator + Client A (real) + Client B (bot)**. A two-real-client run
(A + B both human, two machines) additionally satisfies the client "two real clients on two machines"
wording; the bot substitution is explicitly permitted (#151: "one may be the bot").

---

## 3. Environments

| Environment | Description | Gating issue |
|-------------|-------------|--------------|
| **Test-realm host** | Ubuntu LTS + Docker VPS (2–4 vCPU, M0–M1 CCU sizing), DNS, firewall opening **only the authd/world ports**, deploy-pipeline access. The nightly redeploy targets this host and IT-M0 runs on it. | **#159** (provision) |
| **`authd` deploy** | The login/realm/grant daemon (stateless, LB-able), TLS 1.3 listener, connected to the `auth` MariaDB. **Built** (IF-1 login flow, #79 / #201). | Deploy: D-30 compose (#159 dovetail) |
| **`worldd` deploy** | The shard worker: boots the world DB from the `mcc` artifact (TLS-01), accepts the grant handoff, runs movement intake / validation v0 (speed + bounds) + AoI relay on the flat bootstrap map. | **#151 blocker** — `worldd` movement/AoI **not built yet** |
| **MariaDB (3-DB split)** | `auth` (accounts, realms, grants) · `characters` (durable player state) · `world` (read-only `mcc` artifact). | `auth` built (#79/#80); `world` via `mcc` |
| **Content pack** | The compiled world DB delta (SQL) + client `.pck` produced by `mcc` from the Codex-authored test NPC + starter items + the M0 starter art kit + the adaptive track. | Content authoring; `mcc` v0 |
| **Min-spec client** | GTX 1060-class machine (authoritative min-spec gate) running the Godot client; a second machine (or a bot host) for Client B. | Client not started (#111 bot) |

---

## 4. Preconditions

All must be green **before** the run. The operator checks each off.

### 4.1 Accounts created
- Two accounts exist in the `auth` DB, created via the **`meridian-account`** CLI (#80 / #200):
  `meridian-account create --username <A>` and `--username <B>` (SRP-6a verifier; passwords never
  stored plaintext). Confirm exit code 0; a duplicate username exits 3.
- Credentials for A and B are held by whoever/whatever drives each client (human or bot config).

### 4.2 Realm seeded
- At least one **realm** row exists in the `auth` DB pointing at the deployed `worldd` (host/port),
  so `authd`'s RealmList returns it and RealmSelect yields a grant. *(Gate: realm-seed step of #159 deploy.)*

### 4.3 Content compiled + pushed
- Test NPC + starter items authored in **Codex**; the M0 starter art kit + `art.char.human.male.base`
  + the one adaptive track (5 stems, 2 stingers) present in `/content`.
- `mcc` compiles from a tagged `/content@<tag>` to (a) the **world DB SQL** and (b) the client **`.pck`**;
  build is reproducible from the tag. **Zero hand-written SQL** — everything came through `mcc`.
- `worldd`'s world DB loaded from that artifact; the client `.pck` shipped to both client machines.

### 4.4 Services deployed
- `authd` + `worldd` running on the test-realm host via the D-30 compose stack; content-hash logged by
  `worldd` on boot (TLS-01). Session-flow trace spans (auth → grant → handshake → enter-world) emitting.
- Firewall exposes only the authd + world ports.

---

## 5. Scripted steps

Numbered, per-track. Each has an explicit **expected result**. Steps run in order; Client A and
Client B each traverse steps 1–3 (B may be scripted via the bot). "Gate" names the blocking issue for
any not-yet-built component; the step stays in the script regardless.

### Step 1 — Auth / login *(Server IT-M0: authd login + realm list + grant · Client IT-M0: Login UI → authd TLS → realm select)*
1.1 Client A opens the login UI, enters username + password, connects to `authd` over **TLS 1.3**.
1.2 IF-1 flow runs: ClientHello/ServerHello (build floor + proto check) → SrpStart → SrpChallenge{salt, B}
    → SrpProof{A, M1} → **AuthResult{M2}**. Password is never sent; SRP proves knowledge.
1.3 Client A sends RealmListRequest → receives RealmList (the seeded realm from §4.2).
1.4 Client A sends RealmSelect{realm_id} → receives **SessionGrant{grant_id, session_key,
    reconnect_window_ms}** (single-use, ~30 s TTL).
1.5 Repeat 1.1–1.4 for Client B (real or bot).

**Expected:** both clients reach AuthResult success and hold a valid single-use grant. Failure UX is
exercised separately (bad password → clean protocol rejection; realm down; version mismatch → build
floor / proto reject). *Real component: `server/authd` (#79/#201); `meridian-account` accounts (#80/#200).*

### Step 2 — Enter world *(Server IT-M0: grant handoff + character stub · Client IT-M0: worldd handoff → character stub → empty map)*
2.1 Client A opens the `worldd` connection and authenticates with the **session grant/token** — never
    re-sends the password. `worldd` atomically consumes the single-use grant (rejects reuse).
2.2 `worldd` spawns Client A's **D-11 character stub** (name + class, no appearance) on the **flat
    bootstrap test map** (D-19: bounds-only movement validation, no heightfield).
2.3 Client A's `.pck` mounts the core content pack (IF-5); the map is dressed with the M0 starter kit.
2.4 Repeat 2.1–2.3 for Client B.

**Expected:** both characters are in-world on the same map, each rendering `art.char.human.male.base`.
*Gate: `worldd` enter-world path — #151 blocker (worldd not built).*

### Step 3 — Movement + AoI (< 250 ms see-each-other-move) *(Server IT-M0: movement intake/validation v0 + AoI relay · Client IT-M0: predicted movement + remote interp · Art IT-M0: character animating)*
3.1 Client A moves (predicted local walk/run/jump). `worldd` intake validates v0 (speed + bounds) and
    relays via AoI to Client B.
3.2 Client B moves (scripted if bot); `worldd` relays to Client A.
3.3 On each client, the **other** character moves smoothly via remote interpolation — **no teleporting**.
3.4 Measure: the other client's movement is **visible within < 250 ms** (Server done-criterion); client
    target is smooth interp at **≤ 150 ms simulated latency** (Client done-criterion). Character
    animates with the locomotion stub at **30 FPS on min-spec** (Art done-criterion).

**Expected:** each client sees the other move within the latency budget, smoothly, animating at 30 FPS.
*Gate: `worldd` movement/AoI — #151 blocker. Art asset via §4.3 pipe.*

### Step 4 — Reconnect *(Client IT-M0: disconnect/reconnect works · Server §5 reconnect)*
4.1 Client A (or B) suffers a transient network drop **within the reconnect window**
    (`reconnect_window_ms`, ~30 s).
4.2 The client's `net` state machine reconnects with the token; the session **resumes without a relog**.
    (A drop **beyond** the window re-authenticates through `authd` — grants are single-use.)

**Expected:** the session resumes in-world without returning to the login screen; the character is where
it was, still visible to the other client. *Gate: client `net` reconnect (#111 bot exercises it) + `worldd`.*

### Step 5 — Content visibility via the `mcc` pipe *(Tools IT-M0: NPC + starter items via mcc → worldd DB + client .pck)*
5.1 Both clients observe the **test NPC** and **starter items** in-world — the ones authored in Codex.
5.2 Confirm provenance: the content in-world came through **`YAML → mcc → SQL/.pck`**, **zero
    hand-written SQL**, reproducible from `/content@<tag>` (verify against §4.3 build log).

**Expected:** Codex-authored content is visible in-world and traceable to the `mcc` artifact.
*Real component: `tools/mcc` (v0). Gate: content in `worldd` DB depends on the §2 deploy.*

### Step 6 — Adaptive audio *(Music IT-M0: 1 adaptive track in ZoneMusicPlayer; IDs resolve from compiled content)*
6.1 On both clients, the one **adaptive zone track** is **audible** via the `ZoneMusicPlayer` runtime.
6.2 A **debug state switch** (explore → tension/combat) produces a **bar-quantized** layer change with
    **no audible seam**; transition timing is measured bar-accurate under load (TD-11 gate evidence).
6.3 Confirm asset IDs **resolve from compiled content** (TLS-01 hook path), not hardcoded paths; login
    UI click + footstep placeholders play through the same ID path.

**Expected:** both clients hear the track; the debug flip is seamless and bar-quantized; IDs resolve
from the compiled pack. *Real component: `mcc` Ogg encode + loudness lint (v0). Gate: client runtime.*

### Step 7 — Soak (zero crashes) *(Server done-criterion: 10 bots / 1 h zero crashes · Client done-criterion: 30-min soak zero crashes)*
7.1 Run the **10-bot fleet** (#111) on the realm for **≥ 1 h**; `worldd` survives with **zero crashes**.
7.2 Real clients complete a **30-min soak** with **zero crashes**; crash reporting is live.

**Expected:** no crashes across the soak windows. *Gate: bot fleet #111; crash telemetry (Client M0).*

---

## 6. Pass/fail recording

One row per done-criterion. Operator fills **Result** (PASS / FAIL / BLOCKED) and **Notes** during the run.
"BLOCKED" = component not yet built (record the gating issue); it is not a FAIL of the plan.

| # | Track | Done-criterion (measurable) | Step | Result | Notes |
|---|-------|-----------------------------|------|--------|-------|
| DC-1 | Server | Both clients log in via `authd` (IF-1 SRP/TLS), get realm list + single-use grant | 1 | ☐ | |
| DC-2 | Server | Grant handoff to `worldd` accepted; single-use grant consumed (reuse rejected) | 2 | ☐ | |
| DC-3 | Server | Character stub spawned; movement intake + validation v0 (speed + bounds) | 2–3 | ☐ | |
| DC-4 | Server | AoI relay: each sees the other move with **< 250 ms** visible latency | 3 | ☐ | |
| DC-5 | Server | Dockerized deploy survives **10 bots for 1 h, zero crashes** | 7 | ☐ | |
| DC-6 | Client | Full session flow: login UI → authd TLS → realm select → worldd handoff → stub → empty map | 1–2 | ☐ | |
| DC-7 | Client | Each sees the other move **smoothly (interp, no teleporting) at ≤ 150 ms** simulated latency | 3 | ☐ | |
| DC-8 | Client | Disconnect/reconnect works (resume within window, no relog) | 4 | ☐ | |
| DC-9 | Client | **Zero crashes across a 30-min soak** | 7 | ☐ | |
| DC-10 | Tools | Content in-world came through **YAML → mcc → SQL/.pck**; zero hand-written SQL | 5 | ☐ | |
| DC-11 | Tools | Build **reproducible from `/content@<tag>`** | 4.3 / 5 | ☐ | |
| DC-12 | Art | Two clients see each other's `art.char.human.male.base` move (locomotion stub) at **30 FPS on min-spec** | 3 | ☐ | |
| DC-13 | Art | Art bible approved; pipeline + import presets documented (pre-run artifact) | 4.3 | ☐ | |
| DC-14 | Music | Both clients **hear the track**; debug state switch = **bar-quantized layer change, no seam** | 6 | ☐ | |
| DC-15 | Music | Transition timing **bar-accurate under load** (TD-11 gate evidence) | 6 | ☐ | |
| DC-16 | Music | Asset IDs **resolve from compiled content**, not hardcoded paths | 6 | ☐ | |

**Overall IT-M0 verdict:** PASS only when DC-1 … DC-16 are all PASS. Record the `/content@<tag>`, the
`authd`/`worldd` build IDs, and the world content-hash logged at boot alongside the verdict.

---

## 7. Cross-reference — step → owning track's done-criteria row

| Step | Owning PRD done-criteria row(s) | DC rows |
|------|--------------------------------|---------|
| 1 — Auth / login | Server §8.3 IT-M0 (authd login + realm list; grant handoff) · Client §10.3 IT-M0 (login UI → authd TLS → realm select) | DC-1, DC-6 |
| 2 — Enter world | Server §8.3 IT-M0 (grant handoff; character stub) · Client §10.3 IT-M0 (worldd handoff → stub → empty map) | DC-2, DC-6 |
| 3 — Movement + AoI | Server §8.3 IT-M0 (movement intake/validation v0; AoI relay, < 250 ms) · Client §10.3 IT-M0 (predicted movement; remote interp, ≤ 150 ms) · Art §8.3 IT-M0 (character animating, 30 FPS min-spec) | DC-3, DC-4, DC-7, DC-12 |
| 4 — Reconnect | Client §10.3 IT-M0 (disconnect/reconnect) · Server PRD §5 (reconnect = single-use grant re-auth) | DC-8 |
| 5 — Content visibility | Tools §11.3 IT-M0 (NPC + starter items via mcc → worldd DB + client .pck; zero hand-written SQL; reproducible) | DC-10, DC-11 |
| 6 — Adaptive audio | Music §8.3 IT-M0 (1 adaptive track in ZoneMusicPlayer; bar-quantized flip; IDs from compiled content) | DC-14, DC-15, DC-16 |
| 7 — Soak | Server §8.3 IT-M0 (10 bots / 1 h zero crashes) · Client §10.3 IT-M0 (30-min soak zero crashes) | DC-5, DC-9 |
| Pre-run (§4) | Art §8.3 IT-M0 (art bible approved; pipeline documented) · Tools §11.3 IT-M0 (mcc build reproducible) | DC-13, DC-11 |

---

## 8. Gating issues (what must land before #151 can run)

| Component | Status | Issue |
|-----------|--------|-------|
| Test-realm host (Ubuntu + Docker + DNS/firewall/deploy access) | To provision | **#159** |
| `authd` login flow (IF-1 SRP/TLS/grant) | **Built** | #79 / #201 |
| `meridian-account` CLI (account creation) | **Built** | #80 / #200 |
| `worldd` movement intake / validation v0 / AoI relay + enter-world | **Not built** | #151 blocker (build next) |
| Client (login UI, sim, net/reconnect, stream) | Not started | Client epic #9 |
| Bot client v0 (`--headless`, second client + soak driver) | Not started | **#111** |
| Codex content + `mcc` artifacts (NPC, items, art kit, track) | In progress | Tools / Content |

When these close, the §5 script runs unchanged and the §6 table is filled to produce the IT-M0 verdict.
