# CD Infrastructure — Hosted Realms (Dev / PTR / Prod) on Talos + ArgoCD

- **Date:** 2026-07-07
- **Status:** Design (awaiting approval)
- **Author:** Ken Williams (with Claude)
- **Repo:** `github.com/kwilliams312/project_meridian`

## 1. Overview

Stand up three GitOps-managed hosted realms of Project Meridian in the existing
Talos Kubernetes cluster — **Dev, PTR, and Prod** — each tracking its own git
branch, kept in sync by the cluster's ArgoCD. Provide a concurrency-test harness
that can drive load both from inside the cluster (scale) and from an external
NodePort endpoint (ad-hoc / real client). Rework CD to publish **multi-arch**
(amd64 + arm64) images built natively on self-hosted runners on the cluster's own
nodes. **The local build env is retained unchanged.**

### Goals

- One canonical hosted-deploy model: **GitOps via ArgoCD**, replacing the older
  compose-over-SSH `nightly-redeploy.yml` path (which was never provisioned).
- Three long-lived realms with a **branch-promotion flow**: `dev → ptr → main`.
- **Multi-arch** images so pods schedule on both the amd64 NUCs and arm64 Talos
  nodes, built **natively** (no QEMU) on in-cluster self-hosted runners.
- An **on-demand concurrency harness** (in-cluster bot Jobs + external endpoint).
- Reuse the mature `deploy/helm/meridian` chart; realms are values overlays.

### Non-goals

- Replacing the local `scripts/dev/run-local.sh` loop (explicitly retained).
- Gameplay/feature work; new daemon functionality.
- Ingress/HTTP routing — the daemons speak **raw TLS over TCP**, so exposure is
  NodePort (see §5.3), not an HTTP Ingress.
- A full secrets-management platform — one-time secrets are provisioned by hand
  from a checklist (§8).

## 2. Context (as-built, verified 2026-07-07)

**Existing repo infrastructure**
- `deploy/docker/Dockerfile` — one parameterized multi-stage image builds both
  C++20 daemons (`authd`, `worldd`) via cmake + Ninja + g++, linking
  `libssl3`/`libmariadb3`; non-root uid 10001, read-only-rootfs friendly.
  Currently built **amd64-only** (no `platforms:` in CD).
- `.github/workflows/cd.yml` — GHCR autopublish on green `main`, tags
  `<short-sha>` + `latest`, cosign **keyless** signing + Syft CycloneDX SBOM
  attestation. Gated on `workflow_run(build) == success && head_branch == main`.
- `deploy/helm/meridian/` — Helm chart mirroring compose; **assumes an external
  MariaDB** (`db.host: meridian-mariadb`), honours the 3-DB split
  (auth/characters/world), mounts a TLS cert Secret at `/certs`, asserts the
  non-root/read-only posture. No DB bundled.
- `deploy/docker/db-init/*.sql` — the 3-DB schema + grants (`01-auth`,
  `02-characters`, `03-world`, `04-grants`).
- `client/bot/` — `meridian-bot`, an engine-free **C++ headless bot** (cmake
  option `MERIDIAN_BOT=ON`); the concurrency driver.
- `scripts/dev/add-users.sh` — bulk test-account provisioner wrapping the
  `meridian-account` CLI against the local throwaway MariaDB.
- `.github/workflows/nightly-redeploy.yml` + `deploy/scripts/redeploy.sh` — the
  **older** compose-over-SSH hosted-deploy model to an unprovisioned "test realm
  host" (#159), guarded to skip. **Superseded by this design** (see §9).

**Cluster facts (context `delayedpackets-talos-default`)**
- **Talos** (immutable OS, API-driven; no node SSH; containerd, not dockerd).
- Nodes: 3× amd64 (`nuc1..3`) + 4× arm64 (`talos-*`), all Ready.
- **ArgoCD v3.2.1** live in ns `argocd` (application, applicationset, and
  notifications controllers present). Existing apps auto-sync from `main`
  (`usurper-prod` → Kustomize `k8s/overlays/prod`). This repo is already
  registered with ArgoCD.
- **Longhorn** is the default StorageClass (`longhorn`) — used for PTR/Prod PVCs.
- **LoadBalancer is non-functional** (no MetalLB / Cilium L2): the one
  `LoadBalancer` service (`enigma-bbs`) sits `<pending>` but is reachable via its
  allocated **NodePorts**. ⇒ external exposure = NodePort.
