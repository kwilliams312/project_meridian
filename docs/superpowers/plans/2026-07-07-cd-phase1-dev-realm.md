# CD Phase 1 — Dev Realm Live Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a stable, ArgoCD-managed `meridian-dev` realm (authd + worldd + an ephemeral in-cluster MariaDB) in the Talos cluster, tracking the `dev` branch, reachable over TLS on fixed NodePorts — using the existing amd64 images, with no ARC/multi-arch dependency.

**Architecture:** Extend the existing `deploy/helm/meridian` Helm chart with three opt-in capabilities — a bundled MariaDB, self-signed TLS via a per-pod initContainer, and NodePort exposure — then drive it from a `deploy/gitops/` overlay + ArgoCD `Application`. Database seed data is baked into a new `meridian-db` image so the schema stays source-of-truth in-repo. Dev pins pods to amd64 nodes (temporary, removed in Phase 2).

**Tech Stack:** Helm 4, ArgoCD v3.2.1, Talos Kubernetes, Longhorn (future realms only), MariaDB 11, Docker Buildx, C++ daemons (authd/worldd), `openssl`, `kubectl`, `argocd` CLI.

**Branch:** All work commits to `dev` (per `CLAUDE.md`). Verify with `git branch --show-current` → `dev` before starting.

**Reference:** Design spec [docs/superpowers/specs/2026-07-07-cd-hosted-realms-design.md](../specs/2026-07-07-cd-hosted-realms-design.md) (§3 realm matrix, §5 chart extensions, §11 phases). This plan implements **Phase 1** only.

---

## File Structure

**Created:**
- `deploy/docker/Dockerfile.db` — `meridian-db` image: `mariadb:11` + baked db-init wrappers and schema dirs. One responsibility: a self-seeding MariaDB image.
- `deploy/helm/meridian/templates/mariadb.yaml` — bundled MariaDB workload + Service, gated on `mariadb.enabled`. One responsibility: the in-cluster DB.
- `deploy/helm/meridian/templates/_mariadb.tpl` — MariaDB naming/selector helpers.
- `deploy/gitops/realms/dev/values.yaml` — the Dev realm's Helm values overlay.
- `deploy/gitops/apps/meridian-dev.yaml` — ArgoCD `Application` for the Dev realm.
- `deploy/gitops/root-app.yaml` — app-of-apps root; owns `apps/*`.
- `deploy/gitops/README.md` — how the GitOps tree maps to realms + how to register.

**Modified:**
- `deploy/helm/meridian/values.yaml` — add `mariadb.*`, `tls.mode`, per-daemon `service.nodePort`, and document the in-cluster-DB defaulting.
- `deploy/helm/meridian/templates/_daemon.tpl` — branch the `/certs` volume on `tls.mode` (Secret vs self-signed initContainer + emptyDir); add an optional `wait-for-db` initContainer.
- `deploy/helm/meridian/templates/_helpers.tpl` — `meridian.db.host` helper (default to the bundled DB service when `mariadb.enabled`); `meridian.mariadb.fullname`.
- `deploy/helm/meridian/templates/configmap.yaml` — source `MERIDIAN_DB_HOST` from the new helper.
- `deploy/helm/meridian/Chart.yaml` — bump chart `version` 0.1.0 → 0.2.0.

