#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build the restored Codex solution and reject warning text from any tool layer.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Warning diagnostics emitted by the supported toolchain layers. Keep this
# narrower than a raw "warning" substring so branch/file names such as
# story-720-codex-warning-clean and MSBuild's "0 Warning(s)" summary pass.
WARNING_PATTERN='(^|[[:space:]:])([[:alnum:]_]*Warning:|warning([[:space:]]+(CS|MSB|NU|NETSDK)[0-9]+|:)|CMake([[:space:]]+Deprecation)?[[:space:]]+Warning([[:space:]]+\(dev\))?([[:space:]]+at|:))|^WARN([[:space:]]|:)'

assert_warning_clean() {
  local output="$1"
  [ -r "$output" ] || { echo "Codex warning-clean assertion failed: cannot read $output" >&2; return 2; }
  if LC_ALL=C grep -Eiq "$WARNING_PATTERN" "$output"; then
    echo "Codex warning-clean assertion failed: warning output detected." >&2
    return 1
  fi
  echo "Codex warning-clean assertion passed."
}

# Input-only mode makes the classifier independently regression-testable without
# replacing dotnet or running a build. It is also useful when inspecting a saved
# CI build log.
if [ "${1:-}" = "--scan-output" ]; then
  [ "$#" -eq 2 ] || { echo "usage: $0 --scan-output <build-log>" >&2; exit 2; }
  assert_warning_clean "$2"
  exit $?
fi

OUTPUT="$(mktemp "${TMPDIR:-/tmp}/meridian-codex-build.XXXXXX")"
trap 'rm -f "$OUTPUT"' EXIT

set +e
dotnet build "${SCRIPT_DIR}/Meridian.Codex.sln" \
  --no-restore -m:1 --disable-build-servers "$@" 2>&1 | tee "$OUTPUT"
status=${PIPESTATUS[0]}
set -e

if [ "$status" -ne 0 ]; then
  exit "$status"
fi

assert_warning_clean "$OUTPUT"
