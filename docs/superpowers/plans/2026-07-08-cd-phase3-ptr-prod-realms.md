# CD Phase 3 — PTR + Prod Realms Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
>
> ⚠️ **Human-gated:** Tasks 4–5 stand up real persistent realms and (for Prod) are deliberately manual. Do NOT execute the promotions / Prod sync autonomously — they are the operator's call (design §3: Prod is manual-sync).

**Goal:** Stand up the **PTR** (`meridian-ptr`, tracks `ptr`) and **Prod** (`meridian-prod`, tracks `main`) realms alongside the live Dev realm — both with a **persistent** Longhorn-backed MariaDB — using the same chart. PTR auto-syncs and auto-restarts like Dev; Prod is **manual-sync** (the human gate). Wire the branch-promotion flow `dev → ptr → main`.

**Architecture:** Two more values overlays (`realms/ptr`, `realms/prod`) + two ArgoCD `Application`s under the existing `meridian-root` app-of-apps. The chart's `mariadb.persistence.enabled=true` path (already built + unit-rendered in Phase 1) gives each a StatefulSet + PVC. The realms come up by **promoting the branch**: `dev → ptr` makes `ptr` carry the chart + overlays so the `meridian-ptr` app (tracking `ptr`) syncs; likewise `ptr → main` for Prod, which then requires a deliberate manual sync. cd.yml already publishes `:ptr`/`:prod` multi-arch on those branch pushes.

**Tech Stack:** ArgoCD, Helm (existing `meridian` chart 0.2.0), Longhorn (default StorageClass), the multi-arch `cd.yml` from Phase 2b.

**Branch:** author on `dev`. **Reference:** spec §3 (realm matrix), §5.1 (persistence). Realm matrix (spec §3):

| Realm | Branch | NS | MariaDB | Moving tag | NodePorts authd/worldd | Replicas | Sync |
|-------|--------|----|---------|-----------|------------------------|----------|------|
| Dev (live) | `dev` | `meridian-dev` | ephemeral emptyDir | `:dev` + restart | 31710/31720 | 1 | auto |
| **PTR** | `ptr` | `meridian-ptr` | **StatefulSet + Longhorn PVC** | `:ptr` + restart | 31730/31740 | 2 | auto |
| **Prod** | `main` | `meridian-prod` | **StatefulSet + Longhorn PVC** | `:prod` (+`:latest`) | 31750/31760 | 3 | **manual** |

**Prereq facts:** cd.yml publishes `:ptr`/`:prod` multi-arch (Phase 2b). Longhorn is the default StorageClass. The `ptr` branch already exists (created in the Phase-0 bootstrap at `bb2fe25`) but is stale — it must be fast-forwarded/merged from `dev`. `main` currently lacks the CD infra (it arrives via promotion).

---

## File Structure

**Created:**
- `deploy/gitops/realms/ptr/values.yaml` — PTR overlay (persistent DB, `:ptr`, 2 replicas, NodePorts 317x).
- `deploy/gitops/realms/prod/values.yaml` — Prod overlay (persistent DB, `:prod`, 3 replicas, NodePorts 317x).
- `deploy/gitops/apps/meridian-ptr.yaml` — Application, tracks `ptr`, auto-sync + ignoreDifferences.
- `deploy/gitops/apps/meridian-prod.yaml` — Application, tracks `main`, **manual sync** + ignoreDifferences.

**Modified:**
- `.github/workflows/cd.yml` — restart job targets **daemons only** (exclude the persistent MariaDB from rollout-restart — see rationale below).
- `deploy/gitops/rbac/meridian-restarter.yaml` — add the `meridian-ptr` Role+Binding (deferred from Phase 2b).

