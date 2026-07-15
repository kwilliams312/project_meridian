<!-- SPDX-License-Identifier: Apache-2.0 -->

# `deploy/helm/meridian` — Helm chart v0 (Kubernetes deployment option)

A Helm chart that deploys a single-realm Project Meridian server (`authd` +
`worldd`) to Kubernetes. This is the **Kubernetes option** from
[`docs/01-SYNC-DECISIONS.md` §10 (D-30)](../../../docs/01-SYNC-DECISIONS.md) —
the scale-out / orchestrated deployment that ships **alongside** the reference
docker compose, not instead of it.

> **D-30 §10 rule 3:** _"Kubernetes is a supported option, not the reference: a
> versioned Helm chart ships alongside the compose file — v0 covers the
> single-`worldd` realm (M0–M2 topology + the optional OPS-05 observability
> stack)… Charts are config-only consumers of the same images and the same
> 12-factor env overrides — no K8s-specific code paths in the daemons."_

## Relationship to the compose default

| | `deploy/docker/` (compose) | `deploy/helm/meridian` (this chart) |
|--|--|--|
| Role | **Reference / default** deployment + contributor onboarding | **Optional** scale-out / orchestrated deployment |
| Images | `ghcr.io/kwilliams312/project_meridian/{authd,worldd}` | **same** images |
| Env | `MERIDIAN_DB_*`, `--cert/--key/--bind/--port` | **same** 12-factor env + flags |
| DB | bundled MariaDB (first-boot schema init) | **external** MariaDB (see [Database](#database)) |
| Security | `read_only: true`, `no-new-privileges`, uid 10001 | **same** posture as K8s `securityContext` |
| TLS | `certs/server.{crt,key}` mounted ro at `/certs` | Secret mounted ro at `/certs` |

Both consume the identical GHCR images built by the one parameterized
`deploy/docker/Dockerfile` (#176) and published by `.github/workflows/cd.yml`
(#175). The daemon binary in the image is `/usr/local/bin/meridiand`, runs as
non-root uid/gid **10001**, and its `--version` is a valid health signal — the
chart reuses all three facts.

## What it deploys (v0 — M0–M2 topology)

- **authd** — IF-1 login / realm-list / session-grant, TLS 1.3 on port **7100**.
  Reads the `meridian_auth` DB (`MERIDIAN_DB_*`).
- **worldd** — IF-2 shard worker / map simulation, TLS 1.3 on port **7200**.
  No DB dependency at M0 (SAD §5.2) — `worldd.db.enabled=false` by default.
- A **ConfigMap** for non-secret DB connection config (host/port/user, + metrics
  knobs when observability is on).
- A **Secret** for the DB password (or reference your own via `db.existingSecret`).
- A **Secret** for the TLS cert/key (or reference your own via `tls.existingSecret`).
- A **ClusterIP Service** per daemon exposing the game port (+ metrics port when
  observability is on).

The sharded-topology chart (`gatewayd`/`coordd`/workers/`servicesd`) lands with
OPS-04 at M3 (D-30 §10 rule 3) — out of scope for v0.

## Prerequisites

- Kubernetes 1.23+ and Helm 3.
- An **external MariaDB** reachable from the cluster, with the three schemas
  loaded (`meridian_auth`, `meridian_characters`, `meridian_world`) and an app
  user granted on them — see [Database](#database).
- A **TLS server cert + key** (operator PKI / cert-manager, or a dev self-signed
  pair — the same material the compose `certs/README.md` describes).

## Install

The chart requires two things to be provided at install time: **a DB password**
(or an existing DB Secret) and **a TLS Secret** (existing, or created from PEM).

### Quick start (dev) — chart creates both Secrets

```bash
# Generate a throwaway self-signed pair (same one-liner as compose certs/README):
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key -out server.crt \
  -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

helm install meridian deploy/helm/meridian \
  --namespace meridian --create-namespace \
  --set db.host=my-mariadb.default.svc.cluster.local \
  --set db.password='change-me' \
  --set tls.create=true \
  --set-file tls.cert=server.crt \
  --set-file tls.key=server.key
```

### Production — reference externally-managed Secrets

Provision the DB password and TLS material as Secrets you own (external-secrets
operator, sealed-secrets, cert-manager, …), then point the chart at them:

```bash
# DB creds Secret must carry key MERIDIAN_DB_PASS (override via db.passwordKey).
kubectl create secret generic meridian-db --from-literal=MERIDIAN_DB_PASS='...'
# TLS Secret must carry server.crt / server.key (override via tls.certKey/keyKey).
kubectl create secret generic meridian-tls \
  --from-file=server.crt=server.crt --from-file=server.key=server.key

helm install meridian deploy/helm/meridian \
  --namespace meridian --create-namespace \
  --set db.host=my-mariadb.prod.svc.cluster.local \
  --set db.existingSecret=meridian-db \
  --set tls.existingSecret=meridian-tls \
  --set image.tag=<short-sha-or-release-semver>
```

Pin `image.tag` to a commit short-SHA or release semver in production (D-30 §10
rule 1: images are tagged commit-SHA + `latest`); `latest` is only the dev
default.

## Values to set

The values you will almost always touch:

| Value | Default | Purpose |
|-------|---------|---------|
| `image.tag` | `latest` | Pin the daemon image (short-SHA / release semver). |
| `image.repository` | `kwilliams312/project_meridian` | GHCR repo prefix; daemons are `.../authd`, `.../worldd`. |
| `db.host` | `meridian-mariadb` | External MariaDB host / in-cluster Service. |
| `db.port` / `db.user` | `3306` / `meridian` | DB connection. |
| `db.password` | `""` | DB password → chart-created Secret (dev). |
| `db.existingSecret` | `""` | Reference an existing DB Secret (prod). Key `db.passwordKey`. |
| `db.names.{auth,characters,world}` | `meridian_{auth,characters,world}` | The 3-DB split (SAD §4). authd uses `auth`. |
| `tls.create` | `false` | If true, create a TLS Secret from `tls.cert`/`tls.key` (use `--set-file`). |
| `tls.existingSecret` | `""` | Reference an existing TLS Secret (keys `server.crt`/`server.key`). |
| `authd.replicaCount` / `worldd.replicaCount` | `1` | Replicas per daemon. |
| `authd.port` / `worldd.port` | `7100` / `7200` | Game listen ports (IF-1 / IF-2). |
| `authd.resources` / `worldd.resources` | see values.yaml | CPU/memory requests + limits. |
| `authd.probes.type` / `worldd.probes.type` | `exec` | `exec` (`--version`) or `tcp` (game port). |
| `worldd.db.enabled` | `false` | worldd has no DB dependency at M0 (SAD §5.2). |
| `realmTheme` | `""` (core) | Content theme worldd serves (chibi-theme design §4, #762). Set to a pack namespace (e.g. `chibi`) → worldd gets `MERIDIAN_REALM_THEME`; also point `mariadb.contentSeed.sqlPath` at that theme's `/content/world-content-<theme>.sql`. |
| `mariadb.contentSeed.sqlPath` | `/content/world-content.sql` | Baked world-DB DML the content-seed Job loads. A themed realm points this at `/content/world-content-<theme>.sql` (single-pack). |
| `observability.enabled` | `false` | Turn on the in-process `/metrics` endpoint (OPS-05). |

Full documented set: [`values.yaml`](values.yaml).

### Non-root / read-only-rootfs posture

The chart asserts, at the K8s layer, the same hardening the image already ships
(defense in depth): `runAsNonRoot`, `runAsUser: 10001`, `readOnlyRootFilesystem:
true` (== compose `read_only`), `allowPrivilegeEscalation: false` (== compose
`no-new-privileges`), and `capabilities: drop [ALL]`. A `/tmp` `emptyDir` is
mounted so the read-only rootfs still has scratch space. Tunable via
`podSecurityContext` / `containerSecurityContext`.

### Health probes

Liveness/readiness default to the daemon's own `--version` (`exec` probe) — the
exact signal the Dockerfile `HEALTHCHECK` uses (exit 0 ⇒ the ELF + every runtime
`.so` loads). Switch a daemon to a TCP probe on its game port with
`authd.probes.type=tcp` once the listener is known to accept pre-handshake
connects; a deeper `/metrics` probe lands with OPS-05.

## Database

**v0 assumes an external, operator-managed MariaDB — the chart does NOT bundle a
MariaDB subchart.** Rationale:

1. **D-30 §10 rule 3** makes charts _config-only consumers_ of the same 12-factor
   env — the DB is a connection target, not chart-owned state.
2. The **world DB is not migration-managed** — it is the mcc-produced, read-only
   content artifact (SAD §4.3); a real deploy points `worldd` at an mcc-emitted
   world DB, which a bundled MariaDB would fight.
3. Stateful DB lifecycle (backups, HA, PVC sizing) is better owned by a
   dedicated operator (e.g. mariadb-operator, a cloud managed DB) than by this
   app chart.

Provide the three schemas out-of-band (the compose ships the DDL under
`deploy/docker/db-init/` + `server/db/**/migrations/` for reference) and set
`db.host` / `db.user` / `db.password` (or `db.existingSecret`). authd connects to
`db.names.auth` (`meridian_auth`); worldd needs no DB at M0.

> **Bundling MariaDB later** is a follow-up: add a `dependencies:` entry in
> `Chart.yaml` (e.g. Bitnami MariaDB) guarded by a `mariadb.enabled` value, and a
> migration/seed Job. Deferred so v0 stays a thin, config-only consumer.

## Observability (OPS-05, optional)

`--set observability.enabled=true` turns on each daemon's **in-process Prometheus
`/metrics` endpoint** (`server/ops/README.md`: port **9464**, plain HTTP; flags
`--metrics-port` / `--metrics-bind`, env `MERIDIAN_METRICS_PORT` / `_BIND` /
`MERIDIAN_REALM`). When on, the chart:

- adds `MERIDIAN_METRICS_*` to the ConfigMap and `--metrics-port/--metrics-bind`
  to each daemon,
- exposes a `metrics` port on the container and Service,
- sets `prometheus.io/{scrape,port,path}` pod annotations for discovery.

An in-cluster Prometheus / otel-collector then scrapes the daemons by Service
name. Daemons **degrade gracefully** when no collector runs (D-29 §9 rule 6), so
this is safe to leave off. Set `observability.realm` to label the series (defaults
to the release name). The full OPS-05 stack (Grafana/Loki/Prometheus/collector)
is a separate deployment (`server/ops/`), not bundled here in v0.

## Verify the render locally

```bash
helm lint  deploy/helm/meridian --set db.password=x --set tls.existingSecret=meridian-tls
helm template meridian deploy/helm/meridian \
  --set db.password=x --set tls.existingSecret=meridian-tls | kubectl apply --dry-run=client -f -
```

(A `helm-lint` CI job is a noted follow-up — see the PR.)

## Uninstall

```bash
helm uninstall meridian --namespace meridian
```

This removes the chart-created objects. Externally-managed Secrets and the
external MariaDB are untouched.
