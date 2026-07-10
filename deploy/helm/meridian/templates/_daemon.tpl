{{/* SPDX-License-Identifier: Apache-2.0 */}}
{{/*
  Reusable Deployment + Service for a Meridian daemon (authd / worldd). Both
  daemons share the same shape — same image family, same /certs TLS mount, same
  non-root uid-10001 securityContext, same --version probes — differing only in
  image name, port, and whether they read a DB (authd yes / worldd no at M0).

  Usage:
    {{ include "meridian.daemon.deployment" (dict "root" $ "daemon" "authd" "cfg" .Values.authd "dbDefault" .Values.db.names.auth) }}
    {{ include "meridian.daemon.service"    (dict "root" $ "daemon" "authd" "cfg" .Values.authd) }}
*/}}

{{- define "meridian.daemon.deployment" -}}
{{- $root := .root -}}
{{- $daemon := .daemon -}}
{{- $cfg := .cfg -}}
{{- $fullname := printf "%s-%s" (include "meridian.fullname" $root) $daemon -}}
apiVersion: apps/v1
kind: Deployment
metadata:
  name: {{ $fullname }}
  labels:
    {{- include "meridian.labels" $root | nindent 4 }}
    app.kubernetes.io/component: {{ $daemon }}
  {{- with (include "meridian.annotations" $root) }}
  annotations:
    {{- . | nindent 4 }}
  {{- end }}
