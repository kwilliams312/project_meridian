<!-- SPDX-License-Identifier: Apache-2.0 -->
# M0 — Foundation: Exit Review & M1 Unfreeze

**Milestone:** M0 — Foundation
**Status:** proposed for exit (awaiting owner sign-off, #153)
**Prepared:** 2026-07-07

M0's charter (server SAD §7 build plan) was to stand up a *thin but real* vertical
slice of the whole stack — every track producing a working, integration-tested
artifact — so that M1 (the greybox vertical slice) can build on proven foundations
rather than speculative ones. This document records what shipped, the integration
proof, what was consciously deferred to M1, and the exit criteria.

---

## 1. The IT-M0 integration proof (the exit gate)

The M0 exit criterion is the cross-track loop running end-to-end. **It has been
executed live**, twice, and verified independently (rebuilt + rerun, not trusted
from an agent report):

```
login (SRP-6a over TLS 1.3, authd)
  → realm list
  → single-use session grant (IF-3)
  → worldd enter (AEAD ChaCha20-Poly1305, IF-2)
  → server-authoritative movement + validation
  → Grid/AoI relay (#87)
  → a second client / the GUI client renders the remote entity moving
```

- **Headless proof:** `client/test/run_two_bot_it.sh` — two bots enter, each sees
  the other move (`both_entered=1`, mutual `EntityUpdate`).
  `client/test/run_client_sees_bot_it.sh` — the GUI net path sees a bot move
  (`login_ok=1 entered=1 saw_peer_move=1`).
- **On-screen proof:** `scripts/dev/demo-networked.sh` — the Godot client logs in,
  enters the world, and renders a remote bot walking over worldd's AoI relay;
  local WASD drives the player's own server-authoritative capsule.

Related M0 exit items closed as satisfied: two-client session flow (#151),
IT-M0 runbook (#149), IT-M0 content build + CI gate (#123).

---

## 2. What shipped, by track

| Track | M0 deliverables (verified, merged to `main`) |
|-------|----------------------------------------------|
| **Server / authd** | TLS 1.3 listener, original SRP-6a auth (2048-bit group, constant-time proofs, RFC-vector verified), realm list, single-use IF-3 session grants, layered config (#90), account CLI. |
| **Server / worldd** | Opcode dispatch, IF-3 grant validation + session crypto, movement intake + validation v0, Grid/AoI engine + relay (#87), world-DB boot (manifest + content-hash), message bus + bus-only architecture test. |
| **Server / OPS-01** | Layered config, Dockerfiles + compose realm (#91), structured JSON logs → Loki, Prometheus `/metrics`, OTel session-flow traces, dashboards. |
| **Client / net** | GDExtension (godot-cpp) net module, dedicated net thread + lock-free SPSC (#97), login → realm → world handshake, connection state machine + reconnect-with-token. |
| **Client / sim** | Kinematic movement controller + client prediction/reconciliation, remote-entity interpolation + clock-sync estimator (#104), TPS camera. |
| **Client / boot** | Pack mount + manifest verification (IF-5), crash reporting → telemetry (#319/#109), headless bot v0 (#111). |
| **Client / macOS** | arm64 export preset + CI export job, Metal boot smoke (#114), self-hosted Apple-Silicon runner (native arm64 via `arch -arm64`). |
| **Tools / mcc** | YAML → SQL (IF-4) + pck (IF-5), deterministic build + determinism gate + golden corpus, link stage + IF-9 idmap allocator, CST-preserving YAML, validation-as-you-type. |
| **Tools / Codex + Forge** | Avalonia shell (#124), NPC/item editors, `libmccore` C ABI + P/Invoke, Forge terrain spike (A-09 decision) + EditorPlugin skeleton (#134). |
| **Audio (systems)** | ZoneMusicPlayer v1 (#144), placeholder SFX through the shared ID hook (#148), TD-11 timing gate run under load + decision D-36 (#147/#323). Placeholder tones only — real music/SFX content is M1. |
| **Cross / CI-CD** | Repo/CI foundation, GHCR image autopublish, SBOM + signing, semver release → GitHub Release, Helm chart v0, nightly test-realm redeploy + smoke + rollback. |
| **Cross / contracts** | Net protocol v1 (IF-1/IF-2/IF-3), A-15 pack-local asset tree, A-12 asset schema, telemetry privacy policy, clean-room CONTRIBUTING checklists. |

Every server + client + mcc test suite is green on `main`; the macOS runner CI is
green (server 27/27, client cores + gdext, mcc, content-build, validate).

---

## 3. Consciously deferred to M1 (with rationale)

These moved to **M1 — Greybox Vertical Slice** rather than blocking M0, because M0
proved the *systems* and M1 is where *content* and *scale-out infra* belong.

| Deferred | Why it is M1, not M0 |
|----------|----------------------|
| Art content: master material library (#139), pipeline-proof character (#140), Zone-01 starter kit (#141), art bible (#136) | M0 proved the art *pipeline* (Blender addon #137, import presets #138, provenance/budget lints #142). Producing the actual assets is greybox-slice work. |
| Music/SFX content: adaptive zone track (#146), audio-direction doc (#143) | M0 built the audio *systems* against placeholder tones (owner ruling: placeholders well into M4). Real music is content. |
| Sign-offs gated on art/music people: IF-8 registry approval (#72), A-08 requirements walk (#70), contract sign-off epic (#3) | Cannot be signed off until the art/music deliverables above exist. |
| Scale-out infra: persistent test realm (#150), test-realm host (#159), Git LFS (#160), Mac bench + procurement (#61) | M0's integration ran locally + on the CI runner; a persistent hosted realm and LFS (needed once large art assets land) are greybox-era. |
| Later platform / polish: Windows export CI (#59), HiDPI scaling (#304, LOW), demo-bot full-watch polish (#305) | Windows is a post-macOS platform (D-28); the two bugs are low-severity polish. |
| Known-issue (worked around): MoltenVK headless import abort (#283) | Upstream (Khronos) `SPIRVToMSLConverter` crash; worked around by a one-time windowed `.godot` seed + auto class-cache refresh (#321). Does not block the loop. |

---

## 4. Exit criteria

- [x] IT-M0 cross-track loop runs end-to-end, verified live (§1).
- [x] Every M0 track shipped a working, tested artifact merged to `main` (§2).
- [x] CI green on the macOS arm64 runner + all server/client/mcc suites.
- [x] Content pipeline (mcc IF-4/IF-5) deterministic + gated in CI.
- [x] Telemetry foundation (metrics/logs/traces/ingest/crash) live.
- [x] All non-deferred M0 issues closed; deferrals moved to M1 with rationale (§3).
- [ ] **Owner sign-off (#153).**

## 5. M1 unfreeze recommendation

With the foundations proven, **M1 — Greybox Vertical Slice is recommended for
unfreeze.** M1 opens with the deferred content + infra above already triaged into
its backlog, plus the greybox slice proper. The measured TD-11 `--source godot`
ratification (D-36) and a confirmed full-duration multi-client demo watch are the
first M1 verification tasks that need real runtime on the owner's machine.

---

*Sign-off:* _______________________  (owner, #153)  — date: ____________
