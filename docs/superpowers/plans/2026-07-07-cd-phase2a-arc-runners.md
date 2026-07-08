# CD Phase 2a — ARC Self-Hosted Runners Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up GitHub Actions self-hosted runners on the Talos cluster — the ARC controller plus two arch-pinned autoscaling runner scale sets (`meridian-amd64` on the NUCs, `meridian-arm64` on the arm64 Talos workers), authenticated by a GitHub App, building via dind — all GitOps-managed by ArgoCD and proven by a smoke workflow that builds a container natively on each arch.

**Architecture:** GitHub's official ARC (`gha-runner-scale-set-controller` + two `gha-runner-scale-set` releases, OCI Helm charts) deployed as ArgoCD Applications under `deploy/gitops/apps/` (owned by the existing `meridian-root` app-of-apps). Controller in ns `arc-systems`; runners in ns `arc-runners` (labeled PSA=privileged so the dind sidecar can run — the cluster already permits privileged, e.g. longhorn). Runner sets pin to arch via `nodeSelector` (no tolerations needed; the one control-plane-tainted arm node is repelled automatically). GitHub App credentials live in a hand-created Secret (not in git).

**Tech Stack:** ArgoCD v3.2.1 (OCI Helm + multi-source), Actions Runner Controller (gha-runner-scale-set), Docker-in-Docker/buildx, Talos Kubernetes, Helm 4, GitHub App auth.

**Branch:** All work commits to `dev` (per `CLAUDE.md`). Verify `git branch --show-current` → `dev`.

**Reference:** Design spec [2026-07-07-cd-hosted-realms-design.md](../specs/2026-07-07-cd-hosted-realms-design.md) §6.1 (ARC), §10.1 (Talos/dind risk — retired: cluster permits privileged). This plan is **Phase 2a**; Phase 2b (multi-arch `cd.yml` rework + bot image + auto-restart) builds on the runners this plan delivers.

**Cluster facts (verified):** no ARC installed; `longhorn-system` runs privileged (PSA=privileged permitted); nodes — amd64 `nuc1..3` (untainted), arm64 `talos-*` (3 untainted workers + 1 control-plane-tainted). ArgoCD `meridian-root` already watches `deploy/gitops/apps/` (recurse:false), so new Application manifests there auto-sync.

---

## File Structure

**Created:**
- `deploy/gitops/runners/controller-values.yaml` — Helm values for the ARC controller.
- `deploy/gitops/runners/amd64-values.yaml` — values for the amd64 runner scale set.
- `deploy/gitops/runners/arm64-values.yaml` — values for the arm64 runner scale set.
- `deploy/gitops/apps/arc-controller.yaml` — ArgoCD Application: the controller (sync-wave 0).
- `deploy/gitops/apps/arc-runner-amd64.yaml` — ArgoCD Application: amd64 scale set (sync-wave 1).
- `deploy/gitops/apps/arc-runner-arm64.yaml` — ArgoCD Application: arm64 scale set (sync-wave 1).
- `.github/workflows/arc-smoke.yml` — workflow_dispatch smoke: a dind buildx build on each runner label.
- `docs/ops/arc-runners.md` — runbook: GitHub App creation, Secret creation, verification, operations.

**Not committed (hand-created live):** the `arc-runners/arc-github-app` Secret (GitHub App credentials).

**Test harness:** values render with `helm template oci://…` against the pinned charts; Application manifests with `kubectl apply --dry-run=client`; live registration with `kubectl`; end-to-end with the smoke workflow via `gh`.

---

## Task 1: Pin the ARC chart versions

**Files:** none yet (records versions used by later tasks).

- [ ] **Step 1: Query the current published chart versions**

Run:
```bash
helm show chart oci://ghcr.io/actions/actions-runner-controller-charts/gha-runner-scale-set-controller 2>&1 | grep -E '^version:'
helm show chart oci://ghcr.io/actions/actions-runner-controller-charts/gha-runner-scale-set 2>&1 | grep -E '^version:'
```
Expected: two `version: X.Y.Z` lines (the two charts are released in lockstep — the versions should match, e.g. `0.9.3`). Record that version as `<ARC_VERSION>` for use in Tasks 2–3.

- [ ] **Step 2: Confirm the charts pull**

Run:
```bash
helm template t oci://ghcr.io/actions/actions-runner-controller-charts/gha-runner-scale-set-controller --version <ARC_VERSION> >/dev/null && echo "controller chart OK"
```
Expected: `controller chart OK` (chart pulls + templates with defaults).