**Rationale — daemons-only restart:** the Phase-2b restart used `-l app.kubernetes.io/part-of=meridian`, which also rolls MariaDB. For Dev (ephemeral) that reseeds — tolerable. For PTR/Prod (persistent PVC) restarting MariaDB is pure downtime with no benefit (initdb doesn't re-run on a non-empty volume; schema migrations are a separate future concern). So restart only `component in (authd, worldd)`.

---

## Task 1: PTR + Prod realm overlays

**Files:** Create `deploy/gitops/realms/ptr/values.yaml`, `deploy/gitops/realms/prod/values.yaml`. Test: `helm template` + `kubectl --dry-run`.

- [ ] **Step 1: `deploy/gitops/realms/ptr/values.yaml`:**

```yaml
# SPDX-License-Identifier: Apache-2.0
# PTR realm (meridian-ptr) — persistent Longhorn MariaDB, :ptr multi-arch images,
# self-signed TLS, NodePort. Pre-prod test realm; auto-synced from the ptr branch.
image:
  tag: "ptr"
  pullPolicy: Always
authd:
  replicaCount: 2
  service: { type: NodePort, nodePort: 31730 }
  probes: { type: tcp }
worldd:
  replicaCount: 2
  service: { type: NodePort, nodePort: 31740 }
  probes: { type: tcp }
tls:
  mode: selfSignedInit
db:
  password: "meridian"
mariadb:
  enabled: true
  image: { tag: "ptr" }
  persistence:
    enabled: true
    storageClass: "longhorn"
    size: 8Gi
```

- [ ] **Step 2: `deploy/gitops/realms/prod/values.yaml`:**

```yaml
# SPDX-License-Identifier: Apache-2.0
# Prod realm (meridian-prod) — persistent Longhorn MariaDB, :prod multi-arch
# images, self-signed TLS, NodePort. Manual-sync (deliberate promotion gate).
image:
  tag: "prod"
  pullPolicy: Always
authd:
  replicaCount: 3
  service: { type: NodePort, nodePort: 31750 }
  probes: { type: tcp }
worldd:
  replicaCount: 3
  service: { type: NodePort, nodePort: 31760 }
  probes: { type: tcp }
tls:
  mode: selfSignedInit
db:
  password: "meridian"
mariadb:
  enabled: true
  image: { tag: "prod" }
  persistence:
    enabled: true
    storageClass: "longhorn"
    size: 20Gi
```

NOTE: both realms use `tls.mode: selfSignedInit` for now (matches Dev; clients skip-verify). Migrating PTR/Prod to real certs via `tls.mode: existingSecret` is a follow-up (needs cert provisioning; no cert-manager in-cluster yet).

- [ ] **Step 3: Validate both render + dry-run** (persistence renders a StatefulSet):

```bash
for r in ptr prod; do
  echo "== $r =="
  helm template meridian-$r deploy/helm/meridian --namespace meridian-$r \
    -f deploy/gitops/realms/$r/values.yaml | kubectl apply --dry-run=client -f - 2>&1 | grep -E 'statefulset|deployment|service' | head
done
```
Expected: each shows a `statefulset.apps/meridian-$r-mariadb`, `deployment.apps/meridian-$r-authd`/`-worldd`, and the three services — all `created (dry run)`.

- [ ] **Step 4: Commit**

```bash
git add deploy/gitops/realms/ptr/ deploy/gitops/realms/prod/
git commit -m "feat(gitops): PTR + Prod realm overlays (persistent Longhorn MariaDB)"
```

---

## Task 2: PTR + Prod ArgoCD Applications + PTR restart RBAC

**Files:** Create `deploy/gitops/apps/meridian-ptr.yaml`, `deploy/gitops/apps/meridian-prod.yaml`; modify `deploy/gitops/rbac/meridian-restarter.yaml`. Test: `kubectl --dry-run`.

- [ ] **Step 1: `deploy/gitops/apps/meridian-ptr.yaml`** (auto-sync, tracks `ptr`):

```yaml
# SPDX-License-Identifier: Apache-2.0
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: meridian-ptr
  namespace: argocd
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  source:
    repoURL: https://github.com/kwilliams312/project_meridian.git
    targetRevision: ptr
    path: deploy/helm/meridian
    helm:
      valueFiles:
        - ../../gitops/realms/ptr/values.yaml
  destination:
    server: https://kubernetes.default.svc
    namespace: meridian-ptr
  ignoreDifferences:
    - group: apps
      kind: Deployment
      jqPathExpressions:
        - '.spec.template.metadata.annotations."kubectl.kubernetes.io/restartedAt"'
  syncPolicy:
    automated:
      prune: true
      selfHeal: true
    syncOptions:
      - CreateNamespace=true
      - ServerSideApply=true
```

- [ ] **Step 2: `deploy/gitops/apps/meridian-prod.yaml`** (MANUAL sync — no `automated:` block — tracks `main`):

```yaml
# SPDX-License-Identifier: Apache-2.0
# Prod realm — MANUAL sync (design §3): promoting to production is a deliberate
# operator action (`argocd app sync meridian-prod`). No automated syncPolicy.
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: meridian-prod
  namespace: argocd
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  source:
    repoURL: https://github.com/kwilliams312/project_meridian.git
    targetRevision: main
    path: deploy/helm/meridian
    helm:
      valueFiles:
        - ../../gitops/realms/prod/values.yaml
  destination:
    server: https://kubernetes.default.svc
    namespace: meridian-prod
  ignoreDifferences:
    - group: apps
      kind: Deployment
      jqPathExpressions:
        - '.spec.template.metadata.annotations."kubectl.kubernetes.io/restartedAt"'
  syncPolicy:
    syncOptions:
      - CreateNamespace=true
      - ServerSideApply=true
```

- [ ] **Step 3: Add the `meridian-ptr` restart Role+Binding** to `deploy/gitops/rbac/meridian-restarter.yaml` (append, mirroring the `meridian-dev` pair, same runner SA `meridian-amd64-gha-rs-no-permission`):

```yaml
---
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: meridian-restarter
  namespace: meridian-ptr
rules:
  - apiGroups: ["apps"]
    resources: ["deployments"]
    verbs: ["get", "list", "patch"]
---
apiVersion: rbac.authorization.k8s.io/v1
kind: RoleBinding
metadata:
  name: meridian-restarter
  namespace: meridian-ptr
subjects:
  - kind: ServiceAccount
    name: meridian-amd64-gha-rs-no-permission
    namespace: arc-runners
roleRef:
  kind: Role
  name: meridian-restarter
  apiGroup: rbac.authorization.k8s.io
```
(No Prod restart RBAC: Prod does not auto-restart — cd.yml's restart job is `dev`/`ptr` only, and promotion to Prod is manual.)

- [ ] **Step 4: Validate**

```bash
kubectl apply --dry-run=client -f deploy/gitops/apps/meridian-ptr.yaml -f deploy/gitops/apps/meridian-prod.yaml
kubectl apply --dry-run=client -f deploy/gitops/rbac/meridian-restarter.yaml
```
Expected: both Applications + all 4 RBAC objects `created (dry run)`. (The `meridian-ptr` Role's namespace doesn't exist yet — dry-run=client validates schema only, no error.)

- [ ] **Step 5: Commit**

```bash
git add deploy/gitops/apps/meridian-ptr.yaml deploy/gitops/apps/meridian-prod.yaml deploy/gitops/rbac/meridian-restarter.yaml
git commit -m "feat(gitops): PTR (auto) + Prod (manual-sync) Applications + PTR restart RBAC"
```

---

## Task 3: Restrict cd.yml auto-restart to the daemons

**Files:** modify `.github/workflows/cd.yml`. Test: YAML parse.

- [ ] **Step 1: In the `restart` job**, change the rollout-restart to daemons only:

Replace:
```yaml
          kubectl -n "$NS" rollout restart deployment -l app.kubernetes.io/part-of=meridian
          kubectl -n "$NS" rollout status deployment -l app.kubernetes.io/component=authd --timeout=120s
```
with:
```yaml
          # Daemons only — never rollout-restart the (persistent, in PTR/Prod)
          # MariaDB; a restart there is downtime with no benefit.
          kubectl -n "$NS" rollout restart deployment -l 'app.kubernetes.io/part-of=meridian,app.kubernetes.io/component in (authd,worldd)'
          kubectl -n "$NS" rollout status deployment -l app.kubernetes.io/component=authd --timeout=120s
```

- [ ] **Step 2: Validate + commit**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/cd.yml')); print('cd.yml valid')"
git add .github/workflows/cd.yml
git commit -m "fix(cd): auto-restart daemons only (never the persistent MariaDB)"
```

- [ ] **Step 3: Push everything** (this cd.yml change fires a build on `dev`; it validates the daemons-only restart against the live Dev realm):

```bash
git push origin dev
```
Then confirm the run's `restart` job stays green and Dev's MariaDB pod is NOT rolled (its AGE is unchanged after the run):
```bash
RID=$(gh run list --workflow=cd.yml --branch dev --limit 1 --json databaseId --jq '.[0].databaseId')
gh run watch "$RID" --exit-status --interval 30
kubectl -n meridian-dev get pods -l app.kubernetes.io/component=mariadb --no-headers   # AGE should predate the run
```

---

## Task 4 (HUMAN-GATED, LIVE): promote to PTR + verify

> Operator action — the branch promotion is deliberate. ArgoCD `meridian-root` (on `dev`) will have created the `meridian-ptr`/`meridian-prod` Applications after Task 2's push; `meridian-ptr` will show `ComparisonError` until `ptr` carries the chart.

- [ ] **Step 1: Promote `dev → ptr`** (fast-forward the stale `ptr` branch to `dev`):

```bash
git switch ptr && git merge --ff-only dev && git push origin ptr && git switch dev
```
(If `ptr` has diverged and `--ff-only` refuses, use a normal merge `git merge dev` and resolve — `ptr` should be a strict ancestor here.)

- [ ] **Step 2: The `ptr` push fires cd.yml → publishes `:ptr` multi-arch; `meridian-ptr` (auto-sync) then deploys.** Watch:

```bash
gh run watch "$(gh run list --workflow=cd.yml --branch ptr --limit 1 --json databaseId --jq '.[0].databaseId')" --exit-status --interval 30
kubectl -n argocd get application meridian-ptr -o jsonpath='{.status.sync.status}/{.status.health.status}{"\n"}'
kubectl -n meridian-ptr get pods -o wide
```
Expected: cd green; `meridian-ptr` Synced/Healthy; 2×authd + 2×worldd + 1×mariadb (StatefulSet, PVC-backed) Running across arch pools.

- [ ] **Step 3: Verify persistence + reachability**

```bash
kubectl -n meridian-ptr get pvc                              # a bound Longhorn PVC for mariadb
NODE_IP=$(kubectl get node -o jsonpath='{.items[0].status.addresses[?(@.type=="InternalIP")].address}')
echo | openssl s_client -connect "${NODE_IP}:31730" 2>/dev/null | openssl x509 -noout -subject   # authd
```
Expected: a `Bound` PVC; `subject=CN=meridian-ptr-authd`.

---

## Task 5 (HUMAN-GATED, LIVE): promote to Prod + manual sync

> Prod is the deliberate gate. Do this only when PTR looks good.

- [ ] **Step 1: Promote `ptr → main`:**

```bash
git switch main && git merge --ff-only ptr && git push origin main && git switch dev
```
This makes `main` carry the chart + prod overlay; cd.yml publishes `:prod` (+`:latest`) multi-arch. **Prod does NOT auto-deploy** (manual-sync).

- [ ] **Step 2: Manually sync Prod** (the human gate):

```bash
kubectl -n argocd get application meridian-prod -o jsonpath='{.status.sync.status}{"\n"}'   # OutOfSync
# Sync via the ArgoCD UI, or:
kubectl -n argocd patch application meridian-prod --type merge -p '{"operation":{"sync":{"revision":"main"}}}'
kubectl -n argocd get application meridian-prod -o jsonpath='{.status.sync.status}/{.status.health.status}{"\n"}'
kubectl -n meridian-prod get pods -o wide
```
Expected: after the manual sync, `meridian-prod` Synced/Healthy; 3×authd + 3×worldd + persistent mariadb Running.

- [ ] **Step 3: Record go-live** — add `docs/ops/ptr-prod-realms.md` capturing the NodePort map, the promotion commands, and the manual-Prod-sync step; commit + push to `dev`.

---

## Self-Review

**Spec coverage (§3):** PTR + Prod realms with persistent Longhorn MariaDB → Tasks 1–2 ✓; branch mapping + promotion `dev→ptr→main` → Tasks 4–5 ✓; PTR auto-sync + auto-restart / Prod manual-sync → the two apps' syncPolicy (Task 2) ✓; distinct NodePorts (31730/40, 31750/60) → overlays (Task 1) ✓; daemons-only restart for persistent DBs → Task 3 ✓.

**Deferred/flagged:** real TLS certs for PTR/Prod (`tls.mode: existingSecret`) — needs cert provisioning (no cert-manager); persistent-DB **schema migrations** (reseed-on-restart only works for the ephemeral Dev DB) — a separate concern when schema changes need to reach PTR/Prod. Both noted inline.

**Consistency:** overlay tags `:ptr`/`:prod` match cd.yml's branch→tag map (Phase 2b) and the app image tags; NodePorts match the spec matrix; the PTR restart RBAC binds the same runner SA (`meridian-amd64-gha-rs-no-permission`) proven in Phase 2b; Prod has no restart RBAC because it never auto-restarts.