spec:
  replicas: {{ $cfg.replicaCount | default 1 }}
  selector:
    matchLabels:
      {{- include "meridian.selectorLabels" $root | nindent 6 }}
      app.kubernetes.io/component: {{ $daemon }}
  template:
    metadata:
      labels:
        {{- include "meridian.labels" $root | nindent 8 }}
        app.kubernetes.io/component: {{ $daemon }}
      annotations:
        {{- /* Roll pods when config/secrets change. */}}
        checksum/config: {{ include (print $root.Template.BasePath "/configmap.yaml") $root | sha256sum }}
        {{- if $root.Values.observability.enabled }}
        {{- if $root.Values.observability.podAnnotations }}
        prometheus.io/scrape: "true"
        prometheus.io/port: {{ $root.Values.observability.metricsPort | quote }}
        prometheus.io/path: "/metrics"
        {{- end }}
        {{- end }}
        {{- with $cfg.podAnnotations }}
        {{- toYaml . | nindent 8 }}
        {{- end }}
    spec:
      {{- with $root.Values.imagePullSecrets }}
      imagePullSecrets:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      securityContext:
        {{- toYaml $root.Values.podSecurityContext | nindent 8 }}
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
      {{- end }}
      containers:
        - name: {{ $daemon }}
          image: {{ include "meridian.image" (dict "root" $root "daemon" $daemon "cfg" $cfg) | quote }}
          imagePullPolicy: {{ $root.Values.image.pullPolicy }}
          securityContext:
            {{- toYaml $root.Values.containerSecurityContext | nindent 12 }}
          {{- /* meridiand parses "--flag value" (space-separated); the
                 "--flag=value" form is silently ignored (authd/worldd main.cpp),
                 so each flag + value MUST be separate argv entries. */}}
          args:
            - --cert
            - {{ printf "/certs/%s" $root.Values.tls.certKey | quote }}
            - --key
            - {{ printf "/certs/%s" $root.Values.tls.keyKey | quote }}
            - --bind
            - {{ $cfg.bindAddress | default "0.0.0.0" | quote }}
            - --port
            - {{ $cfg.port | quote }}
            {{- if $root.Values.observability.enabled }}
            - --metrics-port
            - {{ $root.Values.observability.metricsPort | quote }}
            - --metrics-bind
            - {{ $root.Values.observability.metricsBind | quote }}
            {{- end }}
            {{- with $cfg.extraArgs }}
            {{- toYaml . | nindent 12 }}
            {{- end }}
          ports:
            - name: game
              containerPort: {{ $cfg.port }}
              protocol: TCP
            {{- if $root.Values.observability.enabled }}
            - name: metrics
              containerPort: {{ $root.Values.observability.metricsPort }}
              protocol: TCP
            {{- end }}
          envFrom:
            - configMapRef:
                name: {{ include "meridian.configMapName" $root }}
          env:
            {{- if $cfg.db.enabled }}
            - name: MERIDIAN_DB_NAME
              value: {{ $cfg.db.name | default .dbDefault | quote }}
            - name: MERIDIAN_DB_PASS
              valueFrom:
                secretKeyRef:
                  name: {{ include "meridian.dbSecretName" $root }}
                  key: {{ include "meridian.dbSecretKey" $root }}
            {{- end }}
            {{- /* Characters DB (ENTER_WORLD ownership load, D-35/#341). worldd
                   reads a SEPARATE MERIDIAN_CHARDB_* block — it does NOT reuse the
                   MERIDIAN_DB_* host/port/user, and it enables its characters DB
                   ONLY when MERIDIAN_CHARDB_USER is non-empty. The shared ConfigMap
                   supplies host/port/user under MERIDIAN_DB_* only, so ALL FIVE
                   CHARDB vars must be rendered here (host/port/user mirror the DB
                   connection; NAME defaults to the characters schema; PASS reuses
                   the same DB Secret). Rendered only when chardb.enabled. */ -}}
            {{- if and $cfg.chardb $cfg.chardb.enabled }}
            - name: MERIDIAN_CHARDB_HOST
              value: {{ include "meridian.db.host" $root | quote }}
            - name: MERIDIAN_CHARDB_PORT
              value: {{ $root.Values.db.port | quote }}
            - name: MERIDIAN_CHARDB_USER
              value: {{ $root.Values.db.user | quote }}
            - name: MERIDIAN_CHARDB_NAME
              value: {{ $cfg.chardb.name | default $root.Values.db.names.characters | quote }}
            - name: MERIDIAN_CHARDB_PASS
              valueFrom:
                secretKeyRef:
                  name: {{ include "meridian.dbSecretName" $root }}
                  key: {{ include "meridian.dbSecretKey" $root }}
            {{- end }}
            {{- /* World / CONTENT DB (IF-4 boot + #390 DB content stores + #483
                   DbAbilityStore). worldd reads its OWN MERIDIAN_WORLDDB_* block
                   (main.cpp maps MERIDIAN_WORLDDB_* -> worlddb.*) so the content
                   DB can live on a different host than the auth/characters DB
                   (SAD §2.2 3-DB split). It loads the DB-backed content stores
                   ONLY when MERIDIAN_WORLDDB_USER is non-empty; unset ⇒ worldd
                   logs "no world DB configured … serving without content" and the
                   realm serves ZERO content (#482). The shared ConfigMap supplies
                   host/port/user under MERIDIAN_DB_* only, so ALL FIVE WORLDDB vars
                   are rendered here (host/port/user mirror the DB connection; NAME
                   defaults to the world schema; PASS reuses the DB Secret).
                   MERIDIAN_WORLDDB_EXPECTED_HASH is optional (advisory at M0–M1):
                   when set, worldd loudly warns on an IF-4 content-hash mismatch
                   but still boots. Rendered only when worlddb.enabled. */ -}}
            {{- if and $cfg.worlddb $cfg.worlddb.enabled }}
            - name: MERIDIAN_WORLDDB_HOST
              value: {{ include "meridian.db.host" $root | quote }}
            - name: MERIDIAN_WORLDDB_PORT
              value: {{ $root.Values.db.port | quote }}
            - name: MERIDIAN_WORLDDB_USER
              value: {{ $root.Values.db.user | quote }}
            - name: MERIDIAN_WORLDDB_NAME
              value: {{ $cfg.worlddb.name | default $root.Values.db.names.world | quote }}
            - name: MERIDIAN_WORLDDB_PASS
              valueFrom:
                secretKeyRef:
                  name: {{ include "meridian.dbSecretName" $root }}
                  key: {{ include "meridian.dbSecretKey" $root }}
            {{- with $cfg.worlddb.expectedHash }}
            - name: MERIDIAN_WORLDDB_EXPECTED_HASH
              value: {{ . | quote }}
            {{- end }}
            {{- end }}
            {{- with $cfg.extraEnv }}
            {{- toYaml . | nindent 12 }}
            {{- end }}
          {{- /* Liveness/readiness: the daemon's own --version (Dockerfile
                 HEALTHCHECK), or a TCP probe on the game port. */ -}}
          {{- if eq ($cfg.probes.type | default "exec") "tcp" }}
          livenessProbe:
            tcpSocket:
              port: game
            {{- toYaml $cfg.probes.liveness | nindent 12 }}
          readinessProbe:
            tcpSocket:
              port: game
            {{- toYaml $cfg.probes.readiness | nindent 12 }}
          {{- else }}
          livenessProbe:
            exec:
              command: ["/usr/local/bin/meridiand", "--version"]
            {{- toYaml $cfg.probes.liveness | nindent 12 }}
          readinessProbe:
            exec:
              command: ["/usr/local/bin/meridiand", "--version"]
            {{- toYaml $cfg.probes.readiness | nindent 12 }}
          {{- end }}
          resources:
            {{- toYaml $cfg.resources | nindent 12 }}
          volumeMounts:
            - name: tls
              mountPath: /certs
              readOnly: {{ ne $root.Values.tls.mode "selfSignedInit" }}
            {{- /* read-only rootfs: give the daemon a writable tmp scratch. */}}
            - name: tmp
              mountPath: /tmp
      volumes:
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
        - name: tmp
          emptyDir: {}
      {{- with $cfg.nodeSelector }}
      nodeSelector:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      {{- with $cfg.affinity }}
      affinity:
        {{- toYaml . | nindent 8 }}
      {{- end }}
      {{- with $cfg.tolerations }}
      tolerations:
        {{- toYaml . | nindent 8 }}
      {{- end }}
{{- end -}}

{{- define "meridian.daemon.service" -}}
{{- $root := .root -}}
{{- $daemon := .daemon -}}
{{- $cfg := .cfg -}}
{{- $fullname := printf "%s-%s" (include "meridian.fullname" $root) $daemon -}}
apiVersion: v1
kind: Service
metadata:
  name: {{ $fullname }}
  labels:
    {{- include "meridian.labels" $root | nindent 4 }}
    app.kubernetes.io/component: {{ $daemon }}
  {{- with $cfg.service.annotations }}
  annotations:
    {{- toYaml . | nindent 4 }}
  {{- end }}
spec:
  type: {{ $cfg.service.type | default "ClusterIP" }}
  selector:
    {{- include "meridian.selectorLabels" $root | nindent 4 }}
    app.kubernetes.io/component: {{ $daemon }}
  ports:
    - name: game
      port: {{ $cfg.service.port | default $cfg.port }}
      targetPort: game
      protocol: TCP
      {{- if and (eq ($cfg.service.type | default "ClusterIP") "NodePort") $cfg.service.nodePort }}
      nodePort: {{ $cfg.service.nodePort }}
      {{- end }}
    {{- if $root.Values.observability.enabled }}
    - name: metrics
      port: {{ $root.Values.observability.metricsPort }}
      targetPort: metrics
      protocol: TCP
    {{- end }}
{{- end -}}