No commit (this task only pins the version string used below).

---

## Task 2: Runner Helm values

**Files:**
- Create: `deploy/gitops/runners/controller-values.yaml`, `deploy/gitops/runners/amd64-values.yaml`, `deploy/gitops/runners/arm64-values.yaml`
- Test: `helm template oci://…` with each values file

- [ ] **Step 1: Write `deploy/gitops/runners/controller-values.yaml`**

```yaml
# SPDX-License-Identifier: Apache-2.0
# ARC controller (gha-runner-scale-set-controller). Pin to amd64 for a stable home.
nodeSelector:
  kubernetes.io/arch: amd64
```

- [ ] **Step 2: Write `deploy/gitops/runners/amd64-values.yaml`**

```yaml
# SPDX-License-Identifier: Apache-2.0
# amd64 runner scale set. runs-on label = runnerScaleSetName. dind for buildx.
githubConfigUrl: https://github.com/kwilliams312/project_meridian
githubConfigSecret: arc-github-app        # pre-created Secret (GitHub App creds)
runnerScaleSetName: meridian-amd64
minRunners: 0
maxRunners: 3
containerMode:
  type: dind
# ArgoCD + `helm template` cannot run the chart's controller-SA `lookup`, so name
# it explicitly: SA = <controller release>-gha-rs-controller, and the controller
# ArgoCD Application (Task 3) is named `arc-controller`.
controllerServiceAccount:
  name: arc-controller-gha-rs-controller
  namespace: arc-systems
template:
  spec:
    nodeSelector:
      kubernetes.io/arch: amd64
```

- [ ] **Step 3: Write `deploy/gitops/runners/arm64-values.yaml`**

```yaml
# SPDX-License-Identifier: Apache-2.0
# arm64 runner scale set. Schedules on the untainted arm64 Talos workers.
githubConfigUrl: https://github.com/kwilliams312/project_meridian
githubConfigSecret: arc-github-app
runnerScaleSetName: meridian-arm64
minRunners: 0
maxRunners: 3
containerMode:
  type: dind
# See amd64-values.yaml — the controller-SA lookup must be named explicitly.
controllerServiceAccount:
  name: arc-controller-gha-rs-controller
  namespace: arc-systems
template:
  spec:
    nodeSelector:
      kubernetes.io/arch: arm64
```

- [ ] **Step 4: Validate each values file renders against the chart**

Run (substitute the pinned `<ARC_VERSION>`; a dummy secret name is fine for templating):
```bash
helm template amd64 oci://ghcr.io/actions/actions-runner-controller-charts/gha-runner-scale-set \
  --version <ARC_VERSION> --namespace arc-runners -f deploy/gitops/runners/amd64-values.yaml >/dev/null && echo "amd64 values OK"
helm template arm64 oci://ghcr.io/actions/actions-runner-controller-charts/gha-runner-scale-set \
  --version <ARC_VERSION> --namespace arc-runners -f deploy/gitops/runners/arm64-values.yaml >/dev/null && echo "arm64 values OK"
helm template arc-controller oci://ghcr.io/actions/actions-runner-controller-charts/gha-runner-scale-set-controller \
  --version <ARC_VERSION> --namespace arc-systems -f deploy/gitops/runners/controller-values.yaml >/dev/null && echo "controller values OK"
```
Expected: `amd64 values OK`, `arm64 values OK`, `controller values OK` (no template errors — proves the value keys are valid for the chart).

- [ ] **Step 5: Commit**

```bash
git add deploy/gitops/runners/
git commit -m "feat(arc): runner scale-set + controller Helm values (amd64/arm64, dind)"
```

---

## Task 3: ArgoCD Applications for ARC (controller + two runner sets)

**Files:**
- Create: `deploy/gitops/apps/arc-controller.yaml`, `deploy/gitops/apps/arc-runner-amd64.yaml`, `deploy/gitops/apps/arc-runner-arm64.yaml`
- Test: `kubectl apply --dry-run=client`

Uses the ArgoCD **multi-source** pattern: source 1 = this git repo (as `$values` for the values file), source 2 = the OCI chart. Sync-wave 0 for the controller (installs CRDs), wave 1 for the runner sets.

- [ ] **Step 1: Write `deploy/gitops/apps/arc-controller.yaml`** (replace `<ARC_VERSION>`)