- **No cert-manager** installed ⇒ dev/realm TLS certs need a self-signed
  bootstrap.
- `cloudflare` namespace present (tunnel) — HTTP-oriented; not on the raw-TCP
  game path.

## 3. Realm model & promotion flow

Three branches, each mapped 1:1 to a realm; all three realms run in the one
cluster as separate namespaces.

| Realm | Branch | Namespace | MariaDB | Moving tag | NodePorts authd/worldd | Replicas | ArgoCD sync |
|-------|--------|-----------|---------|-----------|------------------------|----------|-------------|
| **Dev** | `dev` | `meridian-dev` | Deployment + `emptyDir` (ephemeral) | `:dev` + auto-restart | 31710 / 31720 | 1 | auto (prune + selfHeal) |
| **PTR** | `ptr` | `meridian-ptr` | StatefulSet + Longhorn PVC | `:ptr` + auto-restart | 31730 / 31740 | 2 | auto (prune + selfHeal) |
| **Prod** | `main` | `meridian-prod` | StatefulSet + Longhorn PVC | `:prod` (+`:latest`) | 31750 / 31760 | 3 | **manual sync** |

**Promotion flow:** work lands on `dev` → merge `dev → ptr` → merge `ptr → main`.
Each branch's green CI builds + publishes images; each realm's ArgoCD
`Application` tracks its own branch (`targetRevision: dev|ptr|main`), so a
promotion is a branch merge ArgoCD picks up.

**Image tagging:** every branch build publishes an immutable `:<short-sha>` plus a
moving per-branch tag (`:dev`, `:ptr`, `:prod`); `main` additionally keeps
`:latest` for compose/local parity. Realm values reference the moving tag with
`imagePullPolicy: Always`.

**Auto-restart vs the Prod gate:** after a publish, an in-cluster runner triggers
ArgoCD's **native Deployment `restart` action** (not `kubectl rollout restart`,
which would show as drift and fight selfHeal) so pods repull the moving tag.
This fires **automatically for `dev` and `ptr` only**. **Prod is manual-sync**:
promoting to Prod is a deliberate operator action (`argocd app sync
meridian-prod` + the restart action), preserving a human gate in front of
production. Rollback at any tier = `git revert` on that branch (+ manual
sync/restart for Prod).

## 4. GitOps repository layout

New top-level `deploy/gitops/`:

```
deploy/gitops/
  root-app.yaml                 # app-of-apps; registered in ArgoCD once, owns all below
  apps/
    meridian-dev.yaml           # Application → chart + realms/dev  → ns meridian-dev  (auto)
    meridian-ptr.yaml           # Application → chart + realms/ptr  → ns meridian-ptr  (auto)
    meridian-prod.yaml          # Application → chart + realms/prod → ns meridian-prod (manual)
    arc-controller.yaml         # Application → actions-runner-controller (OCI Helm)
    arc-runners.yaml            # Application → the two arch-pinned runner scale-sets
  realms/
    dev/values.yaml             # ephemeral DB, :dev, NodePort 317x, 1 replica
    ptr/values.yaml             # Longhorn StatefulSet, :ptr, NodePort 317x, 2 replicas
    prod/values.yaml            # Longhorn StatefulSet, :prod, NodePort 317x, 3 replicas
  runners/
    values.yaml                 # ARC AutoscalingRunnerSet config (amd64 + arm64)
  loadtest/
    add-users.job.yaml          # seed accounts into a realm's auth DB
    loadtest.job.yaml           # Indexed Job: N meridian-bot pods
```

- All `Application`s: `project: default`, `repoURL` = this repo, `targetRevision`
  = the realm's branch, Helm source `path: deploy/helm/meridian` with
  `valueFiles: [../../gitops/realms/<realm>/values.yaml]` (or inline
  `valuesObject`), matching `usurper-prod`'s conventions.
- `root-app.yaml` is the only object applied out-of-band (once); everything else
  is pure git thereafter.

## 5. Helm chart extensions (`deploy/helm/meridian`)

### 5.1 Bundled MariaDB (new templates, `mariadb.enabled`)
- **Ephemeral (Dev):** `Deployment` + `emptyDir`.
- **Persistent (PTR/Prod):** `StatefulSet` + `volumeClaimTemplates` on the
  `longhorn` StorageClass. Driven by `mariadb.persistence.enabled` (+ `size`,
  `storageClass`).
