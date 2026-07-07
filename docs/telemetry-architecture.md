# Telemetry Architecture & Signal Catalog — Project Meridian

**Feature:** OPS-05 (D-29). **Milestone:** M0→.
**Reads with:** [D-29 (§9)](01-SYNC-DECISIONS.md), [Telemetry Privacy & Retention Policy](telemetry-privacy.md), [Server SAD §8.5](sad/server-sad.md), [Server PRD §6/OPS-05](prd/server-prd.md).
**Companion:** the runnable reference stack in [`server/ops/`](../server/ops/) (otel-collector + Prometheus + Loki + Grafana).

This document explains **how** observability works in Meridian and catalogs **what** signals exist. It is the design note for #162; the compose stack it describes is #163. It does not change any feature scope — it documents what D-29 already binds and what Server SAD §8.5 already enumerates.

---

## 1. Principles (from D-29)

1. **Server-side first.** Player experience (lag, corrections, disconnects, errors) is measured **authoritatively on the server**, where the UX actually happens. The client routinely sends **nothing** — its telemetry surface is exactly the D-29 client triple (crash dumps, ERROR/CRITICAL log events, missing-content placeholder events), covered by [`telemetry-privacy.md`](telemetry-privacy.md) §2a, not by this server-side stack.
2. **Prometheus-style in-process instrumentation, unchanged.** Every daemon already exposes a Prometheus `/metrics` endpoint (Server SAD §8.5). D-29 does **not** rewrite instrumentation into OTLP SDK calls — the in-process metric set stays exactly as the SAD defines it.
3. **OpenTelemetry compatibility via the collector pattern.** An **OTel Collector** in the deploy stack *scrapes* `/metrics` and *receives* OTLP logs/traces, then exports onward. OTLP is the export lingua franca: operators pipe to any backend (the reference Prometheus/Loki, or a hosted APM) **without touching daemon code**.
4. **Dashboards as code.** Grafana dashboards ship as versioned JSON in [`server/ops/grafana/dashboards/`](../server/ops/grafana/dashboards/) — never screenshots. Datasources and dashboard folders are provisioned from files, so a clean `docker compose up` yields the three v1 dashboards with no click-ops.
5. **Optional & self-hosted.** The stack is an OPS-01 extension. Daemons **degrade gracefully** to `/metrics` + local JSON logs when no collector is configured. Telemetry lands only in the operator's **own** self-hosted stack — no mandatory third-party sink (privacy policy §6).

---

## 2. Data-flow architecture

```
                      ┌──────────────────────────────────────────────────────────┐
                      │  Server daemons (authd, worldd; later gatewayd/servicesd/  │
                      │  coordd) — in-process Prometheus-style instrumentation      │
                      │                                                            │
                      │   ● GET /metrics  (Prometheus text; SAD §8.5 metric set)   │
                      │   ● OTLP push     (structured logs + session-flow traces)  │
                      └───────────────┬───────────────────────┬────────────────────┘
                          scrape /metrics            OTLP/gRPC logs+traces
                                      │                        │
                              ┌───────▼────────────────────────▼───────┐
                              │            OTel Collector               │
                              │  receivers: prometheus (scrape), otlp   │
                              │  processors: batch, resource, filter    │
                              │  exporters:  prometheusremotewrite →    │
                              │              Prometheus; loki → Loki;    │
                              │              (otlp → any backend)        │
                              └───┬───────────────┬─────────────────┬────┘
                        metrics   │        logs   │        traces   │  (M0 stub;
                                  ▼               ▼                 ▼   real spans
                          ┌────────────┐   ┌────────────┐   ┌──────────────┐  land w/
                          │ Prometheus │   │    Loki    │   │ trace backend │  M1/M2)
                          │  (metrics) │   │   (logs)   │   │  (via OTLP)   │
                          └──────┬─────┘   └──────┬─────┘   └──────┬───────┘
                                 └────────────────┼────────────────┘
                                                  ▼
                                       ┌──────────────────────┐
                                       │       Grafana        │
                                       │  provisioned:        │
                                       │  · Prometheus DS     │
                                       │  · Loki DS           │
                                       │  · 3 v1 dashboards   │
                                       └──────────────────────┘
```