**Test harness:** Chart changes are verified with `helm template` render assertions + `helm lint` (there is no unit-test framework for the chart; this repo's `tests/` is Python-only). The `meridian-db` image is verified with `docker build` + `docker run`. Live deploy is verified with `kubectl`, `argocd`, and `openssl s_client`.

---

## Task 1: `meridian-db` self-seeding image

**Files:**
- Create: `deploy/docker/Dockerfile.db`
- Reference (baked in, unchanged): `deploy/docker/db-init/*.sql`, `server/db/auth/migrations/*.up.sql`, `server/db/characters/migrations/*.up.sql`, `schema/sql/world/*.sql`

- [ ] **Step 1: Write the failing test (a build + run smoke script)**

Create `deploy/docker/db-init/verify-meridian-db.sh`:

```bash
#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build meridian-db and assert it self-seeds the 3-DB split on first boot.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

docker build --platform linux/amd64 -f deploy/docker/Dockerfile.db -t meridian-db:test .

cid=$(docker run -d --rm \
  -e MARIADB_ROOT_PASSWORD=meridian-root \
  -e MARIADB_USER=meridian -e MARIADB_PASSWORD=meridian \
  meridian-db:test)
trap 'docker stop "$cid" >/dev/null 2>&1 || true' EXIT

echo "waiting for mariadb to become healthy + seed..."
for i in $(seq 1 60); do
  if docker exec "$cid" healthcheck.sh --connect --innodb_initialized >/dev/null 2>&1; then break; fi
  sleep 2
done

# The three schemas must exist, and the app user must reach all three.
got=$(docker exec "$cid" mariadb -umeridian -pmeridian -N -e \
  "SELECT GROUP_CONCAT(schema_name ORDER BY schema_name) FROM information_schema.schemata \
   WHERE schema_name LIKE 'meridian\\_%';")
echo "schemas: $got"
[ "$got" = "meridian_auth,meridian_characters,meridian_world" ] || { echo "FAIL: schemas"; exit 1; }

# Auth tables must be present (proves the SOURCEd migrations ran).
tbls=$(docker exec "$cid" mariadb -umeridian -pmeridian -N meridian_auth -e "SHOW TABLES;" | wc -l | tr -d ' ')
echo "auth tables: $tbls"
[ "$tbls" -ge 1 ] || { echo "FAIL: no auth tables"; exit 1; }
echo "PASS"
```

```bash
chmod +x deploy/docker/db-init/verify-meridian-db.sh
```

- [ ] **Step 2: Run it to verify it fails**

Run: `deploy/docker/db-init/verify-meridian-db.sh`
Expected: FAIL early — `failed to read dockerfile: deploy/docker/Dockerfile.db: no such file`.

- [ ] **Step 3: Write `deploy/docker/Dockerfile.db`**

```dockerfile
# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: Apache-2.0
#
# meridian-db — MariaDB 11 that self-seeds the 3-DB split (auth / characters /
# world) on first boot. The db-init wrappers (deploy/docker/db-init/*.sql) run
# alphabetically from /docker-entrypoint-initdb.d and SOURCE the real schema
# from /schemas/{auth,characters,world}. Baking the schema into the image keeps
# it source-of-truth in-repo (server/db/*, schema/sql/world) while letting both
# compose and the Helm chart consume one seeded image. Build context = repo root.
FROM mariadb:11

# The first-boot init wrappers (CREATE DATABASE + SOURCE the migrations + grants).
COPY deploy/docker/db-init/*.sql /docker-entrypoint-initdb.d/

# The schema the wrappers SOURCE, at the exact paths they reference.
COPY server/db/auth/migrations/          /schemas/auth/
COPY server/db/characters/migrations/    /schemas/characters/
COPY schema/sql/world/                    /schemas/world/
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `deploy/docker/db-init/verify-meridian-db.sh`
Expected: `schemas: meridian_auth,meridian_characters,meridian_world` then `PASS`.

- [ ] **Step 5: Tag + push the amd64 image to GHCR**

```bash
docker tag meridian-db:test ghcr.io/kwilliams312/project_meridian/meridian-db:dev
docker tag meridian-db:test ghcr.io/kwilliams312/project_meridian/meridian-db:latest
echo "$GITHUB_TOKEN" | docker login ghcr.io -u kwilliams312 --password-stdin
docker push ghcr.io/kwilliams312/project_meridian/meridian-db:dev
docker push ghcr.io/kwilliams312/project_meridian/meridian-db:latest
```
Expected: both pushes succeed. (In Phase 2 this build moves into `cd.yml` as a multi-arch matrix leg; the manual push here is the Phase-1 bootstrap.)

- [ ] **Step 6: Commit**

```bash
git add deploy/docker/Dockerfile.db deploy/docker/db-init/verify-meridian-db.sh
git commit -m "feat(cd): meridian-db self-seeding image (3-DB split baked from source)"
```

---

## Task 2: Chart — bundled MariaDB (ephemeral for dev, StatefulSet-ready)

**Files:**
- Create: `deploy/helm/meridian/templates/_mariadb.tpl`, `deploy/helm/meridian/templates/mariadb.yaml`
- Modify: `deploy/helm/meridian/values.yaml`
- Test: `helm template` / `helm lint` (run from `deploy/helm/meridian`)

- [ ] **Step 1: Add MariaDB values**

In `deploy/helm/meridian/values.yaml`, append a `mariadb` block after the `db:` block (before `authd:`):

```yaml
# ── Bundled MariaDB (opt-in; D-37 hosted realms) ─────────────────────────────
# When enabled, the chart runs an in-cluster MariaDB seeded from the meridian-db
# image (the 3-DB split baked from source). Dev uses persistence.enabled=false
# (ephemeral emptyDir — reseeded on every pod start). PTR/Prod set
# persistence.enabled=true for a StatefulSet + Longhorn PVC.
mariadb:
  enabled: false
  image:
    registry: ghcr.io
    repository: kwilliams312/project_meridian/meridian-db
    tag: "latest"
    pullPolicy: IfNotPresent
  # Root password (chart-created Secret). Dev only; PTR/Prod use db.existingSecret.
  rootPassword: "meridian-root"
  persistence:
    enabled: false        # false → Deployment + emptyDir; true → StatefulSet + PVC
    storageClass: "longhorn"
    size: 8Gi
  resources:
    requests:
      cpu: 100m
      memory: 256Mi
    limits:
      cpu: "1"
      memory: "1Gi"
  # MariaDB runs as its own uid (image default 999); it needs a writable rootfs,
  # so it does NOT inherit the daemons' read-only-rootfs containerSecurityContext.
  podSecurityContext:
    fsGroup: 999
  nodeSelector: {}
  tolerations: []
  affinity: {}
```

- [ ] **Step 2: Write the failing render test**

Run (from `deploy/helm/meridian`):
```bash
helm template t . --set mariadb.enabled=true --set tls.create=true \
  --set-file tls.cert=/dev/null --set-file tls.key=/dev/null \
  --show-only templates/mariadb.yaml
```
Expected: FAIL — `could not find template templates/mariadb.yaml`.

- [ ] **Step 3: Add MariaDB helpers**

Create `deploy/helm/meridian/templates/_mariadb.tpl`:

```yaml
{{/* SPDX-License-Identifier: Apache-2.0 */}}
{{/* MariaDB resource name: <fullname>-mariadb. */}}
{{- define "meridian.mariadb.fullname" -}}
{{- printf "%s-mariadb" (include "meridian.fullname" .) -}}
{{- end -}}

{{/* MariaDB selector labels. */}}
{{- define "meridian.mariadb.selectorLabels" -}}
{{ include "meridian.selectorLabels" . }}
app.kubernetes.io/component: mariadb
{{- end -}}
```

- [ ] **Step 4: Write `templates/mariadb.yaml`**

```yaml
{{- /* SPDX-License-Identifier: Apache-2.0 */ -}}
{{- /* Bundled MariaDB (D-37). Deployment+emptyDir when persistence is off (dev),
       StatefulSet+PVC when on (PTR/Prod). Seeded from the meridian-db image. */ -}}
{{- if .Values.mariadb.enabled }}
{{- $full := include "meridian.mariadb.fullname" . -}}
{{- $img := printf "%s/%s:%s" .Values.mariadb.image.registry .Values.mariadb.image.repository .Values.mariadb.image.tag -}}
apiVersion: v1
kind: Secret
metadata:
  name: {{ $full }}
  labels:
    {{- include "meridian.mariadb.selectorLabels" . | nindent 4 }}
type: Opaque
stringData:
  MARIADB_ROOT_PASSWORD: {{ .Values.mariadb.rootPassword | quote }}
  MARIADB_USER: {{ .Values.db.user | quote }}
  MARIADB_PASSWORD: {{ required "db.password is required when mariadb.enabled=true" .Values.db.password | quote }}
---
apiVersion: v1
kind: Service
metadata:
  name: {{ $full }}
  labels:
    {{- include "meridian.mariadb.selectorLabels" . | nindent 4 }}
spec:
  type: ClusterIP
  selector:
    {{- include "meridian.mariadb.selectorLabels" . | nindent 4 }}
  ports:
    - name: mysql
      port: 3306
      targetPort: mysql
      protocol: TCP
---
{{- if .Values.mariadb.persistence.enabled }}
apiVersion: apps/v1
kind: StatefulSet
{{- else }}
apiVersion: apps/v1
kind: Deployment
{{- end }}
metadata:
  name: {{ $full }}
  labels:
    {{- include "meridian.mariadb.selectorLabels" . | nindent 4 }}
spec:
  replicas: 1
  {{- if .Values.mariadb.persistence.enabled }}
  serviceName: {{ $full }}
  {{- end }}
  selector:
    matchLabels:
      {{- include "meridian.mariadb.selectorLabels" . | nindent 6 }}
  template:
    metadata:
      labels:
        {{- include "meridian.mariadb.selectorLabels" . | nindent 8 }}
    spec:
      securityContext:
        {{- toYaml .Values.mariadb.podSecurityContext | nindent 8 }}
      {{- with .Values.mariadb.nodeSelector }}
      nodeSelector:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      {{- with .Values.mariadb.affinity }}
      affinity:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      {{- with .Values.mariadb.tolerations }}
      tolerations:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      containers:
        - name: mariadb
          image: {{ $img | quote }}
          imagePullPolicy: {{ .Values.mariadb.image.pullPolicy }}
          args: ["--skip-name-resolve"]
          envFrom:
            - secretRef:
                name: {{ $full }}
          ports:
            - name: mysql
              containerPort: 3306
              protocol: TCP
          livenessProbe:
            exec:
              command: ["healthcheck.sh", "--connect", "--innodb_initialized"]
            initialDelaySeconds: 30
            periodSeconds: 15
            timeoutSeconds: 5
            failureThreshold: 6
          readinessProbe:
            exec:
              command: ["healthcheck.sh", "--connect", "--innodb_initialized"]
            initialDelaySeconds: 10
            periodSeconds: 10
            timeoutSeconds: 5
            failureThreshold: 6
          resources:
            {{- toYaml .Values.mariadb.resources | nindent 12 }}
          volumeMounts:
            - name: data
              mountPath: /var/lib/mysql
            - name: run
              mountPath: /run/mysqld
      volumes:
        - name: run
          emptyDir: {}
        {{- if not .Values.mariadb.persistence.enabled }}
        - name: data
          emptyDir: {}
        {{- end }}
  {{- if .Values.mariadb.persistence.enabled }}
  volumeClaimTemplates:
    - metadata:
        name: data
      spec:
        accessModes: ["ReadWriteOnce"]
        storageClassName: {{ .Values.mariadb.persistence.storageClass | quote }}
        resources:
          requests:
            storage: {{ .Values.mariadb.persistence.size | quote }}
  {{- end }}
{{- end }}
```

- [ ] **Step 5: Run render + lint to verify pass**

Run (from `deploy/helm/meridian`):
```bash
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.create=true --set-file tls.cert=/dev/null --set-file tls.key=/dev/null \
  --show-only templates/mariadb.yaml | grep -E "kind: (Deployment|Service|Secret)"
helm lint . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.create=true --set-file tls.cert=/dev/null --set-file tls.key=/dev/null
```
Expected: the `grep` prints `kind: Secret`, `kind: Service`, `kind: Deployment`; `helm lint` → `1 chart(s) linted, 0 chart(s) failed`.

- [ ] **Step 6: Verify the persistent path renders a StatefulSet**

Run:
```bash
helm template t . --set mariadb.enabled=true --set mariadb.persistence.enabled=true \
  --set db.password=meridian --set tls.create=true \
  --set-file tls.cert=/dev/null --set-file tls.key=/dev/null \
  --show-only templates/mariadb.yaml | grep -E "kind: StatefulSet|storageClassName"
```
Expected: prints `kind: StatefulSet` and `storageClassName: "longhorn"`.

- [ ] **Step 7: Commit**

```bash
git add deploy/helm/meridian/templates/_mariadb.tpl deploy/helm/meridian/templates/mariadb.yaml deploy/helm/meridian/values.yaml
git commit -m "feat(chart): bundled MariaDB (ephemeral Deployment / persistent StatefulSet)"
```

---

## Task 3: Chart — point daemons at the in-cluster DB + wait-for-db

**Files:**
- Modify: `deploy/helm/meridian/templates/_helpers.tpl`, `deploy/helm/meridian/templates/configmap.yaml`, `deploy/helm/meridian/templates/_daemon.tpl`
- Test: `helm template` render assertions

- [ ] **Step 1: Write the failing render test**

Run (from `deploy/helm/meridian`):
```bash
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.create=true --set-file tls.cert=/dev/null --set-file tls.key=/dev/null \
  --show-only templates/configmap.yaml | grep MERIDIAN_DB_HOST
```
Expected: FAIL to match the bundled service — it prints the static default `MERIDIAN_DB_HOST: "meridian-mariadb"` (from `db.host`), NOT the release-derived `t-meridian-mariadb`. We want it to auto-resolve to the bundled Service name when `mariadb.enabled=true`.

- [ ] **Step 2: Add the `meridian.db.host` helper**

Append to `deploy/helm/meridian/templates/_helpers.tpl`:

```yaml
{{/*
  Effective DB host. When the chart bundles MariaDB (mariadb.enabled), the
  daemons connect to the in-cluster Service (<fullname>-mariadb); otherwise the
  operator-provided db.host (external MariaDB) is used.
*/}}
{{- define "meridian.db.host" -}}
{{- if .Values.mariadb.enabled -}}
{{- include "meridian.mariadb.fullname" . -}}
{{- else -}}
{{- .Values.db.host -}}
{{- end -}}
{{- end -}}
```

- [ ] **Step 3: Use the helper in the ConfigMap**

In `deploy/helm/meridian/templates/configmap.yaml`, replace:
```yaml
  MERIDIAN_DB_HOST: {{ .Values.db.host | quote }}
```
with:
```yaml
  MERIDIAN_DB_HOST: {{ include "meridian.db.host" . | quote }}
```

- [ ] **Step 4: Add a `wait-for-db` initContainer for DB-backed daemons**

In `deploy/helm/meridian/templates/_daemon.tpl`, inside the pod `spec:` (immediately after the `securityContext:` block at line ~58-59, before `containers:`), insert:

```yaml
      {{- if and $root.Values.mariadb.enabled $cfg.db.enabled }}
      initContainers:
        - name: wait-for-db
          image: busybox:1.36
          command:
            - sh
            - -c
            - |
              until nc -z {{ include "meridian.mariadb.fullname" $root }} 3306; do
                echo "waiting for {{ include "meridian.mariadb.fullname" $root }}:3306"; sleep 2;
              done
          securityContext:
            runAsNonRoot: true
            runAsUser: 10001
            allowPrivilegeEscalation: false
            readOnlyRootFilesystem: true
            capabilities:
              drop: ["ALL"]
      {{- end }}
```

- [ ] **Step 5: Run render to verify pass**

Run (from `deploy/helm/meridian`):
```bash
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.create=true --set-file tls.cert=/dev/null --set-file tls.key=/dev/null \
  --show-only templates/configmap.yaml | grep 'MERIDIAN_DB_HOST: "t-meridian-mariadb"'
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.create=true --set-file tls.cert=/dev/null --set-file tls.key=/dev/null \
  --show-only templates/authd.yaml | grep -A1 'initContainers:' | grep 'wait-for-db'
```
Expected: first prints the release-derived host line; second prints `- name: wait-for-db`. worldd (db disabled) must NOT get the initContainer — verify:
```bash
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.create=true --set-file tls.cert=/dev/null --set-file tls.key=/dev/null \
  --show-only templates/worldd.yaml | grep -c 'wait-for-db' || true
```
Expected: `0`.

- [ ] **Step 6: Commit**

```bash
git add deploy/helm/meridian/templates/_helpers.tpl deploy/helm/meridian/templates/configmap.yaml deploy/helm/meridian/templates/_daemon.tpl
git commit -m "feat(chart): daemons resolve bundled-DB host + wait-for-db initContainer"
```

---

## Task 4: Chart — self-signed TLS via initContainer (`tls.mode: selfSignedInit`)

**Files:**
- Modify: `deploy/helm/meridian/values.yaml`, `deploy/helm/meridian/templates/_daemon.tpl`
- Test: `helm template` render assertions

- [ ] **Step 1: Add the `tls.mode` value**

In `deploy/helm/meridian/values.yaml`, inside the `tls:` block, add after `create:`:

```yaml
  # Cert-provisioning mode:
  #   existingSecret : mount tls.existingSecret (real certs — PTR/Prod).
  #   create         : chart renders a Secret from tls.cert/tls.key PEM.
  #   selfSignedInit : each pod generates a throwaway self-signed cert into an
  #                    emptyDir /certs via an initContainer (dev only; no Secret).
  mode: existingSecret
```

- [ ] **Step 2: Write the failing render test**

Run (from `deploy/helm/meridian`):
```bash
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.mode=selfSignedInit --show-only templates/authd.yaml | grep 'name: certgen'
```
Expected: FAIL (no output, exit 1) — and note that without the new mode the template errors on the missing `tls.existingSecret`.

- [ ] **Step 3: Branch the TLS volume + add the certgen initContainer**

In `deploy/helm/meridian/templates/_daemon.tpl`:

(a) Replace the `initContainers:` guard you added in Task 3 so both initContainers coexist. Change the Task-3 block's opening from:
```yaml
      {{- if and $root.Values.mariadb.enabled $cfg.db.enabled }}
      initContainers:
```
to:
```yaml
      {{- if or (eq $root.Values.tls.mode "selfSignedInit") (and $root.Values.mariadb.enabled $cfg.db.enabled) }}
      initContainers:
      {{- if eq $root.Values.tls.mode "selfSignedInit" }}
        - name: certgen
          image: alpine/openssl:3.3.2
          args:
            - req
            - -x509
            - -newkey
            - rsa:2048
            - -nodes
            - -keyout
            - /certs/{{ $root.Values.tls.keyKey }}
            - -out
            - /certs/{{ $root.Values.tls.certKey }}
            - -days
            - "3650"
            - -subj
            - /CN={{ $fullname }}
            - -addext
            - subjectAltName=DNS:localhost,DNS:{{ $fullname }},DNS:{{ $fullname }}.{{ $root.Release.Namespace }}.svc,IP:127.0.0.1
          securityContext:
            runAsNonRoot: true
            runAsUser: 10001
            allowPrivilegeEscalation: false
            readOnlyRootFilesystem: true
            capabilities:
              drop: ["ALL"]
          volumeMounts:
            - name: tls
              mountPath: /certs
      {{- end }}
      {{- if and $root.Values.mariadb.enabled $cfg.db.enabled }}
```
And close the DB initContainer with its own `{{- end }}` before the outer `{{- end }}` (the outer end already exists from Task 3). The result: an outer `if or(...)`, then two inner `if` blocks.

(b) Branch the `tls` volume. Replace the existing `volumes:` `- name: tls` block:
```yaml
        - name: tls
          secret:
            secretName: {{ include "meridian.tlsSecretName" $root }}
            items:
              - key: {{ $root.Values.tls.certKey }}
                path: {{ $root.Values.tls.certKey }}
              - key: {{ $root.Values.tls.keyKey }}
                path: {{ $root.Values.tls.keyKey }}
```
with:
```yaml
        - name: tls
          {{- if eq $root.Values.tls.mode "selfSignedInit" }}
          emptyDir: {}
          {{- else }}
          secret:
            secretName: {{ include "meridian.tlsSecretName" $root }}
            items:
              - key: {{ $root.Values.tls.certKey }}
                path: {{ $root.Values.tls.certKey }}
              - key: {{ $root.Values.tls.keyKey }}
                path: {{ $root.Values.tls.keyKey }}
          {{- end }}
```

(c) The `volumeMounts` for `tls` in the daemon container must be writable in selfSignedInit mode (the initContainer writes it; the daemon reads it). It is already `readOnly: true` — change it to conditionally drop `readOnly` when selfSignedInit. Replace:
```yaml
            - name: tls
              mountPath: /certs
              readOnly: true
```
with:
```yaml
            - name: tls
              mountPath: /certs
              readOnly: {{ ne $root.Values.tls.mode "selfSignedInit" }}
```

- [ ] **Step 4: Run render to verify pass**

Run (from `deploy/helm/meridian`):
```bash
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.mode=selfSignedInit --show-only templates/authd.yaml \
  | grep -E 'name: certgen|emptyDir: \{\}|name: wait-for-db'
```
Expected: prints `name: certgen`, `emptyDir: {}`, and `name: wait-for-db` (authd has both initContainers).

- [ ] **Step 5: Verify the existingSecret path still works**

Run:
```bash
helm template t . --set mariadb.enabled=false --set db.host=ext-db --set db.password=x \
  --set tls.mode=existingSecret --set tls.existingSecret=my-tls \
  --show-only templates/authd.yaml | grep -E 'secretName: my-tls'
```
Expected: prints `secretName: my-tls` (no certgen initContainer in this mode).

- [ ] **Step 6: Commit**

```bash
git add deploy/helm/meridian/values.yaml deploy/helm/meridian/templates/_daemon.tpl
git commit -m "feat(chart): tls.mode=selfSignedInit — per-pod self-signed cert via initContainer"
```

---

## Task 5: Chart — NodePort exposure

**Files:**
- Modify: `deploy/helm/meridian/values.yaml`, `deploy/helm/meridian/templates/_daemon.tpl`
- Test: `helm template` render assertions

- [ ] **Step 1: Add `service.nodePort` to each daemon's values**

In `deploy/helm/meridian/values.yaml`, under `authd.service:` add `nodePort: null`, and the same under `worldd.service:`:

```yaml
  service:
    type: ClusterIP
    port: 7100                      # (authd; worldd uses 7200)
    nodePort: null                  # set (e.g. 31710) when type=NodePort
    annotations: {}
```

- [ ] **Step 2: Write the failing render test**

Run (from `deploy/helm/meridian`):
```bash
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.mode=selfSignedInit \
  --set authd.service.type=NodePort --set authd.service.nodePort=31710 \
  --show-only templates/authd.yaml | grep 'nodePort: 31710'
```
Expected: FAIL (no output) — the Service port has no `nodePort` field yet.

- [ ] **Step 3: Render `nodePort` in the Service template**

In `deploy/helm/meridian/templates/_daemon.tpl`, in `meridian.daemon.service`, replace the `game` port entry:
```yaml
    - name: game
      port: {{ $cfg.service.port | default $cfg.port }}
      targetPort: game
      protocol: TCP
```
with:
```yaml
    - name: game
      port: {{ $cfg.service.port | default $cfg.port }}
      targetPort: game
      protocol: TCP
      {{- if and (eq ($cfg.service.type | default "ClusterIP") "NodePort") $cfg.service.nodePort }}
      nodePort: {{ $cfg.service.nodePort }}
      {{- end }}
```

- [ ] **Step 4: Run render to verify pass**

Run:
```bash
helm template t . --set mariadb.enabled=true --set db.password=meridian \
  --set tls.mode=selfSignedInit \
  --set authd.service.type=NodePort --set authd.service.nodePort=31710 \
  --show-only templates/authd.yaml | grep -E 'type: NodePort|nodePort: 31710'
```
Expected: prints `type: NodePort` and `nodePort: 31710`.

- [ ] **Step 5: Bump the chart version + lint the whole chart**

In `deploy/helm/meridian/Chart.yaml`, change `version: 0.1.0` to `version: 0.2.0`. Then run:
```bash
helm lint . --set mariadb.enabled=true --set db.password=meridian --set tls.mode=selfSignedInit
```
Expected: `1 chart(s) linted, 0 chart(s) failed`.

- [ ] **Step 6: Commit**

```bash
git add deploy/helm/meridian/values.yaml deploy/helm/meridian/templates/_daemon.tpl deploy/helm/meridian/Chart.yaml
git commit -m "feat(chart): NodePort exposure + bump chart to 0.2.0"
```

---

## Task 6: GitOps — Dev realm values, ArgoCD Application, root-app

**Files:**
- Create: `deploy/gitops/realms/dev/values.yaml`, `deploy/gitops/apps/meridian-dev.yaml`, `deploy/gitops/root-app.yaml`, `deploy/gitops/README.md`
- Test: `helm template` full render with the dev overlay; `kubectl apply --dry-run=client`

- [ ] **Step 1: Write the Dev realm values overlay**

Create `deploy/gitops/realms/dev/values.yaml`:

```yaml
# SPDX-License-Identifier: Apache-2.0
# Dev realm (meridian-dev) overlay — ephemeral DB, self-signed TLS, NodePort,
# :dev images, amd64-pinned (temporary until Phase 2 multi-arch). Design §3.
image:
  tag: "dev"                # moving per-branch tag (Phase 2 publishes it; :latest works until then)
  pullPolicy: Always

# Temporary: Phase-1 images are amd64-only. Removed in Phase 2 (multi-arch).
authd:
  replicaCount: 1
  service: { type: NodePort, nodePort: 31710 }
  nodeSelector: { kubernetes.io/arch: amd64 }
  probes: { type: tcp }
worldd:
  replicaCount: 1
  service: { type: NodePort, nodePort: 31720 }
  nodeSelector: { kubernetes.io/arch: amd64 }
  probes: { type: tcp }

tls:
  mode: selfSignedInit

db:
  password: "meridian"

mariadb:
  enabled: true
  image: { tag: "dev" }
  persistence: { enabled: false }
  nodeSelector: { kubernetes.io/arch: amd64 }
```

Note: `probes.type: tcp` — with a running DB and generated cert the daemons open their listener, so a TCP readiness probe on the game port is a truer signal than `--version` here.

- [ ] **Step 2: Verify the overlay renders a complete, valid manifest set**

Run (from repo root):
```bash
helm template meridian-dev deploy/helm/meridian \
  --namespace meridian-dev -f deploy/gitops/realms/dev/values.yaml \
  | kubectl apply --dry-run=client -f - 2>&1 | tail -20
```
Expected: `configmap/... created (dry run)`, `secret/... created (dry run)`, `service/meridian-dev-authd created (dry run)`, `deployment.apps/meridian-dev-authd created (dry run)`, `deployment.apps/meridian-dev-mariadb created (dry run)`, etc. — no errors.

- [ ] **Step 3: Write the ArgoCD Application**

Create `deploy/gitops/apps/meridian-dev.yaml`:

```yaml
# SPDX-License-Identifier: Apache-2.0
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: meridian-dev
  namespace: argocd
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  source:
    repoURL: https://github.com/kwilliams312/project_meridian.git
    targetRevision: dev
    path: deploy/helm/meridian
    helm:
      valueFiles:
        - ../../gitops/realms/dev/values.yaml
  destination:
    server: https://kubernetes.default.svc
    namespace: meridian-dev
  syncPolicy:
    automated:
      prune: true
      selfHeal: true
    syncOptions:
      - CreateNamespace=true
      - ServerSideApply=true
```

- [ ] **Step 4: Write the root app-of-apps**

Create `deploy/gitops/root-app.yaml`:

```yaml
# SPDX-License-Identifier: Apache-2.0
# App-of-apps: owns every Application under deploy/gitops/apps/. Register ONCE:
#   kubectl apply -f deploy/gitops/root-app.yaml
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: meridian-root
  namespace: argocd
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  source:
    repoURL: https://github.com/kwilliams312/project_meridian.git
    targetRevision: dev
    path: deploy/gitops/apps
    directory:
      recurse: false
  destination:
    server: https://kubernetes.default.svc
    namespace: argocd
  syncPolicy:
    automated:
      prune: true
      selfHeal: true
```

- [ ] **Step 5: Write `deploy/gitops/README.md`**

```markdown
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
```

- [ ] **Step 6: Validate the Application manifests**

Run:
```bash
kubectl apply --dry-run=client -f deploy/gitops/apps/meridian-dev.yaml
kubectl apply --dry-run=client -f deploy/gitops/root-app.yaml
```
Expected: `application.argoproj.io/meridian-dev created (dry run)` and `.../meridian-root created (dry run)` — no schema errors.

- [ ] **Step 7: Commit**

```bash
git add deploy/gitops/
git commit -m "feat(gitops): meridian-dev ArgoCD Application + dev values overlay + root-app"
```

---

## Task 7: Live deploy + realm-up verification

**Files:** none (operational). Prereq: Tasks 1–6 committed **and pushed** to `dev` (`git push origin dev`), since ArgoCD reads the branch from `origin`.

- [ ] **Step 1: Push the branch**

```bash
git push origin dev
```
Expected: ArgoCD can now see all Phase-1 files on `origin/dev`.

- [ ] **Step 2: Register the root app + trigger sync**

```bash
kubectl apply -f deploy/gitops/root-app.yaml
argocd app sync meridian-root
argocd app sync meridian-dev
```
Expected: both apps report `Synced`. (If `argocd` CLI is unauthenticated, use `kubectl -n argocd patch app meridian-dev --type merge -p '{"operation":{"sync":{}}}'` or the ArgoCD UI.)

- [ ] **Step 3: Verify the namespace + pods are healthy**

```bash
kubectl -n meridian-dev get pods -o wide
```
Expected: `meridian-dev-mariadb-*` `1/1 Running`; `meridian-dev-authd-*` and `meridian-dev-worldd-*` `1/1 Running`, all on amd64 (`nuc*`) nodes. authd shows its init (`wait-for-db`, `certgen`) completed.

- [ ] **Step 4: Verify the DB seeded (3-DB split)**

```bash
kubectl -n meridian-dev exec deploy/meridian-dev-mariadb -- \
  mariadb -umeridian -pmeridian -N -e \
  "SELECT schema_name FROM information_schema.schemata WHERE schema_name LIKE 'meridian\\_%';"
```
Expected: three lines — `meridian_auth`, `meridian_characters`, `meridian_world`.

- [ ] **Step 5: Verify authd is a live TLS listener via its NodePort**

```bash
NODE_IP=$(kubectl get node -l kubernetes.io/arch=amd64 -o jsonpath='{.items[0].status.addresses[?(@.type=="InternalIP")].address}')
echo | openssl s_client -connect "${NODE_IP}:31710" -servername meridian-dev-authd 2>/dev/null \
  | openssl x509 -noout -subject
```
Expected: prints `subject=CN=meridian-dev-authd` — proves the self-signed cert generated and authd is terminating TLS on the NodePort.

- [ ] **Step 6: Verify worldd's TLS NodePort too**

```bash
echo | openssl s_client -connect "${NODE_IP}:31720" -servername meridian-dev-worldd 2>/dev/null \
  | openssl x509 -noout -subject
```
Expected: `subject=CN=meridian-dev-worldd`.

- [ ] **Step 7: Verify ArgoCD reports the realm Healthy + record evidence**

```bash
argocd app get meridian-dev --refresh -o wide 2>/dev/null | grep -E 'Health Status|Sync Status' \
  || kubectl -n argocd get application meridian-dev -o jsonpath='{.status.sync.status} {.status.health.status}{"\n"}'
```
Expected: `Synced` + `Healthy`.

- [ ] **Step 8: Commit an ops runbook capturing the go-live**

Create `docs/ops/dev-realm.md` with: the register/sync commands (Steps 2), the NodePort map (authd 31710 / worldd 31720 on any amd64 node IP), the verification commands (Steps 3–7), and the Phase-2 follow-ups (drop the amd64 nodeSelector once multi-arch images exist; move the `meridian-db` build into `cd.yml`).

```bash
git add docs/ops/dev-realm.md
git commit -m "docs(ops): dev realm go-live runbook + verification evidence"
git push origin dev
```

---

## Self-Review

**Spec coverage (design §5 / §11 Phase 1):**
- §5.1 bundled MariaDB (ephemeral + StatefulSet-ready) → Task 2 ✓; seeded from `db-init` + schema → Task 1 (baked image) ✓.
- §5.2 self-signed TLS bootstrap → Task 4 (initContainer variant; simpler than the spec's hook-Job — noted at plan top) ✓.
- §5.3 NodePort exposure + always-present ClusterIP → Task 5 (NodePort) ✓; ClusterIP is the default Service type for the bundled DB and for any daemon not overridden ✓.
- §4 GitOps layout (root-app, apps/, realms/dev) → Task 6 ✓.
- §11 Phase 1 "deploy current amd64 image, temporary amd64 nodeSelector, no ARC" → Task 6 values + Task 7 ✓.
- Local build env untouched → no compose/`run-local.sh` files modified ✓.

**Deferred (correctly out of Phase 1):** multi-arch/ARC (Phase 2), bot image + login smoke + `add-users` (Phase 2/4), PTR/Prod overlays (Phase 3). Phase 1 acceptance is realm-up + TLS-reachable, not a gameplay smoke — stated explicitly so it does not overclaim.

**Placeholder scan:** every code step contains complete file content or exact commands with expected output; no TBD/TODO. ✓

**Type/name consistency:** `meridian.mariadb.fullname` (Task 2) is reused verbatim in Tasks 3–4; the Service name it yields (`<release>-mariadb`, e.g. `meridian-dev-mariadb`) matches the `MERIDIAN_DB_HOST` render assertion in Task 3 Step 5 and the exec target in Task 7 Step 4. `tls.certKey`/`tls.keyKey` (existing values) are used consistently in the certgen args and volume items. NodePorts 31710/31720 match between the dev overlay (Task 6) and the verification (Task 7). ✓
