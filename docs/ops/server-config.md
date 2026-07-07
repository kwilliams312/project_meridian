<!-- SPDX-License-Identifier: Apache-2.0 -->
# Server configuration — layered loader & key catalog (OPS-01, issue #90)

The server daemons (`authd`, `worldd`, `telemetryd`) resolve their runtime
configuration through the shared **layered loader** in `libmeridian-core`
(`meridian/core/config_loader.hpp`). This realises the PRD §"Config (OPS-01)"
model — *"layered — compiled defaults ← `/etc/meridian/*.toml` ← env-var
overrides (12-factor for containers)"* — with a highest-precedence command-line
layer added per issue #90.

## Precedence

```
Default  <  File  <  Environment  <  CommandLine        (highest wins)
```

A value set by a stronger layer is never clobbered by a weaker one, **regardless
of load order** — `Config::set` only lets an equal-or-higher layer overwrite. So
a daemon may safely parse the file last; it still cannot override an env var or a
flag.

| Layer | Source | How |
|-------|--------|-----|
| **Default** | In-code defaults registered by each daemon | `Config::set(key, val, Default)` at startup |
| **File** | `--config PATH` or `MERIDIAN_CONFIG=PATH` | TOML/INI subset, parsed into keys |
| **Environment** | `MERIDIAN_*` process env | `MERIDIAN_<A>_<B>…` → key `a.b…` (lowercased, `_`→`.`) |
| **CommandLine** | Named flags (e.g. `--port N`) **and** generic `--<key>=<value>` | Named flags map to the same keys as the env vars |

Named flags and their matching `MERIDIAN_*` env vars resolve to the **same key**
(e.g. `--metrics-port` and `MERIDIAN_METRICS_PORT` both key `metrics.port`), so
every documented env var keeps working while a flag takes precedence over it.
Any key is also directly overridable as `--<key>=<value>` (e.g.
`--db.host=10.0.0.5`).

## Config file format

A dependency-free TOML/INI subset (no third-party parser — `libmeridian-core`
stays zero-dependency):

```ini
# '#' or ';' start a comment (first non-space column only — no inline comments,
# so values may contain '#'). Blank lines are ignored.
realm = "reference"          # a bare key -> key "realm"; quotes are stripped

[db]                         # a section prefixes the keys under it with "db."
host = 127.0.0.1             # -> db.host
port = 3306                  # -> db.port
pass = 'p@ss#word'           # single or double quotes; '#' kept inside the value

[metrics]
bind = "127.0.0.1"           # -> metrics.bind
```

Section headers and keys may be dotted (`[a.b]` → prefix `a.b.`). A malformed
line (no `=`, or an unterminated `[section`) stops the parse and is reported with
its 1-based line number; keys before it still apply. A **missing** file is not an
error — the daemon falls back to defaults + env + flags (12-factor friendly).

## Key catalog

Types: `str`, `int`, `bool` (`1/true/yes/on` ↔ `0/false/no/off`).

### Common to all daemons

| Key | Type | Default | Flag | Env |
|-----|------|---------|------|-----|
| `config` | str | — | `--config` | `MERIDIAN_CONFIG` |
| `realm` | str | `reference` | `--realm` | `MERIDIAN_REALM` |
| `metrics.port` | int | `9464` (0=off) | `--metrics-port` | `MERIDIAN_METRICS_PORT` |
| `metrics.bind` | str | `127.0.0.1` | `--metrics-bind` | `MERIDIAN_METRICS_BIND` |
| `log.format` | str | env/`json` | `--log-format` | `MERIDIAN_LOG_FORMAT` |
| `log.level` | str | env/`info` | `--log-level` | `MERIDIAN_LOG_LEVEL` |

`log.format`/`log.level`/`realm` also flow through the logger's own
`configure_from_env()` baseline; the layered values override it.

### authd (login / realm-list / session-grant)

| Key | Type | Default | Flag | Env |
|-----|------|---------|------|-----|
| `tls.cert` | str | — (required) | `--cert` | — |
| `tls.key` | str | — (required) | `--key` | — |
| `bind` | str | `0.0.0.0` | `--bind` | — |
| `port` | int | `7100` (IF-1) | `--port` | — |
| `login.build_floor` | int | `0` | `--build-floor` | — |
| `otlp.endpoint` | str | — (off) | `--otlp-endpoint` | `MERIDIAN_OTLP_ENDPOINT` |
| `trace.sample_ratio` | str/float | `1.0` | `--trace-sample-ratio` | — |
| `db.socket` `db.host` `db.port` `db.user` `db.pass` `db.name` | str/int | ConnectParams defaults (`127.0.0.1:3306`) | — | `MERIDIAN_DB_SOCKET/HOST/PORT/USER/PASS/NAME` |

### worldd (shard worker / map simulation)

| Key | Type | Default | Flag | Env |
|-----|------|---------|------|-----|
| `tls.cert` | str | — (required) | `--cert` | — |
| `tls.key` | str | — (required) | `--key` | — |
| `bind` | str | `0.0.0.0` | `--bind` | — |
| `port` | int | `7200` (IF-2) | `--port` | `MERIDIAN_WORLDD_PORT` (legacy alias) |
| `io.workers` | int | auto (`cores−3`) | `--io-workers` | — |
| `otlp.endpoint` | str | — (off) | `--otlp-endpoint` | `MERIDIAN_OTLP_ENDPOINT` |
| `trace.sample_ratio` | str/float | `1.0` | `--trace-sample-ratio` | — |
| `worldd.realm.id` | int | WorldServerConfig default | — | `MERIDIAN_WORLDD_REALM_ID` |
| `db.*` (auth DB, IF-3 grants) | str/int | ConnectParams defaults | — | `MERIDIAN_DB_*` |
| `chardb.*` (characters DB) | str/int | ConnectParams defaults | — | `MERIDIAN_CHARDB_*` |
| `worlddb.*` (content DB, IF-4) | str/int | ConnectParams defaults | — | `MERIDIAN_WORLDDB_*` |
| `worlddb.expected.hash` | str | — | — | `MERIDIAN_WORLDDB_EXPECTED_HASH` |

### telemetryd (client telemetry ingest)

| Key | Type | Default | Flag | Env |
|-----|------|---------|------|-----|
| `ingest.port` | int | `9469` (0=ephemeral) | `--ingest-port` | `MERIDIAN_INGEST_PORT` |
| `ingest.bind` | str | `127.0.0.1` | `--ingest-bind` | `MERIDIAN_INGEST_BIND` |
| `ingest.path` | str | `/api/1/store/` | `--ingest-path` | `MERIDIAN_INGEST_PATH` |
| `rate.max` | int | `100` | `--rate-max` | — |
| `rate.window_ms` | int | `60000` | `--rate-window-ms` | — |

## Backward compatibility

Every `MERIDIAN_*` environment variable and every named flag that worked before
issue #90 still works and resolves the same effective value. The only deliberate
semantic change: where a knob was settable by **both** a flag and an env var, the
**command line now wins** over the environment (previously the daemons applied
env *after* flags for a handful of knobs). This matches the documented precedence
`Default < File < Environment < CommandLine`.
