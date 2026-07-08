#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/build-client.sh — build ONLY the Godot client GDExtension (Apple-
# Silicon-only, SAD §9.6) and install the framework into client/project/bin/, which
# is what run-client.sh dlopens (#300).
#
# WHY A DEDICATED SCRIPT: run-client.sh does NOT build — it just boots the editor
# against the prebuilt framework. After changing ANY client C++ (client/net/**,
# client/gdextension/**) you MUST rebuild the framework or the running client keeps
# the stale binary (a moving-target trap: the .gd scripts reload from source, but the
# native MeridianNetThread / clientnet codec do not). GDScript-only changes need NO
# rebuild. This is the same recipe as `build.sh --client`, isolated so you can rebuild
# just the client quickly without the server/mcc.
#
# Usage:
#   scripts/dev/build-client.sh                    # editor variant (what run-client loads)
#   scripts/dev/build-client.sh --target both      # editor + template_debug (exported game)
#   scripts/dev/build-client.sh --clean            # wipe the client build tree first
#   scripts/dev/build-client.sh --jobs 4           # cap parallelism
#
# Then:  scripts/dev/run-client.sh --scene res://scenes/login/login_screen.tscn
set -euo pipefail

# shellcheck source=scripts/dev/_common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

TARGET="editor"        # run-client.sh + the editor dlopen libmeridian.macos.editor.framework (#300)
CLEAN=0
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
  case "$1" in
    --target=*) TARGET="${1#*=}" ;;
    --target)   shift; TARGET="${1:?--target needs a value}" ;;
    --clean)    CLEAN=1 ;;
    --jobs=*)   JOBS="${1#*=}" ;;
    --jobs)     shift; JOBS="${1:?--jobs needs a number}" ;;
    -h|--help)  grep '^#' "$0" | sed 's/^#\{1,\} \{0,1\}//'; exit 0 ;;
    *)          die "unknown arg: $1 (see --help)" ;;
  esac
done

case "$TARGET" in
  editor|template_debug|both) ;;
  *) die "unknown --target '$TARGET' (expected: editor | template_debug | both)" ;;
esac

[ "$(uname -m)" = "arm64" ] || die "client build is Apple-Silicon-only (SAD §9.6); host is $(uname -m)."
[ -f "${REPO_ROOT}/client/godot-cpp/CMakeLists.txt" ] \
  || die "godot-cpp submodule missing. Run: scripts/dev/setup-macos.sh --client"

setup_cmake_env   # exports PKG_CONFIG_PATH + CMAKE_PREFIX_PATH (OpenSSL, etc.)

# Build one godot-cpp target into its OWN build tree (a shared tree would reconfigure
# the target each time) and verify the framework binary landed in client/project/bin/.
build_one() {
  local target="$1"
  local btree="${BUILD_ROOT}/client-${target}"
  if [ "$CLEAN" -eq 1 ]; then
    log "Cleaning ${btree}"
    rm -rf "${btree}"
  fi
  log "Configuring client GDExtension (${target}) → ${btree}"
  cmake -S "${REPO_ROOT}/client" -B "${btree}" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DGODOTCPP_TARGET="${target}" \
    >/dev/null || die "client configure failed (${target})"
  log "Building client ${target} (-j${JOBS}) — compiles godot-cpp on first run, can take a while"
  cmake --build "${btree}" -j "${JOBS}" || die "client build failed (${target})"
  local fw="${REPO_ROOT}/client/project/bin/libmeridian.macos.${target}.framework"
  [ -f "${fw}/libmeridian.macos.${target}" ] || die "expected framework not produced: ${fw}"
  ok "client GDExtension (${target}) → ${fw}"
}

case "$TARGET" in
  both) build_one editor; build_one template_debug ;;
  *)    build_one "$TARGET" ;;
esac

echo
ok "Client GDExtension built → client/project/bin/"
echo "  Next: scripts/dev/run-client.sh --scene res://scenes/login/login_screen.tscn"
