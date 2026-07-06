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
