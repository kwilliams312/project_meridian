# CD Phase 2b — Native Multi-Arch CD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Rework CD to build **native multi-arch** (amd64+arm64) images on the ARC self-hosted runners for every realm branch (`dev`/`ptr`/`main`), publish them as manifest lists with cosign+SBOM provenance, retire the Phase-1 `dev-images.yml` stopgap, auto-restart the dev/ptr realms on publish, and drop the temporary amd64 nodeSelector pin so pods schedule on both arch pools.

**Architecture:** A reworked `cd.yml` runs `{image: authd, worldd, meridian-db} × {arch: amd64, arm64}` — each leg builds natively on its arch's runner (`meridian-amd64` / `meridian-arm64`), pushes a per-arch image by digest; a merge job assembles manifest lists tagged `:<short-sha>` + the moving per-branch tag (`:dev`/`:ptr`/`:prod`, plus `:latest` on `main`) and cosign-signs + SBOM-attests the merged digest. A final job (dev/ptr only) triggers a rollout restart so pods repull the moving tag; the realm Applications add an `ignoreDifferences` for the `restartedAt` annotation so ArgoCD self-heal doesn't fight it.

**Tech Stack:** GitHub Actions on ARC self-hosted runners (dind/buildx), `docker buildx imagetools`, cosign (keyless OIDC), Syft SBOM, ArgoCD, kubectl.

**Branch:** commit to `dev`. **Reference:** design spec §6.3/§6.5/§10; supersedes `dev-images.yml` (Phase-1 bootstrap). Bot image (§6.4) is **deferred to Phase 4**.

**Prereq facts:** ARC runners live (labels `meridian-amd64`/`meridian-arm64`, dind, verified). Current `dev` realm runs `:dev` amd64 images pinned to amd64 nodes (`deploy/gitops/realms/dev/values.yaml`). `meridian-db` is `mariadb:11` + COPY layers (arch-independent → trivially multi-arch).

---

## File Structure

**Created:**
- `deploy/gitops/rbac/meridian-restarter.yaml` — a ServiceAccount + Roles (in `meridian-dev`/`meridian-ptr`) letting an in-cluster runner `rollout restart` the daemons, plus the token wiring. One responsibility: least-priv restart access.

**Modified:**
- `.github/workflows/cd.yml` — full rework: per-branch triggers, multi-arch matrix on self-hosted runners, manifest-list merge, cosign+SBOM.
- `deploy/gitops/realms/dev/values.yaml` — drop the amd64 nodeSelectors (authd/worldd/mariadb).
- `deploy/gitops/apps/meridian-dev.yaml` — add `ignoreDifferences` for the `restartedAt` annotation.

**Deleted:**
- `.github/workflows/dev-images.yml` — superseded by the reworked `cd.yml`.

**Test harness:** `helm template`/`kubectl --dry-run` for the RBAC + overlay; YAML+`actionlint` for the workflow; the reworked `cd.yml` is proven live by a push that builds multi-arch, then `docker buildx imagetools inspect` confirming both arches in each manifest list, and the dev realm rescheduling across arches while staying Healthy.

---

## Task 1: Rework `cd.yml` — native multi-arch build + merge, retire `dev-images.yml`

**Files:**
- Rewrite: `.github/workflows/cd.yml`
- Delete: `.github/workflows/dev-images.yml`
- Test: YAML parse + `actionlint`

- [ ] **Step 1: Replace `.github/workflows/cd.yml` with:**

