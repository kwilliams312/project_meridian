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

## Gotchas seen at go-live

- After first creating the `arc-github-app` Secret, the controller may still be in
  reconcile backoff from when the Secret was absent — force it:
  `kubectl -n arc-systems rollout restart deploy/arc-controller-gha-rs-controller`.
- The **listener** pods run in `arc-systems` (the controller namespace), not
  `arc-runners`. Ephemeral runner pods (runner + dind sidecar) appear in
  `arc-runners` only while a job runs.
- `arc-smoke.yml` also triggers on a push that touches itself: a `workflow_dispatch`
  is only dispatchable once the workflow is on the **default branch**.

## Verified

2026-07-08 — both scale sets register; `arc-smoke` green on both arches (native
`x86_64` on `meridian-amd64`, `aarch64` on `meridian-arm64`) via dind buildx.
