# `deploy/docker/` — server packaging & reference compose (D-30)

Container packaging + the reference deployment for a single-realm Meridian, per
[`docs/01-SYNC-DECISIONS.md` §10 (D-30)](../../docs/01-SYNC-DECISIONS.md). Covers
issues **#175** (GHCR autopublish on green main), **#176** (hardened images),
**#177** (compose consumes the published images).

## Layout

```
deploy/docker/
├── Dockerfile                     # parameterized multi-stage build (source of truth)
├── Dockerfile.authd               # convenience wrapper (DAEMON=authd)
├── Dockerfile.worldd              # convenience wrapper (DAEMON=worldd)
├── docker-compose.yml             # authd + worldd (GHCR images) + MariaDB
├── docker-compose.build.yml       # override: build from source instead of pull
├── db-init/                       # first-boot DB init (auth/characters/world)
└── certs/                         # TLS cert/key mount (key material git-ignored)
```

The GHCR autopublish workflow is [`.github/workflows/cd.yml`](../../.github/workflows/cd.yml).

## Images (#176)

`authd` and `worldd` are built from the **one parameterized** `Dockerfile`;
`--build-arg DAEMON=authd|worldd` selects which daemon binary ships. The build
stage mirrors CI's toolchain (`cmake` + `ninja` + `g++`, deps `flatbuffers-compiler`
/ `libflatbuffers-dev` / `libssl-dev` / `libmariadb-dev`) and compiles the server
tree Release; the runtime stage carries only the selected binary.

### Hardening baseline (D-30 §10 rule 6: "multi-stage, non-root, healthchecked")

| Choice | What | Why |
|--------|------|-----|
| **Multi-stage** | build stage compiles; runtime stage copies only the daemon binary | no compilers, headers, or source in the shipped image → smaller + smaller attack surface |
| **Small base** | `debian:bookworm-slim` runtime | minimal Debian; ships the exact glibc/libssl/libmariadb the daemons link |
| **Non-root** | dedicated `meridian` uid/gid **10001**, `nologin` shell, no home | a container escape lands as an unprivileged user, not root |
| **Runtime libs only** | `libssl3` + `libmariadb3` + `ca-certificates` — no `-dev`, no toolchain | only what the binary dlopens at runtime |
| **HEALTHCHECK** | the daemon's own `--version` | exit 0 proves the ELF + every runtime `.so` load; needs no shell/curl/open port. A deeper TCP/`/metrics` probe lands with OPS-05 |
| **EXPOSE** | authd `7100` (IF-1), worldd `7200` (IF-2) | documents the listen port for tooling |
| **Read-only-rootfs-friendly** | daemon writes only stdout/stderr, reads the ro cert mount | compose sets `read_only: true` + `no-new-privileges:true` |
| **OCI labels** | `title`/`source`/`revision`/`licenses` | provenance; `revision` stamped to the published commit by `cd.yml` |

> **Why `debian-slim` not distroless?** The brief allows either. `bookworm-slim`
> is chosen so the same base + apt line matches CI's Ubuntu toolchain contract
> exactly (one place to reason about the `libssl`/`libmariadb` ABI), and so the
> `--version` HEALTHCHECK and ops debugging work without a distroless-debug
> variant. It is still non-root, toolchain-free, and read-only-rootfs clean.
> Moving to `distroless/cc` later is a drop-in swap of the runtime stage.

## Compose (#177)

`docker-compose.yml` runs the **published GHCR images by default**; MariaDB loads
the three schemas on first boot.

```bash
# Published images (default). Pin a build with MERIDIAN_TAG=<short-sha>.
docker compose -f deploy/docker/docker-compose.yml up -d

# Build from source instead (one flag away):
docker compose -f deploy/docker/docker-compose.yml \
               -f deploy/docker/docker-compose.build.yml up -d --build
```

**DB schemas.** `db-init/*.sql` wrappers `CREATE DATABASE` + `SOURCE` the schema
files (mounted read-only), on first boot of an empty data volume:

- `meridian_auth` ← `server/db/auth/migrations/*.up.sql` (down rollbacks skipped)
- `meridian_characters` ← `server/db/characters/migrations/*.up.sql`
- `meridian_world` ← `schema/sql/world/*.sql` — the world DB is **not**
  migration-managed; it's the mcc-produced, read-only content artifact (server
  SAD §4.3). For dev we load its DDL so the DB + template tables exist; a real
  deploy points `worldd` at an mcc-emitted world DB.

**DB connection env** (12-factor, same `MERIDIAN_DB_*` names the daemons read):
`MERIDIAN_DB_HOST=db`, `_PORT=3306`, `_USER`/`_PASS` (default `meridian`/`meridian`,
overridable), `_NAME=meridian_auth` for authd. worldd has no DB dependency at M0.

**TLS.** Drop a `server.crt`/`server.key` pair in `certs/` (see `certs/README.md`
for a self-signed dev one-liner); it's mounted read-only at `/certs`.

### Running with the OPS-05 observability overlay

This compose creates the `meridian` network by name; the observability stack
(`server/ops/docker-compose.observability.yml`) attaches to it as `external: true`.
Bring both up together:

```bash
docker compose \
  -f deploy/docker/docker-compose.yml \
  -f server/ops/docker-compose.observability.yml \
  up -d
```

## Autopublish (#175)

[`.github/workflows/cd.yml`](../../.github/workflows/cd.yml) publishes to GHCR
**on green main**. It triggers via `workflow_run` on the `build` CI workflow
completing, guarded to run only when `conclusion == success` **and**
`head_branch == main` **and** `event == push`. It checks out the exact validated
commit, builds both images with Buildx from `Dockerfile`, tags **short-SHA +
`latest`**, and pushes to `ghcr.io/kwilliams312/project_meridian/{authd,worldd}`
using the built-in `GITHUB_TOKEN` (`packages: write`) — no external secret.

We use a **separate downstream workflow** (not a job appended to `build.yml`) so
CD gates on CI success without editing the CI file — matching the "new file only"
constraint and keeping CD decoupled from CI.
