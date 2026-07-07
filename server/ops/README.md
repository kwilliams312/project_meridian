# `server/ops/` — reference observability stack (OPS-05 / D-29)

The optional, self-hostable observability stack for a Meridian realm:
**otel-collector + Prometheus + Loki + Grafana**, all config provisioned as code.

Design and the full signal catalog live in
[`docs/telemetry-architecture.md`](../../docs/telemetry-architecture.md).
Privacy posture and retention windows are owned by
[`docs/telemetry-privacy.md`](../../docs/telemetry-privacy.md).

## Layout

```
server/ops/
├── docker-compose.observability.yml   # the stack (overlay-friendly)
├── otel-collector/config.yml          # scrape /metrics + receive OTLP; export to Prometheus/Loki
├── prometheus/
│   ├── prometheus.yml                 # scrape config
│   └── rules/meridian-alerts.yml      # SAD §8.5 alert rules
├── loki/loki-config.yml               # log ingest + 30d retention
└── grafana/
    ├── provisioning/
    │   ├── datasources/datasources.yml   # Prometheus + Loki datasources
    │   └── dashboards/dashboards.yml     # dashboard provider
    └── dashboards/                       # dashboards-as-code (versioned JSON)
        ├── realm-overview.json           # golden signals (traffic/errors/latency/saturation)
        ├── worldd.json                   # simulation: tick, movement, AoI, IO-worker saturation
        ├── audit.json                    # {event="audit"} security/grant/session stream
        ├── realm-health.json
        ├── player-experience.json
        └── errors.json
```

Every panel query references a metric/log stream that is **actually emitted** by
the M0 daemons (verified against the `server/libs/metrics` catalog call sites and
the `libmeridian-core` audit stream). Signals that are declared in the catalog but
not yet instrumented at M0 are **NOT** charted on `realm-overview`/`worldd`/`audit`
(they would read "No data"); they are tracked as gaps below.

### Instrumentation gaps — closed in #297

The dashboards + alerts no longer route around declared-but-unemitted metrics.
Every panel query and alert expression now references a metric that is **actually
emitted** by an M0 daemon — the rest were removed from the catalog + dashboards or
marked as clearly-labelled future name reservations that no panel/alert queries.

**Now emitted (with tests):**

| Signal | Resolution |
|--------|------------|
| `meridian_rss_bytes` | **Emitted live** by `RssSampler` (authd + worldd) — portable macOS+Linux RSS read, sampled every 10 s. Was a startup `0` placeholder; now the RSS panels + `ProcessMemoryGrowth` alert have data. |
| `meridian_io_workers` / `meridian_io_workers_busy` | **Emitted** from worldd's IO-worker pool (pool size + in-flight). worldd dashboard charts real `busy/pool` utilization; `IOWorkerSaturation` alert replaces the old CCU stand-in proxy (#278). |
| `meridian_client_log_ingest_total` | telemetryd emits it; **`telemetryd:9464` is now a scrape target** in `otel-collector/config.yml`, so the `errors` client-ingest panel lights up. |

**Removed at M0** (no emit path; would be permanent "No data" / never-firing rules — re-add with the feature):

| Signal | Why removed |
|--------|-------------|
| `meridian_action_rtt_seconds` | No client→apply→client action loop until M1. |
| `meridian_reconnects_total` | Reconnect is client-side (#96); no server-side resume path to count. |
| `meridian_db_queue_depth` | M0 DB layer is synchronous — no write queue to measure (async pool is M1). |

**Future name reservations** (kept in the catalog, queried by no dashboard/alert):

| Signal | Blocked on |
|--------|-----------|
| `meridian_grids_active` / `meridian_instances_active` / `meridian_saves_batched_total` | worldd grid/instance/persistence work (M1). |
| `meridian_client_crash_total` | client crash handler (#109: minimal fatal-signal handler now, Crashpad seam later); emitted by telemetryd's ingest on each crash report received. Crash **sink** is Sentry-compatible, **not this stack** (privacy §4). |

## Run it

The stack attaches to the server compose network `meridian` so the collector
can scrape daemon `/metrics` by service name. As an overlay:

```bash
docker compose \
  -f server/ops/docker-compose.yml \
  -f server/ops/docker-compose.observability.yml \
  up -d
```

Standalone (no daemons to scrape yet — create the network first):

```bash
docker network create meridian
docker compose -f server/ops/docker-compose.observability.yml up -d
```

Grafana: <http://localhost:3000> (admin/admin on first boot — change it).
The three v1 dashboards appear under the **Meridian** folder.

## Daemon `/metrics` endpoint (#164, covers #93)

Each daemon (`authd`, `worldd`) exposes an **in-process Prometheus `/metrics`
endpoint** — implemented by `server/libs/metrics` (a clean-room, dependency-free
registry + tiny HTTP/1.1 responder), rendering the **SAD §8.5 metric set** with
the exact names/labels this stack's dashboards query. The collector's
`meridian-daemons` scrape job (`otel-collector/config.yml`) reads it.

| Aspect | Value |
|--------|-------|
| Default port | **9464** (the port the collector scrapes; `authd:9464`, `worldd:9464`) |
| Path | `/metrics` (the responder answers `GET`/`HEAD` on any path) |
| Transport | **plain HTTP** — the internal scrape surface, SEPARATE from the game TLS 1.3 port |
| Flags | `--metrics-port N` (0 = off), `--metrics-bind ADDR`, `--realm NAME` |
| Env | `MERIDIAN_METRICS_PORT`, `MERIDIAN_METRICS_BIND`, `MERIDIAN_REALM` |

**Why plain HTTP (documented decision, D-29 §9 rule 3):** in-process
instrumentation stays Prometheus-style; the endpoint is the collector's private
scrape hop on the operator-controlled `meridian` network, never the public
internet. TLS/mTLS on the scrape hop is a collector-config concern, not a daemon
rebuild. The default bind is loopback (`127.0.0.1`) for a bare daemon; set
`--metrics-bind 0.0.0.0` (as the compose network does) to let the collector reach
it by service name. A bind failure is logged and the daemon **continues without
metrics** (graceful degradation, rule 6) rather than refusing to serve.

Instrumented signals (matching the catalog / dashboards):
`meridian_ccu`, `meridian_sessions`, `meridian_opcode_total` / `_dropped_total`
/ `_errors_total`, `meridian_movement_corrections_total` /
`_violations_total`, `meridian_disconnects_total`, `meridian_tick_duration_
seconds`, `meridian_aoi_entities`, `meridian_db_latency_seconds`,
`meridian_connections_{accepted,closed}_total`, and the authd auth-funnel series
`meridian_auth_{attempts,results}_total` + `meridian_auth_srp_duration_seconds`.

## Optional by design

Per D-29 §9 rule 6, daemons **degrade gracefully** to `/metrics` + local JSON
logs when no collector is running. This stack is an OPS-01 extension — a realm
operator may run it, run a different backend (OTLP is the export lingua franca),
or run none. Telemetry lands only in the operator's own self-hosted stack; there
is no mandatory third-party sink (privacy policy §6).

## Retention

Provisioned defaults are the [`telemetry-privacy.md`](../../docs/telemetry-privacy.md)
§4 proposals (ceilings — shorten freely):

| Store | Window | Set where |
|-------|--------|-----------|
| Metrics (Prometheus) | 30d | `--storage.tsdb.retention.time=30d` (compose) |
| Logs (Loki) | 30d | `limits_config.retention_period: 720h` (`loki-config.yml`) |
| Traces | 30d | operator's OTLP trace backend |
| Crashes | 90d | client Sentry-compatible sink (not this stack) |
