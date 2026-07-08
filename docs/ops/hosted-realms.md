# Hosted realms (Dev / PTR / Prod) — operations

Three ArgoCD-managed realms on the Talos cluster, one per branch (D-37 / SYNC §16).
See also [dev-realm.md](dev-realm.md), [arc-runners.md](arc-runners.md).

## Realms

| Realm | Branch | NS | Sync | MariaDB | Shared LB IP (authd :7100 / worldd :7200) | Replicas |
|-------|--------|----|------|---------|-------------------------------------------|----------|
| Dev  | `dev`  | `meridian-dev`  | auto (+ auto-restart) | ephemeral emptyDir | `192.168.89.31` | 1 |
| PTR  | `ptr`  | `meridian-ptr`  | auto (+ auto-restart) | StatefulSet + Longhorn 8Gi | `192.168.89.32` | 2 |
| Prod | `main` | `meridian-prod` | **manual** | StatefulSet + Longhorn 20Gi | `192.168.89.33` | 3 |

- authd + worldd **share one MetalLB IP** per realm (`metallb.universe.tf/allow-shared-ip`).
- All images multi-arch (amd64+arm64); pods schedule across both node pools.
- worldd is wired to `meridian_auth` (`worldd.db`) so it can consume IF-3 grants (#340).
- Dev + PTR run the **short-term seed Job** (realm row + `devtester`); Prod does not
  (real provisioning is the admin portal / #339). ⚠️ Remove the seed when #339 lands.

## Promotion flow

Work lands on `dev`; promote by fast-forward merge:

```bash
git switch ptr  && git merge --ff-only dev && git push origin ptr  && git switch dev   # dev → PTR (auto-deploys)
git switch main && git merge --ff-only ptr && git push origin main && git switch dev    # ptr → Prod (does NOT auto-deploy)
```

Each branch push builds that branch's `:<tag>` multi-arch images via `cd.yml`. `dev`/`ptr`
auto-restart to repull; **`main` never auto-restarts** (Prod is deliberate). Rollback =
`git revert` on the branch.

## Promoting Prod (the manual gate)

After `ptr → main` builds `:prod`, sync Prod deliberately:

```bash
# Preferred (respects CreateNamespace): argocd app sync meridian-prod
# kubectl-only fallback — the namespace must exist first (a bare patch-sync does
# NOT create it), so pre-create it:
kubectl create namespace meridian-prod --dry-run=client -o yaml | kubectl apply -f -
kubectl -n argocd patch application meridian-prod --type merge -p '{"operation":{"sync":{"revision":"main"}}}'
kubectl -n argocd get application meridian-prod -o jsonpath='{.status.sync.status}/{.status.health.status}{"\n"}'
```

## Verify a realm

```bash
R=ptr   # or dev / prod
kubectl -n argocd get application meridian-$R -o jsonpath='{.status.sync.status}/{.status.health.status}{"\n"}'
kubectl -n meridian-$R get pods -o wide          # replicas Running across arches
kubectl -n meridian-$R get pvc                   # ptr/prod: Bound Longhorn PVC
IP=$([ $R = dev ] && echo 192.168.89.31 || { [ $R = ptr ] && echo 192.168.89.32 || echo 192.168.89.33; })
echo | openssl s_client -connect $IP:7100 2>/dev/null | openssl x509 -noout -subject   # authd
echo | openssl s_client -connect $IP:7200 2>/dev/null | openssl x509 -noout -subject   # worldd
```

## Gotchas seen at go-live

- **StatefulSet perma-OutOfSync**: the apiserver defaults `volumeClaimTemplates`
  (apiVersion/kind/status/volumeMode). The ptr/prod Applications `ignoreDifferences`
  those (VCTs are immutable anyway).
- **Manual Prod sync + namespace**: a `kubectl patch` sync does not honour
  `CreateNamespace`; pre-create the namespace (above) or use `argocd app sync`.
- **worldd + grants**: worldd needs `meridian_auth` access at M0 to consume grants
  (#340) — set per-realm via `worldd.db.enabled=true, name=meridian_auth`.
