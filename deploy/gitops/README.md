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

## Dev realm
Ephemeral MariaDB, self-signed TLS. Images are pinned to the immutable
`<short-sha>` via GitOps write-back (issue #380): cd.yml's `pin` job rewrites
`realms/dev/image.yaml` on every push to `dev`, and ArgoCD rolls the Deployments
off that manifest diff — no `kubectl rollout restart`, no moving-tag staleness.
`realms/dev/image.yaml` is machine-managed; do not hand-edit it.

PTR still repulls its moving `:ptr` tag via rollout-restart, and Prod is
manual-sync; extending the immutable pin to both is a follow-up (issue #380).