```yaml
# SPDX-License-Identifier: Apache-2.0
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: arc-controller
  namespace: argocd
  annotations:
    argocd.argoproj.io/sync-wave: "0"
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  sources:
    - repoURL: https://github.com/kwilliams312/project_meridian.git
      targetRevision: dev
      ref: values
    - repoURL: ghcr.io/actions/actions-runner-controller-charts
      chart: gha-runner-scale-set-controller
      targetRevision: <ARC_VERSION>
      helm:
        valueFiles:
          - $values/deploy/gitops/runners/controller-values.yaml
  destination:
    server: https://kubernetes.default.svc
    namespace: arc-systems
  syncPolicy:
    automated:
      prune: true
      selfHeal: true
    syncOptions:
      - CreateNamespace=true
      - ServerSideApply=true
```

- [ ] **Step 2: Write `deploy/gitops/apps/arc-runner-amd64.yaml`** (replace `<ARC_VERSION>`)

```yaml
# SPDX-License-Identifier: Apache-2.0
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: arc-runner-amd64
  namespace: argocd
  annotations:
    argocd.argoproj.io/sync-wave: "1"
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  sources:
    - repoURL: https://github.com/kwilliams312/project_meridian.git
      targetRevision: dev
      ref: values
    - repoURL: ghcr.io/actions/actions-runner-controller-charts
      chart: gha-runner-scale-set
      targetRevision: <ARC_VERSION>
      helm:
        valueFiles:
          - $values/deploy/gitops/runners/amd64-values.yaml
  destination:
    server: https://kubernetes.default.svc
    namespace: arc-runners
  syncPolicy:
    automated:
      prune: true
      selfHeal: true
    syncOptions:
      - CreateNamespace=true
      - ServerSideApply=true
    managedNamespaceMetadata:
      labels:
        pod-security.kubernetes.io/enforce: privileged
```

- [ ] **Step 3: Write `deploy/gitops/apps/arc-runner-arm64.yaml`**

Identical to Step 2 but with `name: arc-runner-arm64` and `valueFiles: [$values/deploy/gitops/runners/arm64-values.yaml]`. Repeat the full manifest:

```yaml
# SPDX-License-Identifier: Apache-2.0
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: arc-runner-arm64
  namespace: argocd
  annotations:
    argocd.argoproj.io/sync-wave: "1"
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  sources:
    - repoURL: https://github.com/kwilliams312/project_meridian.git
      targetRevision: dev
      ref: values
    - repoURL: ghcr.io/actions/actions-runner-controller-charts
      chart: gha-runner-scale-set
      targetRevision: <ARC_VERSION>
      helm:
        valueFiles:
          - $values/deploy/gitops/runners/arm64-values.yaml
  destination:
    server: https://kubernetes.default.svc
    namespace: arc-runners
  syncPolicy:
    automated:
      prune: true
      selfHeal: true
    syncOptions:
      - CreateNamespace=true
      - ServerSideApply=true
    managedNamespaceMetadata:
      labels:
        pod-security.kubernetes.io/enforce: privileged
```

- [ ] **Step 4: Validate the manifests parse against the live ArgoCD CRD**

Run:
```bash
kubectl apply --dry-run=client -f deploy/gitops/apps/arc-controller.yaml \
  -f deploy/gitops/apps/arc-runner-amd64.yaml -f deploy/gitops/apps/arc-runner-arm64.yaml
```
Expected: three `application.argoproj.io/... created (dry run)` lines, no schema errors.

- [ ] **Step 5: Commit**

```bash
git add deploy/gitops/apps/arc-controller.yaml deploy/gitops/apps/arc-runner-amd64.yaml deploy/gitops/apps/arc-runner-arm64.yaml
git commit -m "feat(arc): ArgoCD Applications for controller + amd64/arm64 runner sets"
```

---

## Task 4: ARC dind smoke workflow

**Files:**
- Create: `.github/workflows/arc-smoke.yml`
- Test: YAML parse + `actionlint` if available

- [ ] **Step 1: Write `.github/workflows/arc-smoke.yml`**

```yaml
# SPDX-License-Identifier: Apache-2.0
# Proves the self-hosted runners work on BOTH arches and that dind/buildx can
# build a container. Manual trigger (workflow_dispatch). Each job runs on its
# arch's runner label and builds a trivial image natively (no QEMU).
name: arc-smoke

on:
  workflow_dispatch: {}

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        runner: [meridian-amd64, meridian-arm64]
    runs-on: ${{ matrix.runner }}
    steps:
      - name: Report arch
        run: uname -m

      - name: Set up Docker Buildx
        uses: docker/setup-buildx-action@v3

      - name: Build a trivial image natively (proves dind + buildx)
        run: |
          printf 'FROM alpine:3.20\nRUN uname -m > /arch.txt\n' > Dockerfile.smoke
          docker buildx build -f Dockerfile.smoke --load -t arc-smoke:${{ matrix.runner }} .
          docker run --rm arc-smoke:${{ matrix.runner }} cat /arch.txt
```

