# CD Phase 4 — In-Cluster Concurrency Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
>
> ⚠️ Task 5 runs real load in-cluster. Start SMALL (a few bots) to prove the harness; large runs are a deliberate operator choice (resource usage).

**Goal:** Deliver the payoff of the hosted realms — an **in-cluster concurrency harness** that seeds N test accounts and drives N headless `meridian-bot` sessions against a realm's `authd`/`worldd` (by ClusterIP), spread across both arch pools, plus an external NodePort path for ad-hoc/real-client testing. This is the k8s version of the local `scripts/dev/demo-networked.sh` (#338).

**Architecture:** Two new images built by the multi-arch `cd.yml` — `meridian-bot` (client `-DMERIDIAN_BOT=ON`, engine-free — confirmed no godot-cpp) and `meridian-account` (server `tools/meridian-account` CLI). Two k8s Jobs: an `add-users` Job (runs `meridian-account` to seed `loadtest0001..N` into the realm's auth DB) and a `loadtest` **Indexed Job** (`parallelism=N` bot pods, each logging in as `loadtest<index>` and driving a session for a duration). A thin `scripts/dev/loadtest.sh` kicks it off against a chosen realm.

**Tech Stack:** the multi-arch `cd.yml` + ARC runners (Phase 2b), the live realms (Phase 1/3), `meridian-bot`/`meridian-account` C++ CLIs, k8s Indexed Jobs.

**Branch:** author on `dev`, exercise against the Dev realm first. **Reference:** spec §7; `scripts/dev/{add-users.sh,demo-networked.sh}`.

**Known CLIs (verified):**
- `meridian-account` — cmake target `server/tools/meridian-account`; creates an account against `MERIDIAN_DB_*` env with the **password piped on stdin** (see `add-users.sh`). Idempotent (exit 3 = duplicate).
- `meridian-bot --authd HOST:PORT --worldd HOST:PORT --user U --password P [--realm R] [--duration D]` (from `client/bot/bot_main.cpp`).

**Open items to resolve during implementation (flagged, not placeholders):**
1. **Bot TLS verification** — the bot CLI has no `--insecure`/`--ca` flag yet it drives `run-local`'s self-signed realm in `#338`; confirm the client TLS uses `VERIFY_NONE` (grep `client/net`/`login_transport` for `SSL_CTX_set_verify`). If it actually verifies, add a bot `--insecure` flag or point the harness at a realm with a trusted cert. **This gates the whole harness — resolve it in Task 0.**
2. **Exact `meridian-account` create invocation** — mirror `add-users.sh` (subcommand + stdin password).
3. **Bot context subset** — `Dockerfile.bot` needs `client/` but must exclude `client/godot-cpp` (submodule, unused under `MERIDIAN_BOT`) and any Godot binaries from the build context.

---

## Task 0: Resolve bot TLS verification (spike)

**Files:** none (investigation; may add a bot flag).

- [ ] **Step 1:** `grep -rn 'SSL_CTX_set_verify\|VERIFY_NONE\|VERIFY_PEER\|set_verify' client/` and read the client TLS setup (`client/net/*` login transport). Determine whether the bot verifies the server cert.
- [ ] **Step 2:** If `VERIFY_NONE` (or equivalent) → the harness works against `selfSignedInit` realms as-is; record and proceed. If it verifies → add a `--insecure` flag to `meridian-bot` (small change in `bot_main.cpp` + the TLS setup) OR require a trusted-cert realm; pick the `--insecure` route (dev/test harness) and note it. **Do not proceed to Task 3 until this is settled** — the bot must be able to connect to the target realm.

---

## Task 1: `meridian-account` + `meridian-bot` images

**Files:** Create `deploy/docker/Dockerfile.account`, `deploy/docker/Dockerfile.bot`; modify `.dockerignore`, `.github/workflows/cd.yml`.

- [ ] **Step 1: `deploy/docker/Dockerfile.account`** — multi-stage, mirrors the daemon image (build the server tree, ship one binary):

```dockerfile
# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: Apache-2.0
# meridian-account — the M0 account-creation CLI (server/tools/meridian-account).
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build g++ pkg-config \
        flatbuffers-compiler libflatbuffers-dev libssl-dev libmariadb-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY schema/ ./schema/
COPY server/ ./server/
RUN cmake -S server -B server/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build server/build --target meridian-account
FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 libmariadb3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 meridian && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin meridian
COPY --from=build /src/server/build/tools/meridian-account/meridian-account /usr/local/bin/meridian-account
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/meridian-account"]
```
(Confirm the built binary path under `server/build/...` during the build — adjust the COPY if cmake lays it out differently.)

- [ ] **Step 2: `deploy/docker/Dockerfile.bot`** — build the client with `-DMERIDIAN_BOT=ON` (returns before godot-cpp):

```dockerfile
# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: Apache-2.0
# meridian-bot — engine-free headless load bot (client -DMERIDIAN_BOT=ON; no Godot).
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build g++ pkg-config \
        flatbuffers-compiler libflatbuffers-dev libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY schema/ ./schema/
COPY client/ ./client/
RUN cmake -S client -B client/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMERIDIAN_BOT=ON \
    && cmake --build client/build --target meridian-bot
FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 meridian && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin meridian
COPY --from=build /src/client/build/bot/meridian-bot /usr/local/bin/meridian-bot
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/meridian-bot"]
```

- [ ] **Step 3: `.dockerignore`** — the bot build needs `client/` (currently fully excluded). Add an exception but keep the heavy/unused bits out:
```
!client
client/godot-cpp
client/**/*.import
```
(Verify after: `client/bot`, `client/net`, `client/CMakeLists.txt`, and the client core sources the bot links ARE in context; `client/godot-cpp` is NOT.)

- [ ] **Step 4: Add both images to the `cd.yml` matrices** — extend the `build` and `merge` job matrices from `[authd, worldd, meridian-db]` to `[authd, worldd, meridian-db, meridian-account, meridian-bot]`, and extend the `dockerfile` step's `case` to map `meridian-account → Dockerfile.account` and `meridian-bot → Dockerfile.bot` (no build-arg for either). Validate YAML.

- [ ] **Step 5: Commit; push to trigger a build; confirm both images publish multi-arch** (registry API check as in Phase 2b — expect `[amd64, arm64]` for `meridian-account:dev` and `meridian-bot:dev`).

---

## Task 2: `add-users` Job

**Files:** Create `deploy/gitops/loadtest/add-users.job.yaml`. (Applied ad-hoc, not GitOps-synced — it's a run-once tool.)

- [ ] **Step 1:** Write a parameterized Job that runs `meridian-account` against the realm's auth DB. It should seed `${PREFIX}0001..${COUNT}` with a shared password. Mirror `add-users.sh`'s invocation (subcommand + stdin password + `MERIDIAN_DB_*` env pointing at `meridian-<realm>-mariadb`, DB `meridian_auth`). Use an env-substituted template (`envsubst`) so `loadtest.sh` can set REALM/COUNT/PREFIX/PASSWORD. Include the `meridian-account` image `:<realm-tag>`. Idempotent (dup accounts exit 3 → treat as skip).

- [ ] **Step 2:** Dry-run render (`envsubst < ... | kubectl apply --dry-run=client -f -`) and commit.

---

## Task 3: `loadtest` Indexed Job

**Files:** Create `deploy/gitops/loadtest/loadtest.job.yaml`.

- [ ] **Step 1:** Write an **Indexed** Job (`completionMode: Indexed`, `parallelism: ${N}`, `completions: ${N}`) whose pod runs `meridian-bot`:
  - `--authd meridian-${REALM}-authd:7100 --worldd meridian-${REALM}-worldd:7200` (ClusterIP service names)
  - `--user ${PREFIX}$(printf '%04d' $((JOB_COMPLETION_INDEX+1)))` `--password ${PASSWORD}` `--duration ${DURATION}`
  - Pod anti-affinity on `kubernetes.io/arch` (soft) so bots spread across both node pools.
  - `restartPolicy: Never`, sane resource requests, non-root.
- [ ] **Step 2:** Dry-run render + commit.

---

## Task 4: `loadtest.sh` wrapper + docs

**Files:** Create `scripts/dev/loadtest.sh`, `docs/ops/concurrency-harness.md`.

- [ ] **Step 1:** `scripts/dev/loadtest.sh --realm dev --count 50 --duration 120` — `envsubst`s the two Job templates, applies `add-users` (waits for completion), then applies `loadtest`, and tails progress. Include `--external` docs (drive from a laptop via `<node-ip>:31710`).
- [ ] **Step 2:** `docs/ops/concurrency-harness.md` — usage, the ClusterIP vs NodePort paths, how to scale N, and cleanup (`kubectl delete job`). Commit + push.

---

## Task 5 (LIVE): prove the harness against Dev — START SMALL

- [ ] **Step 1:** `scripts/dev/loadtest.sh --realm dev --count 5 --duration 60` — 5 accounts, 5 bots, 60s.
- [ ] **Step 2:** Verify: the `add-users` Job completes (5 accounts in `meridian_auth`); the `loadtest` Indexed Job runs 5 bot pods **spread across amd64 + arm64 nodes** (`kubectl get pods -o wide`); pods complete `Succeeded`; bot logs show login→enter-world→move.
- [ ] **Step 3:** Confirm worldd saw the concurrent sessions (worldd logs / metrics if OPS-05 enabled). Scale up only deliberately (e.g. `--count 50`) once the small run is clean.
- [ ] **Step 4:** Record results in `docs/ops/concurrency-harness.md`; commit + push.

---

## Self-Review

**Spec coverage (§7):** in-cluster bot Jobs (scale) + external NodePort path → Tasks 3/5 ✓; account seeding → Task 2 ✓; bot/account images → Task 1 ✓; spread across arch pools → anti-affinity (Task 3) ✓.

**Risks flagged:** bot TLS verification (Task 0 gate — the one thing that could block everything); the `meridian-account` binary path + exact create subcommand (confirm at build/run); `.dockerignore` `client/` exception must exclude `godot-cpp`. **Load scale is operator-gated** — Task 5 starts at 5 bots.

**Consistency:** service names `meridian-${REALM}-authd:7100` / `-worldd:7200` match the chart's ClusterIP services; account usernames `${PREFIX}<0001..N>` match the bot `--user` index formula; image tags `:<realm>` match cd.yml's per-branch publishing.
