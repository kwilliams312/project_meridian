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
Ephemeral MariaDB, self-signed TLS, NodePort authd 31710 / worldd 31720,
amd64-pinned (temporary until Phase 2). `:dev` images (`:latest` until the
per-branch tag is first published).