```yaml
# SPDX-License-Identifier: Apache-2.0
# Native multi-arch CD (D-37 / Phase 2b). On a push to a realm branch, build the
# server images natively on the ARC self-hosted runners (amd64 on the NUCs, arm64
# on the Talos workers), publish per-arch by digest, then merge into manifest
# lists tagged <short-sha> + the moving per-branch tag (:dev/:ptr/:prod; :latest
# on main), cosign-signed + SBOM-attested on the merged digest. dev/ptr then
# auto-restart to repull the moving tag. Supersedes dev-images.yml.
name: cd

on:
  push:
    branches: [dev, ptr, main]
    paths:
      - server/**
      - schema/**
      - deploy/docker/**
      - .github/workflows/cd.yml

permissions:
  contents: read
  packages: write
  id-token: write        # cosign keyless

env:
  REGISTRY: ghcr.io
  OWNER_REPO: kwilliams312/project_meridian

jobs:
  # ── Derive the realm tag from the branch ────────────────────────────────────
  meta:
    runs-on: meridian-amd64
    outputs:
      sha: ${{ steps.m.outputs.sha }}
      moving: ${{ steps.m.outputs.moving }}
      latest: ${{ steps.m.outputs.latest }}
    steps:
      - id: m
        run: |
          echo "sha=$(echo '${{ github.sha }}' | cut -c1-7)" >> "$GITHUB_OUTPUT"
          case "${{ github.ref_name }}" in
            dev)  echo "moving=dev"  >> "$GITHUB_OUTPUT"; echo "latest=false" >> "$GITHUB_OUTPUT" ;;
            ptr)  echo "moving=ptr"  >> "$GITHUB_OUTPUT"; echo "latest=false" >> "$GITHUB_OUTPUT" ;;
            main) echo "moving=prod" >> "$GITHUB_OUTPUT"; echo "latest=true"  >> "$GITHUB_OUTPUT" ;;
          esac

  # ── Per-arch native build → push by digest ─────────────────────────────────
  build:
    needs: meta
    strategy:
      fail-fast: false
      matrix:
        image: [authd, worldd, meridian-db]
        arch: [amd64, arm64]
    runs-on: meridian-${{ matrix.arch }}
    steps:
      - uses: actions/checkout@v4
      - uses: docker/setup-buildx-action@v3
      - uses: docker/login-action@v3
        with:
          registry: ${{ env.REGISTRY }}
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - id: dockerfile
        run: |
          case "${{ matrix.image }}" in
            authd|worldd) echo "file=deploy/docker/Dockerfile" >> "$GITHUB_OUTPUT"; echo "args=DAEMON=${{ matrix.image }}" >> "$GITHUB_OUTPUT" ;;
            meridian-db)  echo "file=deploy/docker/Dockerfile.db" >> "$GITHUB_OUTPUT"; echo "args=" >> "$GITHUB_OUTPUT" ;;
          esac
      - id: build
        uses: docker/build-push-action@v6
        with:
          context: .
          file: ${{ steps.dockerfile.outputs.file }}
          build-args: ${{ steps.dockerfile.outputs.args }}
          platforms: linux/${{ matrix.arch }}
          # Push by digest only; the merge job assembles the tags.
          outputs: type=image,name=${{ env.REGISTRY }}/${{ env.OWNER_REPO }}/${{ matrix.image }},push-by-digest=true,name-canonical=true,push=true
          cache-from: type=gha,scope=${{ matrix.image }}-${{ matrix.arch }}
          cache-to: type=gha,scope=${{ matrix.image }}-${{ matrix.arch }},mode=max
      - name: Export digest
        run: |
          mkdir -p /tmp/digests
          echo "${{ steps.build.outputs.digest }}" > "/tmp/digests/${{ matrix.image }}-${{ matrix.arch }}"
      - uses: actions/upload-artifact@v4
        with:
          name: digest-${{ matrix.image }}-${{ matrix.arch }}
          path: /tmp/digests/*
          retention-days: 1

  # ── Merge per-arch digests into manifest lists + sign ──────────────────────
  merge:
    needs: [meta, build]
    strategy:
      matrix:
        image: [authd, worldd, meridian-db]
    runs-on: meridian-amd64
    steps:
      - uses: docker/setup-buildx-action@v3
      - uses: docker/login-action@v3
        with:
          registry: ${{ env.REGISTRY }}
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - uses: actions/download-artifact@v4
        with:
          pattern: digest-${{ matrix.image }}-*
          path: /tmp/digests
          merge-multiple: true
      - name: Create manifest list (tags)
        id: create
        run: |
          IMG="${REGISTRY}/${OWNER_REPO}/${{ matrix.image }}"
          TAGS="-t ${IMG}:${{ needs.meta.outputs.sha }} -t ${IMG}:${{ needs.meta.outputs.moving }}"
          if [ "${{ needs.meta.outputs.latest }}" = "true" ]; then TAGS="$TAGS -t ${IMG}:latest"; fi
          REFS=""
          for f in /tmp/digests/${{ matrix.image }}-*; do REFS="$REFS ${IMG}@$(cat "$f")"; done
          docker buildx imagetools create $TAGS $REFS
          DIGEST=$(docker buildx imagetools inspect "${IMG}:${{ needs.meta.outputs.sha }}" --format '{{ .Manifest.Digest }}')
          echo "ref=${IMG}@${DIGEST}" >> "$GITHUB_OUTPUT"
      - uses: sigstore/cosign-installer@v3
      - name: Sign (keyless) + SBOM attest the merged digest
        env:
          COSIGN_YES: "true"
        run: cosign sign --yes "${{ steps.create.outputs.ref }}"
      - uses: anchore/sbom-action@v0
        with:
          image: ${{ steps.create.outputs.ref }}
          format: cyclonedx-json
          output-file: sbom-${{ matrix.image }}.cdx.json
      - name: Attest SBOM (keyless)
        env:
          COSIGN_YES: "true"
        run: cosign attest --yes --type cyclonedx --predicate sbom-${{ matrix.image }}.cdx.json "${{ steps.create.outputs.ref }}"

  # ── Auto-restart dev/ptr so pods repull the moving tag ─────────────────────
  restart:
    needs: [meta, merge]
    if: github.ref_name == 'dev' || github.ref_name == 'ptr'
    runs-on: meridian-amd64
    steps:
      # The ARC runner image has no kubectl; install it. In-cluster auth comes
      # from the runner pod's arc-runners/default SA token (RBAC in Task 2).
      - uses: azure/setup-kubectl@v4
      - name: Rollout-restart the realm daemons
        run: |
          NS="meridian-${{ github.ref_name }}"
          kubectl -n "$NS" rollout restart deployment -l app.kubernetes.io/part-of=meridian
          kubectl -n "$NS" rollout status deployment -l app.kubernetes.io/component=authd --timeout=120s
```

