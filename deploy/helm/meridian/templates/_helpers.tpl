{{/* SPDX-License-Identifier: Apache-2.0 */}}
{{/*
  Shared template helpers for the Project Meridian chart (D-30 §10).
  Naming/labels follow the Helm chart best-practices + kubernetes recommended
  labels (app.kubernetes.io/*).
*/}}

{{/* Base name, truncated to the 63-char DNS limit. */}}
{{- define "meridian.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/*
  Fully-qualified app name: <release>-<chart>, or fullnameOverride if set.
  Truncated to 63 chars (leaving room for the per-daemon suffix appended below).
*/}}
{{- define "meridian.fullname" -}}
{{- if .Values.fullnameOverride -}}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- $name := default .Chart.Name .Values.nameOverride -}}
{{- if contains $name .Release.Name -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}
{{- end -}}

{{/* Chart name + version, for the helm.sh/chart label. */}}
{{- define "meridian.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/* Selector labels — the stable subset (must NOT change across upgrades). */}}
{{- define "meridian.selectorLabels" -}}
app.kubernetes.io/name: {{ include "meridian.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end -}}

{{/* Common labels applied to every object. */}}
{{- define "meridian.labels" -}}
helm.sh/chart: {{ include "meridian.chart" . }}
{{ include "meridian.selectorLabels" . }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/part-of: meridian
{{- with .Values.commonLabels }}
{{ toYaml . }}
{{- end }}
{{- end -}}

{{/* Common annotations. */}}
{{- define "meridian.annotations" -}}
{{- with .Values.commonAnnotations }}
{{ toYaml . }}
{{- end }}
{{- end -}}

{{/*
  Resolve a daemon image reference.
  Usage: include "meridian.image" (dict "root" $ "daemon" "authd" "cfg" .Values.authd)
  - per-daemon image.repository overrides the derived ${repository}/${daemon}
  - per-daemon image.tag overrides the global image.tag
*/}}
{{- define "meridian.image" -}}
{{- $root := .root -}}
{{- $cfg := .cfg -}}
{{- $daemon := .daemon -}}
{{- $registry := $root.Values.image.registry -}}
{{- $repo := "" -}}
{{- if and $cfg.image $cfg.image.repository -}}
{{- $repo = $cfg.image.repository -}}
{{- else -}}
{{- $repo = printf "%s/%s" $root.Values.image.repository $daemon -}}
{{- end -}}
{{- $tag := $root.Values.image.tag -}}
{{- if and $cfg.image $cfg.image.tag -}}
{{- $tag = $cfg.image.tag -}}
{{- end -}}
{{- if $registry -}}
{{- printf "%s/%s:%s" $registry $repo $tag -}}
{{- else -}}
{{- printf "%s:%s" $repo $tag -}}
{{- end -}}
{{- end -}}

{{/* Name of the ConfigMap holding non-secret DB connection config. */}}
{{- define "meridian.configMapName" -}}
{{- printf "%s-config" (include "meridian.fullname" .) -}}
{{- end -}}

{{/* Name of the Secret holding the DB password (chart-created variant). */}}
{{- define "meridian.dbSecretName" -}}
{{- if .Values.db.existingSecret -}}
{{- .Values.db.existingSecret -}}
{{- else -}}
{{- printf "%s-db" (include "meridian.fullname" .) -}}
{{- end -}}
{{- end -}}

{{/* Key within the DB secret holding the password. */}}
{{- define "meridian.dbSecretKey" -}}
{{- if .Values.db.existingSecret -}}
{{- .Values.db.passwordKey -}}
{{- else -}}
MERIDIAN_DB_PASS
{{- end -}}
{{- end -}}

{{/* Name of the TLS secret to mount (chart-created or existing). */}}
{{- define "meridian.tlsSecretName" -}}
{{- if eq .Values.tls.mode "create" -}}
{{- printf "%s-tls" (include "meridian.fullname" .) -}}
{{- else -}}
{{- required "tls.existingSecret is required when tls.mode=existingSecret" .Values.tls.existingSecret -}}
{{- end -}}
{{- end -}}

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
