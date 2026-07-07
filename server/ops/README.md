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
        ├── realm-health.json
        ├── player-experience.json
        └── errors.json
```

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