- **Seeding:** mount `deploy/docker/db-init/*.sql` as a ConfigMap into
  `/docker-entrypoint-initdb.d` — the exact 3-DB split + grants used locally, so
  local↔cluster parity holds.
- authd targets the in-cluster `Service` `meridian-mariadb`.
- DB password from a chart-created Secret (dev) or `db.existingSecret` (PTR/Prod).

### 5.2 TLS bootstrap (no cert-manager)
- A **pre-sync Helm-hook `Job`** generates a self-signed server cert into the
  Secret the chart mounts at `/certs`, if the Secret is absent. Keeps private
  keys out of git.
- `meridian-bot` must connect with the realm CA pinned or verification skipped —
  **confirm the bot exposes such a flag during Phase 1** (open item, §10). If not,
  the bootstrap must emit a CA the bot can be pointed at.

### 5.3 Exposure toggle
- Per-daemon `service.type` ∈ `ClusterIP | NodePort | LoadBalancer`.
- Realms use **NodePort** with the pinned ports in the §3 matrix so all three
  coexist and client configs are stable.
- A **ClusterIP** Service always exists (service-name resolvable) for in-cluster
  bots regardless of the external toggle.

### 5.4 Reused knobs
- `replicaCount`, `resources`, `nodeSelector`/`affinity`, `probes`,
  `podSecurityContext`/`containerSecurityContext` already in the chart. Multi-arch
  images (§6) mean **no arch nodeSelector** is needed once Phase 2 lands.

## 6. CD pipeline rework

### 6.1 Self-hosted runners — ARC (new infra, GitOps-managed)
- Install **Actions Runner Controller** (controller + two
  `AutoscalingRunnerSet`s): `meridian-amd64` (nodeSelector → NUCs) and
  `meridian-arm64` (nodeSelector → Talos arm nodes), registered to this repo.
- Runners need to build C++ Docker images → **dind** mode for buildx. On Talos
  (containerd only) this requires privileged dind pods; the ARC namespace's Pod
  Security Admission must permit `privileged`. **Primary risk — see §10.**
  Fallbacks: rootless BuildKit, or an emulated-buildx stopgap.
- Managed by the `arc-controller` + `arc-runners` ArgoCD Applications.

### 6.2 `build.yml` + `cd.yml` triggers
- Expand both to run on push to **`dev`, `ptr`, `main`** (today `cd.yml` gates on
  `head_branch == main` only).

### 6.3 Multi-arch build + publish (`cd.yml`)
- Matrix `{daemon: authd, worldd, bot} × {arch: amd64, arm64}` on the
  arch-matched self-hosted runner → **native** build, push each arch by digest.
- A `merge` job runs `docker buildx imagetools create` to assemble the manifest
  lists: `:<short-sha>`, the moving branch tag (`:dev`/`:ptr`/`:prod`), and
  `:latest` on `main`.
- **cosign keyless sign + Syft SBOM attest** run on the **merged manifest-list
  digest** (moving the existing provenance from the per-arch image to the list).

### 6.4 New `bot` image
- `deploy/docker/Dockerfile.bot` builds `meridian-bot` with `MERIDIAN_BOT=ON`,
  same hardened multi-stage pattern as the daemon image.

### 6.5 Auto-restart job
- Final `cd.yml` job on an in-cluster runner runs, for `dev`/`ptr` only:
  `argocd app actions run meridian-<realm> restart --kind Deployment` for
  authd/worldd (+ mariadb only when its image changes). Authenticated via an
  ArgoCD token Secret or in-cluster SA RBAC on the `Application`. **Skipped for
  `main`** (Prod promotion is manual).

## 7. Concurrency-test harness

- **`add-users` Job** (`loadtest/add-users.job.yaml`) — containerizes the
  `meridian-account` CLI logic behind `scripts/dev/add-users.sh` to seed
  `PREFIX0001..N` accounts into a target realm's auth DB. Idempotent (dup
  username → skip), same summary semantics.
- **`loadtest` Indexed Job** (`loadtest/loadtest.job.yaml`) — `parallelism=N`
  `meridian-bot` pods, each driving M connections at the realm's `authd`/`worldd`
  **ClusterIP**. Pod anti-affinity spreads load across **both** arch pools so the
  cluster itself generates the load. Knobs: accounts-per-pod, connections,
  duration, target realm.