- [ ] **Step 2: Validate the workflow YAML**

Run:
```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/arc-smoke.yml')); print('arc-smoke.yml valid')"
command -v actionlint >/dev/null && actionlint .github/workflows/arc-smoke.yml || echo "actionlint not installed (skip)"
```
Expected: `arc-smoke.yml valid` (and actionlint clean if installed).

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/arc-smoke.yml
git commit -m "feat(arc): dind smoke workflow (native build on each arch runner)"
```

---

## Task 5: GitHub App + runner runbook

**Files:**
- Create: `docs/ops/arc-runners.md`

- [ ] **Step 1: Write `docs/ops/arc-runners.md`**

````markdown
# Self-hosted runners (ARC) — runbook

GitOps-managed GitHub Actions runners on the Talos cluster (Phase 2a). ARC
controller in `arc-systems`; two dind runner scale sets in `arc-runners`
(`meridian-amd64` on the NUCs, `meridian-arm64` on the arm64 Talos workers).
Consumed by `cd.yml` in Phase 2b for native multi-arch builds.

## Prerequisite: create the GitHub App (one-time, manual)

1. GitHub → Settings → Developer settings → **GitHub Apps → New GitHub App**.
   - Name: e.g. `meridian-arc`. Homepage URL: the repo URL. Uncheck "Active"
     under Webhook (ARC needs no webhook).
   - **Repository permissions:** `Administration: Read and write`,
     `Metadata: Read-only`. (For repo-level runners these suffice.)
   - Create the app. Note the **App ID**.
2. **Generate a private key** (bottom of the app page) → downloads a `.pem`.
3. **Install** the app: app page → Install App → your account → only the
   `project_meridian` repo. After install, the URL shows the **Installation ID**
   (`.../installations/<INSTALLATION_ID>`).

## Create the runner auth Secret (not in git)

```bash
kubectl create namespace arc-runners --dry-run=client -o yaml | kubectl apply -f -
kubectl -n arc-runners create secret generic arc-github-app \
  --from-literal=github_app_id=<APP_ID> \
  --from-literal=github_app_installation_id=<INSTALLATION_ID> \
  --from-file=github_app_private_key=<path-to-downloaded>.pem
