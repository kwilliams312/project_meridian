#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build the restored Codex solution and reject warning text from any tool layer.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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

if grep -Eiq '(RuntimeWarning|(^|[[:space:]:])warning([[:space:]]+(CS|MSB|NU|NETSDK)[0-9]+|:))' "$OUTPUT"; then
  echo "Codex warning-clean assertion failed: warning output detected." >&2
  exit 1
fi

echo "Codex warning-clean assertion passed."