- **Kickoff:** `kubectl create -f` or a thin `scripts/dev/loadtest.sh` wrapper
  (realm, N, duration).
- **External path:** documented — point a laptop-driven `meridian-bot` or the real
  Godot client at `<any-node-ip>:<realm authd NodePort>`.

## 8. Prerequisites / one-time secrets (checklist, provisioned by hand)

- **ARC → GitHub:** a GitHub App (preferred) or repo PAT with runner-registration
  scope → k8s Secret in the ARC namespace.
- **ArgoCD token:** an ArgoCD account/token (or in-cluster SA + RBAC) so the
  restart job can call `argocd`.
- **GHCR pull:** images are public today ⇒ no pull secret; if flipped private, add
  a `ghcr-pull` docker-registry Secret + `imagePullSecrets`.
- **root-app** registered in ArgoCD once (`kubectl apply -f
  deploy/gitops/root-app.yaml` or via the UI).
- **Branches:** create `dev` and `ptr` from `main`; set branch protection /
  promotion policy as desired.

## 9. Relationship to existing paths

- **Local build env — retained, unchanged.** `scripts/dev/run-local.sh` +
  throwaway MariaDB stays the canonical local loop; shared `db-init/*.sql` keeps
  local↔cluster parity.
- **`nightly-redeploy.yml` / `redeploy.sh` — superseded.** Marked as such (README
  note); left dormant (already skip-guarded) or removed during implementation.

## 10. Risks & open questions

1. **Talos + dind buildx (primary risk).** Privileged dind on Talos needs the ARC
   namespace PSA to allow `privileged`. Mitigation: Phase 1 does **not** depend on
   ARC (deploys the current amd64 image with a temporary amd64 nodeSelector), so a
   running Dev env is never blocked on this. Fallbacks: rootless BuildKit / QEMU
   stopgap.
2. **`meridian-bot` TLS trust flag.** Confirm the bot can pin a CA or skip verify
   for the self-signed realm cert (§5.2). If not, adjust the TLS bootstrap to emit
   a bot-consumable CA.
3. **NodePort reachability.** Assumes node IPs reachable from where external tests
   run (true on-LAN). Off-LAN raw TCP would need Cloudflare Spectrum, out of scope.
4. **Cluster capacity.** Three realms + two persistent MariaDB StatefulSets +
   load pods across 3 NUC + 4 arm nodes — expected fine at homelab scale; validate
   Prod replica sizing against real usage.

## 11. Delivery phases

Scope covers **all three realms**; delivery is ordered to get value early and
isolate the dind risk.

1. **Phase 1 — Dev realm live.** Chart extensions (§5) + `meridian-dev`
   Application + TLS bootstrap; deploy the current amd64 image with a temporary
   amd64 nodeSelector. ⇒ a stable hosted Dev env with no ARC dependency.
2. **Phase 2 — Native multi-arch CD.** ARC + `cd.yml` rework + `bot` image +
   auto-restart. ⇒ drop the amd64 pin; pods schedule on both arches.
3. **Phase 3 — PTR + Prod realms.** `ptr`/`prod` overlays (Longhorn StatefulSet),
   their Applications (PTR auto, Prod manual), branch creation + promotion wiring.
4. **Phase 4 — Concurrency harness.** `add-users` + `loadtest` Jobs, `loadtest.sh`,
   docs. Validate an in-cluster run and an external NodePort connect.

## 12. Acceptance criteria

- `dev`/`ptr`/`main` branches exist; pushing to each publishes multi-arch
  (amd64+arm64) `authd`/`worldd`/`bot` images with the immutable + moving tags,
  cosign-signed + SBOM-attested on the manifest-list digest.
- ArgoCD shows `meridian-dev` and `meridian-ptr` **Synced/Healthy** (auto) and
  `meridian-prod` registered and syncable **manually**; each realm's authd reaches
  its own MariaDB (ephemeral for Dev, Longhorn-persistent for PTR/Prod).
- A push to `dev` results in the Dev pods running the new bits (auto-restart);
  a `dev→ptr` merge does the same for PTR; a `ptr→main` merge leaves Prod pending
  a manual sync.
- Each realm's authd/worldd are reachable externally on their NodePorts, and an
  in-cluster `loadtest` Job drives ≥ the target concurrent bot sessions across
  both arch pools.
- `run-local.sh` still brings up a working local stack unchanged.