```

## Deploy (GitOps)

The `arc-controller` / `arc-runner-amd64` / `arc-runner-arm64` Applications live
under `deploy/gitops/apps/` and are auto-synced by `meridian-root`. Force an
immediate reconcile:

```bash
kubectl -n argocd annotate application meridian-root argocd.argoproj.io/refresh=hard --overwrite
```

## Verify

```bash
kubectl -n arc-systems get pods                       # controller Running
kubectl -n arc-runners get autoscalingrunnerset       # meridian-amd64 / -arm64
kubectl -n arc-runners get pods                        # listener pods Running
# Runners show as Idle on GitHub: repo → Settings → Actions → Runners.
gh workflow run arc-smoke.yml --ref dev               # end-to-end dind smoke
```

Runners are ephemeral and scale 0→N on demand (`minRunners: 0`), so no idle
runner pods until a job is queued — the **listener** pod per set is the
always-on component.

## Operate

- Bump ARC: edit `targetRevision` in the three `apps/arc-*.yaml`, commit to `dev`.
- Change capacity/arch: edit `deploy/gitops/runners/*-values.yaml`.
- Rotate creds: recreate the `arc-github-app` Secret; restart the listener pods.
````

- [ ] **Step 2: Commit**

```bash
git add docs/ops/arc-runners.md
git commit -m "docs(ops): ARC runners runbook (GitHub App setup + verify)"
git push origin dev
```

---

## Task 6: Deploy + verify runner registration (LIVE — needs the GitHub App)

**Prereq:** Tasks 1–5 committed and pushed; the operator has created the GitHub App and provided **App ID**, **Installation ID**, and the **private-key `.pem`**.

- [ ] **Step 1: Create the auth Secret** (per the runbook, with the real values)

```bash
kubectl create namespace arc-runners --dry-run=client -o yaml | kubectl apply -f -
kubectl -n arc-runners create secret generic arc-github-app \
  --from-literal=github_app_id=<APP_ID> \
  --from-literal=github_app_installation_id=<INSTALLATION_ID> \
  --from-file=github_app_private_key=<path>.pem
```
Expected: `secret/arc-github-app created`.

- [ ] **Step 2: Trigger the app-of-apps sync + wait for the controller**

```bash
kubectl -n argocd annotate application meridian-root argocd.argoproj.io/refresh=hard --overwrite
kubectl -n arc-systems rollout status deploy -l app.kubernetes.io/name=gha-runner-scale-set-controller --timeout=180s 2>/dev/null \
  || kubectl -n arc-systems get pods
```
Expected: the controller Deployment becomes Available (or its pod is `1/1 Running`).

- [ ] **Step 3: Verify both scale sets registered**

```bash
kubectl -n arc-runners get autoscalingrunnerset
kubectl -n arc-runners get pods            # a listener pod per set, Running
```
Expected: `meridian-amd64` and `meridian-arm64` AutoscalingRunnerSets exist; each has a `...-listener` pod `1/1 Running`. If a listener is CrashLoopBackOff, check `kubectl -n arc-runners logs <listener>` — almost always a Secret key / App-installation mismatch.

- [ ] **Step 4: Confirm the ArgoCD apps are Healthy**

```bash
for a in arc-controller arc-runner-amd64 arc-runner-arm64; do
  printf "%-18s " "$a"; kubectl -n argocd get application "$a" -o jsonpath='{.status.sync.status}/{.status.health.status}{"\n"}'
done
```
Expected: each prints `Synced/Healthy`.

---

## Task 7: End-to-end dind smoke on both arches (LIVE)

- [ ] **Step 1: Run the smoke workflow**

```bash
gh workflow run arc-smoke.yml --ref dev
sleep 8
RID=$(gh run list --workflow=arc-smoke.yml --branch dev --limit 1 --json databaseId --jq '.[0].databaseId')
gh run watch "$RID" --exit-status --interval 20
```
Expected: the run goes green. (Watching may take a couple minutes while runners scale 0→1 on each arch.)

- [ ] **Step 2: Confirm each arch built natively**

```bash
gh run view "$RID" --json jobs --jq '.jobs[] | {name, conclusion}'
gh run view "$RID" --log | grep -E 'x86_64|aarch64|/arch.txt' | head
```
Expected: both matrix jobs `success`; logs show `x86_64` on the amd64 job and `aarch64` on the arm64 job (native, no QEMU) — proving dind buildx works on each arch.

- [ ] **Step 3: Record completion**

Append a short "verified <date>: both runner sets register, dind smoke green (x86_64 + aarch64)" note to `docs/ops/arc-runners.md`, commit, and push:
```bash
git add docs/ops/arc-runners.md
git commit -m "docs(ops): record ARC runners go-live (dind smoke green both arches)"
git push origin dev
```

---

## Self-Review

**Spec coverage (§6.1):** ARC controller + two arch-pinned scale sets → Tasks 2–3 ✓; GitHub App auth → Task 5/6 (Secret) ✓; dind builds → dind containerMode (Task 2) + smoke (Tasks 4/7) ✓; GitOps-managed → ArgoCD Applications under the existing app-of-apps (Task 3) ✓; Talos privileged → `managedNamespaceMetadata` PSA=privileged on `arc-runners` (Task 3) ✓.

**Deferred to Phase 2b (correctly out of scope):** the `cd.yml` multi-arch rework, the `bot` image, manifest-list merge + cosign/SBOM, per-branch tags, ArgoCD auto-restart, and dropping the dev overlay's amd64 nodeSelector. This plan only delivers *working runners*.

**Placeholder scan:** `<ARC_VERSION>`, `<APP_ID>`, `<INSTALLATION_ID>`, and the `.pem` path are intentional runtime values, each resolved by an explicit step (Task 1 pins the version; the GitHub App fields come from the operator via Task 5's runbook). No vague "TBD"/"add error handling".

**Consistency:** `runnerScaleSetName` values (`meridian-amd64` / `meridian-arm64`) match the `runs-on` labels in `arc-smoke.yml` (Task 4) and the smoke assertions (Task 7). The Secret name `arc-github-app` matches `githubConfigSecret` in both runner values files and the create-secret commands. Namespaces `arc-systems`/`arc-runners` are consistent across Applications, runbook, and verification.
