#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/build.sh — native macOS build of the Meridian stack (#223).
#
# Builds, using the known-good recipe (docs/BUILDING-MACOS.md):
#   * server  → authd, worldd, meridian-account (+ ctest test binaries)
#   * mcc     → the Meridian Content Compiler
#   * client  → the GDExtension (only with --client; Apple-Silicon-only, #158)
#
# All build trees live under $REPO_ROOT/build/ (gitignored). Fails loudly with a
# pointer to setup-macos.sh if a dependency is missing.
#
# Usage:
#   scripts/dev/build.sh                 # server + mcc
#   scripts/dev/build.sh --client        # + client GDExtension
#   scripts/dev/build.sh --clean         # wipe build/ first
#   scripts/dev/build.sh --jobs N        # parallelism (default: sysctl hw.ncpu)
#   scripts/dev/build.sh --help
set -euo pipefail

# shellcheck source=scripts/dev/_common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

WITH_CLIENT=0
CLEAN=0
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
  case "$1" in
    --client) WITH_CLIENT=1 ;;
    --clean)  CLEAN=1 ;;
    --jobs)   shift; JOBS="${1:?--jobs needs a number}" ;;
    -h|--help)
      grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'
      exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
  shift
done

require_macos
setup_cmake_env   # exports PKG_CONFIG_PATH + CMAKE_PREFIX_PATH

# --- Preflight: fail loudly on missing deps, don't let CMake do it cryptically.
log "Preflight dependency check"
missing=0
need() {
  if command -v "$1" >/dev/null 2>&1; then ok "$1"; else warn "$1 MISSING"; missing=1; fi
}
need cmake; need ninja; need flatc; need pkg-config
if ! pkg-config --exists libmariadb; then
  warn "libmariadb not found via pkg-config"; missing=1
fi
if [ "$missing" -ne 0 ]; then
  die "missing build prerequisites. Run: scripts/dev/setup-macos.sh"
fi
ok "Recipe env: PKG_CONFIG_PATH=${PKG_CONFIG_PATH}"
ok "Recipe env: CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"

if [ "$CLEAN" -eq 1 ]; then
  log "Cleaning ${BUILD_ROOT}"
  rm -rf "${BUILD_ROOT}"
fi
mkdir -p "${BUILD_ROOT}"

# --- Server (authd, worldd, meridian-account). -------------------------------
log "Configuring server → ${BUILD_ROOT}/server"
cmake -S "${REPO_ROOT}/server" -B "${BUILD_ROOT}/server" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  || die "server configure failed"
log "Building server (-j${JOBS})"
cmake --build "${BUILD_ROOT}/server" -j "${JOBS}" || die "server build failed"

# Verify the three daemons/CLI actually got produced. CMake nests each target's
# output under its own subdir (server/authd/authd, server/worldd/worldd,
# server/tools/meridian-account/meridian-account) — check the real files, not
# the same-named parent dirs.
for bin in authd/authd worldd/worldd tools/meridian-account/meridian-account; do
  path="${BUILD_ROOT}/server/${bin}"
  { [ -f "$path" ] && [ -x "$path" ]; } || die "expected server binary not produced: ${bin}"
done
ok "server binaries: authd, worldd, meridian-account"

# --- mcc. --------------------------------------------------------------------
log "Configuring mcc → ${BUILD_ROOT}/mcc"
cmake -S "${REPO_ROOT}/tools/mcc" -B "${BUILD_ROOT}/mcc" -G Ninja \
  || die "mcc configure failed"
log "Building mcc (-j${JOBS})"
cmake --build "${BUILD_ROOT}/mcc" -j "${JOBS}" || die "mcc build failed"
[ -x "${BUILD_ROOT}/mcc/mcc" ] || die "mcc binary not produced"
ok "mcc binary: ${BUILD_ROOT}/mcc/mcc"

# --- Client GDExtension (optional, Apple-Silicon-only). ----------------------
if [ "$WITH_CLIENT" -eq 1 ]; then
  [ "$(uname -m)" = "arm64" ] || die "client build is Apple-Silicon-only (SAD §9.6); host is $(uname -m)."
  if [ ! -f "${REPO_ROOT}/client/godot-cpp/CMakeLists.txt" ]; then
    die "godot-cpp submodule missing. Run: scripts/dev/setup-macos.sh --client"
  fi
  log "Configuring client GDExtension → ${BUILD_ROOT}/client"
  cmake -S "${REPO_ROOT}/client" -B "${BUILD_ROOT}/client" \
    -DGODOTCPP_TARGET=template_debug \
    || die "client configure failed"
  log "Building client (-j${JOBS}) — this compiles godot-cpp, can take a while"
  cmake --build "${BUILD_ROOT}/client" -j "${JOBS}" || die "client build failed"
  ok "client GDExtension built → client/project/bin/"
fi

echo
ok "Build complete."
echo "  Daemons : ${BUILD_ROOT}/server/authd/authd, ${BUILD_ROOT}/server/worldd/worldd"
echo "  Account : ${BUILD_ROOT}/server/tools/meridian-account/meridian-account"
echo "  mcc     : ${BUILD_ROOT}/mcc/mcc"
echo "  Next    : scripts/dev/test.sh        (DB-free suite)"
echo "            scripts/dev/test.sh --db    (spins up a throwaway MariaDB)"
echo "            scripts/dev/run-local.sh --smoke"
