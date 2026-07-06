# Telemetry Privacy & Retention Policy

**Status: DRAFT — pending owner approval ([#172](https://github.com/kwilliams312/project_meridian/issues/172), OPS-05 / D-29).**
Two judgment calls are flagged inline for the owner: **retention windows** (§4) and the **opt-out default** (§5).

**Scope.** This is the one-page telemetry privacy & retention policy that *gates the client shipping story*: it MUST exist and be owner-approved before the client ships any build that sends telemetry (crash reports, ERROR/CRITICAL logs, or the missing-content events). It codifies the privacy posture already committed in D-29 (Baseline v0.6) into a concrete, operator-facing policy. It does not change any feature scope — it ratifies and bounds what OPS-05 already builds.

**Grounding.** Everything below derives from **D-29** (`docs/01-SYNC-DECISIONS.md` §9), the Server SAD §8.5 observability metric set, Server PRD §6/OPS-05, and Client PRD §8/OPS-05.

---

## 1. Principle

Player-experience is measured **server-side wherever possible**. The client sends the minimum needed to fix crashes and errors — nothing about how a player *plays*. This is both a privacy commitment and an open-source-community trust commitment.

---

## 2. What IS collected

### 2a. Client → project endpoint (the "D-29 client triple")

The client's entire outbound telemetry surface is exactly these three things, on the one project-hosted, Sentry-compatible endpoint:

1. **Crash reports** — Crashpad minidumps (Windows) / crash dumps (macOS), with build/platform context. Existing since M0.
2. **ERROR/CRITICAL log events** — a GDExtension sink on Godot's logger; **batched and rate-limited**, carrying only session ID, build, and platform context. Not general logs — only ERROR and CRITICAL severities.
3. **Perf / lag & missing-content signals** — the missing-content placeholder events (a missing asset ID renders a visible placeholder + emits an event, never a crash). These are the client-side experience signals D-29 permits; deeper lag/correction measurement is done server-side (§2b), not shipped from the client.

### 2b. Server-side signals (OpenTelemetry-compatible)

Captured authoritatively on the server, where the UX actually happens — exported OTLP-compatible via the collector pattern (in-process Prometheus-style `/metrics`, scraped by an OTel Collector; OTLP is the export lingua franca). Per Server SAD §8.5:

- **Metrics** — per-session action-RTT histograms, movement correction / snap-back rates, disconnect reasons + reconnect outcomes, per-opcode error/drop rates, tick health, CCU, DB/queue latencies.
- **Logs** — structured server logs (Loki), including server-side error rates and client ERROR/CRITICAL ingest counts.
- **Traces** — session-flow spans (auth → grant → world handshake → enter-world); cross-process spans light up at the M2 gateway split.

---

## 3. What is NEVER collected

Stated plainly. None of the following is ever collected, on client or server:

- **No behavioral analytics** — no play-session funnels, no engagement/retention tracking, no feature-usage instrumentation.
- **No PII** — no names, emails, IP-as-identity, hardware fingerprints, or any personal identifiers beyond an ephemeral session ID.
- **No chat content** — whispers, /say, party, guild, and channel messages are never telemetry payloads.
- **No gameplay tracking for profiling** — player actions, positions, purchases, and progression are not shipped or retained as telemetry for behavioral profiling. (Server-side gameplay state exists for the *game to function* and for anti-cheat; it is not a telemetry/analytics product and is out of this policy's telemetry scope.)

---

## 4. Retention windows per store

**Judgment-call #1 (owner sets the numbers).** The windows below are the **proposed default**; the owner confirms or adjusts. They are deliberately short — retain long enough to debug and tune, not to build a history.

| Store | What it holds | Proposed window |
|-------|---------------|-----------------|
| **Metrics** (Prometheus) | RTT, corrections, tick health, CCU, error/drop rates | **30 days** |
| **Traces** | Session-flow spans | **30 days** |
| **Logs** (Loki) | Structured server logs + client ERROR/CRITICAL ingest | **30 days** |
| **Crashes** | Crashpad minidumps + symbolicated reports | **90 days** |

Rationale: 30 d covers a milestone's worth of tuning and incident lookback for the high-volume operational stores; crashes get **90 d** because a rare crash signature often needs several build cycles to reproduce and confirm a fix. These are ceilings — stores may be pruned earlier. The reference stack's provisioned retention config carries these as defaults; a realm operator can shorten them (§6).

---

## 5. Opt-out mechanism

The client ships a **clear in-client toggle** (in settings) that stops all client-side telemetry — crash reports, ERROR/CRITICAL logs, and missing-content events — from being sent. The toggle's state is honored before any batch is queued for upload.

**Judgment-call #2 (owner sets the default).** **Recommendation: crash/error reporting ON by default, opt-out (not opt-in).**

- **Tradeoff for ON-by-default (opt-out):** early test builds and community realms get the crash/error signal that makes them worth shipping — D-29's own line is "nightly builds without crash telemetry are wasted testing." The cost is that a player must *act* to turn it off; the mitigation is that the payload is already privacy-minimal by §3 (no PII, no analytics, no chat) and the toggle is prominent.
- **If the owner prefers opt-in (OFF by default):** flip the default. The privacy posture is stronger, but the crash/error firehose from test-realm and community builds largely dries up, weakening the debugging value OPS-05 exists to provide. This is a one-line default flip in the client setting + the shipped policy text — the mechanism is identical either way.

Either default, the toggle itself is mandatory and its behavior is unchanged; only the shipped default value is the owner's call.

---

## 6. Community realm-operator guidance

The policy is **per-realm**. Meridian is self-hostable end to end, and telemetry follows the same posture:

- **Operators run their own OTel stack.** The reference observability stack (otel-collector + Prometheus + Loki + Grafana, with provisioned dashboards and alert rules) ships in compose as an OPS-01 extension. It is **optional** — daemons degrade gracefully to `/metrics` + local JSON logs when no collector is configured.
- **No mandatory third-party telemetry sink.** The reference posture sends telemetry to **no external service** — everything lands in the operator's own self-hosted stack. There is no project-operated analytics backend a realm is forced to phone home to. (The one project-hosted endpoint in §2a is the *reference project's* crash/error sink for *its own* official builds; a community operator hosting their own realm points client builds at their own Sentry-compatible endpoint, or none.)
- **Operators own their retention and opt-out policy.** The §4 windows and the §5 default are this project's reference defaults. A realm operator MAY set stricter (shorter retention, opt-in-only) values for their realm; they SHOULD publish whatever they run, in the same spirit as this document.

---

*This draft codifies D-29 (§9 of `docs/01-SYNC-DECISIONS.md`) into a shippable policy. On approval, remove the DRAFT banner and record the owner's resolved values for judgment-calls #1 and #2.*