- [ ] **Step 2: Delete the superseded stopgap**

```bash
git rm .github/workflows/dev-images.yml
```

- [ ] **Step 3: Validate the workflow YAML**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/cd.yml')); print('cd.yml valid')"
command -v actionlint >/dev/null && actionlint .github/workflows/cd.yml || echo "actionlint: skip/none"
```
Expected: `cd.yml valid`. actionlint will warn about the custom runner labels (known, benign — same as arc-smoke); no OTHER errors.

- [ ] **Step 4: Commit** (do NOT push yet — RBAC for the restart job lands in Task 2)

```bash
git add .github/workflows/cd.yml
git commit -m "feat(cd): native multi-arch CD on self-hosted runners; retire dev-images"
```

---

## Task 2: In-cluster restart RBAC for the runner

**Files:**
- Create: `deploy/gitops/rbac/meridian-restarter.yaml`
- Modify: `deploy/gitops/apps/meridian-dev.yaml` (register the rbac path is NOT needed — see note)
- Test: `kubectl apply --dry-run=client`

The `restart` job runs on a runner pod in `arc-runners`, whose default ServiceAccount needs permission to `patch` Deployments in `meridian-dev`/`meridian-ptr`. Grant it via a Role + RoleBinding in each realm namespace, bound to the runner namespace's default SA.

- [ ] **Step 1: Create `deploy/gitops/rbac/meridian-restarter.yaml`** (dev only — `meridian-ptr`'s Role+Binding are added alongside the PTR realm in Phase 3, since applying to a not-yet-existent namespace would fail the sync):

```yaml
# SPDX-License-Identifier: Apache-2.0
# Lets the ARC runner (arc-runners/default SA) rollout-restart the realm daemons
# so cd.yml can repull the moving image tag. Least-privilege: get/list/patch on
# Deployments in the realm namespace only. (meridian-ptr Role added in Phase 3.)
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: meridian-restarter
  namespace: meridian-dev
rules:
  - apiGroups: ["apps"]
    resources: ["deployments"]
    verbs: ["get", "list", "patch"]
---
apiVersion: rbac.authorization.k8s.io/v1
kind: RoleBinding
metadata:
  name: meridian-restarter
  namespace: meridian-dev
