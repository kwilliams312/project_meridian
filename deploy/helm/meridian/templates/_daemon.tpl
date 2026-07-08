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
          args:
            - --cert={{ printf "/certs/%s" $root.Values.tls.certKey }}
            - --key={{ printf "/certs/%s" $root.Values.tls.keyKey }}
            - --bind={{ $cfg.bindAddress | default "0.0.0.0" }}
            - --port={{ $cfg.port }}
            {{- if $root.Values.observability.enabled }}
            - --metrics-port={{ $root.Values.observability.metricsPort }}
            - --metrics-bind={{ $root.Values.observability.metricsBind }}
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
    {{- if $root.Values.observability.enabled }}
    - name: metrics
      port: {{ $root.Values.observability.metricsPort }}
      targetPort: metrics
      protocol: TCP
    {{- end }}
{{- end -}}
