#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build the restored Codex solution and reject warning text from any tool layer.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Warning diagnostics emitted by the supported toolchain layers. Structured
# .NET source diagnostics accept compiler and analyzer code families without
# treating arbitrary prose, branch/file names, or MSBuild's "0 Warning(s)"
# summary as warnings.
WARNING_PATTERN='(^|[[:space:]:])([[:alnum:]_]*Warning:|warning([[:space:]]+(CS|MSB|NU|NETSDK)[0-9]+[[:space:]]*:|:)|CMake([[:space:]]+Deprecation)?[[:space:]]+Warning([[:space:]]+\(dev\))?([[:space:]]+at|:))|\.(cs|fs|vb)(\([0-9]+(,[0-9]+){1,3}\))?[[:space:]]*:[[:space:]]*warning[[:space:]]+[[:alpha:]][[:alnum:]]*[0-9][[:alnum:]]*[[:space:]]*:|^WARN([[:space:]]|:)'

assert_warning_clean() {
  local output="$1"
  local scanner_status

  if [ ! -f "$output" ] || [ ! -r "$output" ]; then
    echo "Codex warning-clean assertion failed: input is not a readable regular file: $output" >&2
    return 2
  fi

  if LC_ALL=C grep -Eiq "$WARNING_PATTERN" "$output"; then
    scanner_status=0
  else
    scanner_status=$?
  fi

  case "$scanner_status" in
    0)
      echo "Codex warning-clean assertion failed: warning output detected." >&2
      return 1
      ;;
    1)
      echo "Codex warning-clean assertion passed."
      ;;
    *)
      echo "Codex warning-clean assertion failed: warning scanner exited with status $scanner_status." >&2
      return 2
      ;;
  esac
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