**Why the collector sits in the middle.** It decouples the daemons' fixed Prometheus/OTLP surface from whatever backend an operator chooses. Swapping Prometheus for a hosted metrics service, or Loki for another log store, is a collector-config edit — no daemon rebuild. This is the "OTLP as export lingua franca" rule of D-29 made concrete.

**Client channel is out of scope here.** The three client signals (crash dumps, ERROR/CRITICAL logs, missing-content events) go to the one project-hosted, Sentry-compatible endpoint (privacy policy §2a). This stack surfaces them only as **counts** — `meridian_client_log_ingest_total` and `meridian_client_crash_total` — measured server-side at the ingest endpoint, so the Errors dashboard can chart them without the stack ever holding raw client payloads.

---

## 3. Retention (maps to the privacy policy)

Retention windows are **owned by** [`telemetry-privacy.md`](telemetry-privacy.md) §4 (judgment-call #1 — proposed defaults, owner confirms). The reference stack ships those proposed defaults as provisioned config:

| Store | Backend | Signal class | Proposed window (policy §4) | Where configured in the stack |
|-------|---------|--------------|-----------------------------|-------------------------------|
| Metrics | Prometheus | RTT, corrections, tick health, CCU, error/drop rates | **30 days** | `--storage.tsdb.retention.time=30d` (compose command flag) |
| Traces  | (OTLP backend) | Session-flow spans | **30 days** | operator's trace backend; collector forwards OTLP |
| Logs    | Loki | Structured server logs + client ERROR/CRITICAL ingest | **30 days** | `limits_config.retention_period: 720h` (`loki-config.yml`) |
| Crashes | (Sentry-compatible endpoint) | Crashpad minidumps + symbolicated reports | **90 days** | client sink, **not** this stack — noted for completeness |

These are **ceilings** — an operator MAY shorten them (privacy policy §6). Traces at M0 are stubbed (session-flow spans instrument at M1 per D-29 §7); the collector's OTLP trace path is wired so real spans light up without a config change.

---

## 4. Dashboards v1 (D-29 §9 rule 4)

Three provisioned dashboards, versioned JSON under [`server/ops/grafana/dashboards/`](../server/ops/grafana/dashboards/):

| Dashboard | Question it answers | Primary signals |
|-----------|---------------------|-----------------|
| **Realm health** | Is the realm running well? | tick p99, CCU, per-opcode rates, DB/queue latency |
| **Player experience** | How does it *feel* to play? | action-RTT histogram, movement correction / snap-back rate, disconnect reasons, reconnect outcomes |
| **Errors** | What is breaking? | server log error rate, per-opcode error/drop, client ERROR/CRITICAL ingest count, crash rate by build |

The M0 dashboards are **honest stubs**: real panels wired to the metric/log names below, so they populate the moment a daemon exposes `/metrics`. Panels for metrics that only exist post-M1 (correction/lag) or post-M2 (shard/gateway/bus) are present but will read empty until that instrumentation lands — matching the D-29 phasing.

---

## 5. Signal catalog

Every observable signal, grounded in **Server SAD §8.5** (metric set) and D-29 §9 (dashboards, log/trace pipeline). Consistent with [`telemetry-privacy.md`](telemetry-privacy.md): **no PII, no chat content, no behavioral analytics, no gameplay profiling** — every signal below is operational (health, latency, errors), not behavioral.

Label shorthand: map-scoped metrics carry `{realm, zone, shard}` per D-23 (SAD §8.5).

### 5.1 Metrics (Prometheus `/metrics` → collector scrape → Prometheus)

| Signal | Type | Source | Labels | Measures | Dashboard | Phase |
|--------|------|--------|--------|----------|-----------|-------|
| `meridian_tick_duration_seconds` | histogram | worldd | `realm,zone,shard,map` | Map tick loop duration (p99 = tick health) | Realm health | M0 |
| `meridian_ccu` | gauge | worldd | `realm,zone,shard` | Concurrent connected users | Realm health | M0 |
| `meridian_sessions` | gauge | worldd/authd | `realm,state` | Sessions by lifecycle state | Realm health / Player exp | M0 |
| `meridian_opcode_total` | counter | worldd | `realm,zone,shard,opcode` | Per-opcode message rate | Realm health | M0 |
| `meridian_opcode_dropped_total` | counter | worldd | `realm,zone,shard,opcode,reason` | Per-opcode drop rate | Errors | M0 |
| `meridian_opcode_errors_total` | counter | worldd | `realm,zone,shard,opcode` | Per-opcode error rate | Errors | M0 |
| `meridian_db_latency_seconds` | histogram | worldd/authd | `realm,db` | DB round-trip latency | Realm health | M0 |
| `meridian_aoi_entities` | gauge | worldd | `realm,zone,shard,map` | Entities in AoI per map | Realm health | M0 |
| `meridian_movement_violations_total` | counter | worldd | `realm,zone,shard,kind` | Movement-check violations (speed/teleport/bounds/flag) | Player exp / Errors | M0 (counts) / M1 (correction depth) |
| `meridian_movement_corrections_total` | counter | worldd | `realm,zone,shard` | Snap-back corrections issued to clients | Player experience | M0 |
| `meridian_disconnects_total` | counter | worldd/gatewayd | `realm,reason` | Disconnects by reason | Player experience | M0 |
| `meridian_rss_bytes` | gauge | all | `realm,process` | Process resident memory (periodic RssSampler) | Realm health / worldd / Realm overview | M0 (#297) |
| `meridian_io_workers` | gauge | worldd | `realm,process` | IO-worker pool size (#278) | worldd | M0 (#297) |
| `meridian_io_workers_busy` | gauge | worldd | `realm,process` | IO-workers currently serving a connection; busy/pool = utilization (#278) | worldd | M0 (#297) |
| `meridian_client_log_ingest_total` | counter | telemetryd (ingest endpoint) | `realm,severity,build,platform` | Client ERROR/CRITICAL log events received (count only — no payload) | Errors | M0 |
| **Deferred / future (declared but NOT emitted at M0 — #297)** | | | | | | |
| `meridian_action_rtt_seconds` | histogram | worldd | `realm,zone,shard,action` | Client→apply→client action round-trip (SAD §8.2c, < 150 ms p95 budget) | Player experience | M1 (no action-apply loop at M0 — removed from catalog until then) |
| `meridian_db_queue_depth` | gauge | worldd | `realm,db` | DB write-queue backlog | Realm health | M1 (M0 DB layer synchronous, no queue — removed from catalog until then) |
| `meridian_reconnects_total` | counter | worldd/gatewayd | `realm,outcome` | Reconnect attempts by outcome | Player experience | M1 (reconnect is client-side #96 at M0 — removed from catalog until then) |
| `meridian_grids_active` | gauge | worldd | `realm,zone,shard,map` | Active (ticking) grids per map | Realm health | M1 (catalog name reservation) |
| `meridian_instances_active` | gauge | worldd | `realm` | Active dungeon/BG instances | Realm health | M1 (catalog name reservation) |
| `meridian_saves_batched_total` | counter | worldd | `realm` | Batched persistence writes | Realm health | M1 (catalog name reservation) |
| `meridian_client_crash_total` | counter | telemetryd (ingest endpoint) | `realm,build,platform` | Client crash reports received (count only) | Errors | M1 (blocked on client crashpad #109; catalog name reservation) |
| **Sharding additions (SAD §8.5, M2+)** | | | | | | |
| `meridian_shard_population` | gauge | coordd | `realm,zone,shard` | Population per shard | Realm health | M2 |
| `meridian_shard_state` | gauge | coordd | `realm,state` | Shards by lifecycle state | Realm health | M2 |
| `meridian_transfers_total` | counter | coordd | `realm,result` | Shard transfers by result | Realm health | M2 |
| `meridian_transfer_duration_seconds` | histogram | coordd | `realm` | Shard-transfer latency (alert p99 > 1 s) | Realm health | M2 |
| `meridian_placement_decisions_total` | counter | coordd | `realm,rule` | Placement rule hit rates (party/friends/guild/balance) | Realm health | M2 |
| `meridian_gateway_forward_seconds` | histogram | gatewayd | `realm` | Gateway forward latency (< 1 ms budget) | Realm health | M2 |
| `meridian_gateway_sessions` | gauge | gatewayd | `realm` | Sessions on the gateway | Realm health | M2 |
| `meridian_bus_lane_depth` | gauge | all | `realm,link,lane` | Bus lane depth (alert > 60% sustained) | Realm health | M2 |
| `meridian_save_fence_rejections_total` | counter | worldd/coordd | `realm` | Save-ownership fence rejections (**any nonzero page-worthy**) | Errors | M2 |
| `meridian_coord_epoch` | gauge | coordd | `realm` | Coordinator epoch (failover detection) | Realm health | M3 |
| `meridian_journal_lag_seconds` | gauge | coordd (standby) | `realm` | Standby journal lag (alert > 5 s) | Realm health | M3 |

### 5.2 Logs (structured JSON → OTLP → collector → Loki)

| Signal | Type | Source | Labels/fields | Measures | Dashboard | Phase |
|--------|------|--------|---------------|----------|-----------|-------|
| Structured server logs | log stream | all daemons | `realm,process,level,event` | All server-side events; error rate derived from `level>=ERROR` | Errors | M0 |
| Client ERROR/CRITICAL ingest log | log stream | authd (ingest endpoint) | `realm,severity,build,platform,session_id` | Client-reported ERROR/CRITICAL events — **session/build/platform context only, no PII, no chat** (privacy §2a) | Errors | M0 |

Loki label cardinality is kept low deliberately: `realm,process,level` are labels; high-cardinality fields (`session_id`, `opcode`) live in the log body, queried but not indexed — both a cost and a privacy discipline (session IDs are ephemeral per privacy §3).

### 5.3 Traces (OTLP → collector → trace backend)

| Signal | Type | Source | Spans / context | Measures | Dashboard | Phase |
|--------|------|--------|-----------------|----------|-----------|-------|
| Session-flow trace | trace | authd + worldd | `auth → grant → world handshake → enter-world` | End-to-end session establishment latency + failure point | (trace explore; feeds Player exp analysis) | M1 (in-process); M2+ cross-process |

The bus envelope reserves a trace-context field (D-29 §9 rule 5), so cross-process spans light up at the M2 gateway split **without a protocol change**. At M0 the OTLP trace path is wired end-to-end in the collector but carries stub/no spans until M1 instrumentation lands.

---

## 6. Alert rules (SAD §8.5)

Provisioned alongside the dashboards (Prometheus rule file / Grafana alerting). The SAD-defined page-worthy conditions:

| Alert | Condition | Severity |
|-------|-----------|----------|
| Tick over budget | `meridian_tick_duration_seconds` p99 > tick budget | page |
| DB latency high | `meridian_db_latency_seconds` p99 high | warn |
| IO-worker saturation | `meridian_io_workers_busy / meridian_io_workers` > 90% sustained (#278/#297) | warn |
| Process memory growth | `meridian_rss_bytes` climbing over 1 h with no CCU rise (#297) | warn |
| DB queue backlog | `meridian_db_queue_depth` sustained high — **deferred to M1** (metric not emitted until the async DB queue exists, #297) | warn/page |
| CCU saturation | `meridian_ccu` near realm cap | warn |
| Save-fence rejection | `meridian_save_fence_rejections_total` > 0 | **page** (any nonzero) |
| Transfer latency | `meridian_transfer_duration_seconds` p99 > 1 s | page |
| Bus saturation | `meridian_bus_lane_depth` > 60% sustained | warn |
| Journal lag | `meridian_journal_lag_seconds` > 5 s | page |
| Shard flap | > 2 lifecycle actions / zone / hour | warn |

M0 ships the tick/DB/CCU/error alerts (the metrics that exist at M0); sharding/coordinator alerts land with their metrics at M2/M3.

---

## 7. Phasing (D-29 §9 rule 7)

| Milestone | This document / stack scope |
|-----------|-----------------------------|
| **M0** | Stack in compose, log pipeline (Loki), session-span wiring, client-ingest counts, dashboards v1 (populated for M0 metrics) |
| **M1** | Correction/lag instrumentation lands with movement/combat; RTT goes live; alert thresholds tuned against IT-M1 |
| **M2+** | Shard/gateway labels, bus-lane depth, cross-process trace spans (bus envelope already reserved) |

---

*This note documents the OPS-05 telemetry architecture and signal catalog (#162). The runnable reference stack that implements it is in [`server/ops/`](../server/ops/) (#163). Retention and privacy posture are owned by [`telemetry-privacy.md`](telemetry-privacy.md); this document stays consistent with it and does not restate the owner's judgment calls.*