subjects:
  - kind: ServiceAccount
    name: default
    namespace: arc-runners
roleRef:
  kind: Role
  name: meridian-restarter
  apiGroup: rbac.authorization.k8s.io
```

- [ ] **Step 2: Create the ArgoCD app to manage it** — `deploy/gitops/apps/meridian-rbac.yaml`:

```yaml
# SPDX-License-Identifier: Apache-2.0
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: meridian-rbac
  namespace: argocd
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  source:
    repoURL: https://github.com/kwilliams312/project_meridian.git
    targetRevision: dev
    path: deploy/gitops/rbac
  destination:
    server: https://kubernetes.default.svc
    namespace: default
  syncPolicy:
    automated: { prune: true, selfHeal: true }
    syncOptions: [ServerSideApply=true]
```

- [ ] **Step 3: Validate**

```bash
kubectl apply --dry-run=client -f deploy/gitops/rbac/meridian-restarter.yaml -f deploy/gitops/apps/meridian-rbac.yaml
```
Expected: the Role + RoleBinding (meridian-dev) + the Application all `created (dry run)`, no errors.

- [ ] **Step 4: Commit**

```bash
git add deploy/gitops/rbac/meridian-restarter.yaml deploy/gitops/apps/meridian-rbac.yaml
git commit -m "feat(gitops): least-priv restart RBAC for the CD runner (dev/ptr)"
```

---

## Task 3: Drop the amd64 pin + ignore the restart annotation

**Files:**
- Modify: `deploy/gitops/realms/dev/values.yaml`, `deploy/gitops/apps/meridian-dev.yaml`
- Test: `helm template` + `kubectl --dry-run`

- [ ] **Step 1: Remove the temporary amd64 nodeSelectors** in `deploy/gitops/realms/dev/values.yaml` — delete the `nodeSelector: { kubernetes.io/arch: amd64 }` lines from `authd`, `worldd`, and `mariadb`, and update the header comment (drop "amd64-pinned"). The daemons/DB now schedule on any arch (multi-arch images).

- [ ] **Step 2: Add `ignoreDifferences`** to `deploy/gitops/apps/meridian-dev.yaml` `spec:` so self-heal doesn't revert the rollout-restart annotation:

```yaml
  ignoreDifferences:
    - group: apps
      kind: Deployment
      jqPathExpressions:
        - '.spec.template.metadata.annotations."kubectl.kubernetes.io/restartedAt"'
```

- [ ] **Step 3: Validate the overlay still renders + the app parses**

```bash
helm template meridian-dev deploy/helm/meridian --namespace meridian-dev \
  -f deploy/gitops/realms/dev/values.yaml | kubectl apply --dry-run=client -f - 2>&1 | tail -5
