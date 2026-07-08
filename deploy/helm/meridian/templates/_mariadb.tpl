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
