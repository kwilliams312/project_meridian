# Meridian GitOps

ArgoCD-managed hosted realms (D-37, design spec 2026-07-07). Branch → realm:
`dev`→meridian-dev, `ptr`→meridian-ptr, `main`→meridian-prod.

## Register (once)
    kubectl apply -f deploy/gitops/root-app.yaml
The `meridian-root` app-of-apps then syncs everything in `apps/`.

## Layout
- `root-app.yaml`   app-of-apps; owns `apps/*`
- `apps/`           one ArgoCD Application per realm
- `realms/<r>/`     that realm's Helm values overlay

## Immutable image pin (all realms)
Every realm deploys an immutable `<short-sha>` from a machine-managed
`realms/<r>/image.yaml` overlay (listed **after** `values.yaml` in the realm's
Application, so its tags win over the moving `:dev`/`:ptr`/`:prod` tag). ArgoCD
rolls Deployments off the resulting manifest diff — no `kubectl rollout restart`,
no moving-tag staleness. **Do not hand-edit any `image.yaml`** — they are
machine-managed.

- **Dev (issue #380):** cd.yml's `pin` job rewrites `realms/dev/image.yaml` to
  the just-built `<short-sha>` on every push to `dev`.
- **PTR / Prod (issue #382):** the pin is **carried by promotion**, not built
  per-branch — so PTR/Prod always run the exact image validated on the prior
  tier. The `promote` workflow (`.github/workflows/promote.yml`, manual
  `workflow_dispatch`) merges the branch **and** copies the prior tier's pinned
  `<short-sha>` into the next realm's `image.yaml`:
  - `tier=ptr` → merge `dev → ptr`, copy `realms/dev` SHA into `realms/ptr/image.yaml`
  - `tier=prod` → merge `ptr → main`, copy `realms/ptr` SHA into `realms/prod/image.yaml`

  Run it with `dry_run=true` first to preview the exact diff without pushing.

## Rollout & the Prod gate
PTR auto-syncs (prune + selfHeal), so a promotion into `ptr` rolls out on its
own. **Prod stays manual-sync** (design §3): the promotion advances `main` and
pins `realms/prod/image.yaml`, but an operator still runs the ArgoCD sync — a
human gate in front of production. cd.yml no longer touches any cluster (the
interim ptr rollout-restart and its RBAC were removed). Rollback at any tier =
`git revert` on that branch.

## Dev realm
Ephemeral MariaDB, self-signed TLS, pinned as above.