kubectl apply --dry-run=client -f deploy/gitops/apps/meridian-dev.yaml
grep -c 'kubernetes.io/arch' deploy/gitops/realms/dev/values.yaml   # expect 0
```
Expected: overlay renders/dry-runs cleanly; the Application parses; the arch grep returns `0`.

- [ ] **Step 4: Commit + push everything**

```bash
git add deploy/gitops/realms/dev/values.yaml deploy/gitops/apps/meridian-dev.yaml
git commit -m "feat(gitops): drop dev amd64 pin (multi-arch) + ignore restart annotation"
git push origin dev
```

---

## Task 4: Live — prove multi-arch CD + cross-arch scheduling (LIVE)

**Prereq:** Tasks 1–3 pushed. The push to `dev` (Task 3 Step 4) touches `deploy/gitops/**` only — which does NOT match `cd.yml`'s paths — so trigger a real image build with a no-op touch under a matched path, OR just run once by pushing a `deploy/docker/**` change. Use the meta/build path deliberately:

- [ ] **Step 1: Trigger the reworked cd.yml + watch**

```bash
# Touch a cd-path file to fire the build (e.g. re-save the Dockerfile mtime via a comment bump is heavy;
# simplest: the Task-1 commit already changed .github/workflows/cd.yml which IS a cd path — so the push
# in Task 3 fired cd.yml. Find that run:
RID=$(gh run list --workflow=cd.yml --branch dev --limit 1 --json databaseId --jq '.[0].databaseId')
gh run watch "$RID" --exit-status --interval 30
```
Expected: green — 6 build legs (3 images × 2 arches), 3 merge legs, 1 restart. (First run is slow: native arm64 C++ compiles + cold cache.)

- [ ] **Step 2: Confirm each image is a real multi-arch manifest list**

```bash
for img in authd worldd meridian-db; do
  echo "== $img =="
  docker buildx imagetools inspect ghcr.io/kwilliams312/project_meridian/$img:dev \
    --format '{{ range .Manifest.Manifests }}{{ .Platform.OS }}/{{ .Platform.Architecture }} {{ end }}'
done
```
Expected: each prints `linux/amd64 linux/arm64` (both arches present).

- [ ] **Step 3: Confirm the dev realm now runs across both arch pools**

```bash
kubectl -n meridian-dev get pods -o wide --no-headers | awk '{print $1, $7}'
kubectl -n argocd get application meridian-dev -o jsonpath='{.status.sync.status}/{.status.health.status}{"\n"}'
```
Expected: pods `Running`; ArgoCD `Synced/Healthy`. With the pin dropped, the scheduler MAY place pods on arm64 (`talos-*`) or amd64 (`nuc*`) nodes — either is valid; the point is they are no longer forced to amd64. (Force-verify by deleting a pod and confirming it can land on an arm64 node.)

- [ ] **Step 4: Confirm auto-restart fired** (the cd.yml `restart` job)

```bash
kubectl -n meridian-dev get deployment meridian-dev-authd -o jsonpath='{.spec.template.metadata.annotations.kubectl\.kubernetes\.io/restartedAt}{"\n"}'
kubectl -n meridian-dev rollout history deployment meridian-dev-authd | tail -3
```
Expected: a `restartedAt` timestamp is present and a recent revision — proving the runner rollout-restarted the realm and self-heal did not revert it.

- [ ] **Step 5: Record completion in the runbook**

Append to `docs/ops/dev-realm.md` a "Phase 2b verified <date>: multi-arch images (amd64+arm64) via cd.yml on self-hosted runners; amd64 pin dropped; auto-restart working." Commit + push.

---

## Self-Review

**Spec coverage (§6.3/§6.5/§11 Phase 2):** native multi-arch build on ARC runners → Task 1 matrix ✓; manifest-list merge + per-branch tags + cosign/SBOM → Task 1 merge job ✓; retire dev-images stopgap → Task 1 delete ✓; auto-restart dev/ptr via ArgoCD-friendly rollout restart → Tasks 1(restart job)+2(RBAC)+3(ignoreDifferences) ✓; drop amd64 pin → Task 3 ✓; verified live → Task 4 ✓.

**Deferred (flagged):** the **bot image** (§6.4) → Phase 4, where the concurrency harness needs it. **godot-cpp question RESOLVED (investigated 2026-07-08):** `client/CMakeLists.txt:56-77` — building with `-DMERIDIAN_BOT=ON` configures only the bot + net core and `return()`s *before* `add_subdirectory(godot-cpp)` (and before the submodule FATAL_ERROR check), so the bot builds with a plain C++17 + OpenSSL + flatc toolchain, **no godot-cpp / no Godot**. ⇒ Phase 4's `Dockerfile.bot` mirrors the daemon multi-stage image (build `client/` with `-DMERIDIAN_BOT=ON`, target `meridian-bot`); context needs `client/` + `schema/` (add a `.dockerignore` exception for `client/`, which is currently fully excluded). Phase 3 (PTR/Prod realms) unchanged.

**Placeholder scan:** the only runtime-derived values are the branch→tag mapping (computed in the `meta` job) and `<date>` in the runbook note — no vague TODOs.

**Consistency:** runner labels `meridian-amd64`/`meridian-arm64` match ARC (Phase 2a) and the matrix `runs-on`. The moving tags `:dev`/`:ptr`/`:prod` match the realm overlays (dev uses `:dev`). The restart job's namespace `meridian-${ref_name}` and label selector `app.kubernetes.io/part-of=meridian` match the chart's labels; the RBAC binds `arc-runners/default` which is the SA the runner pods use; `ignoreDifferences` targets the exact `restartedAt` path the restart writes.
