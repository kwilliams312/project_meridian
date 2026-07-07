# SPDX-License-Identifier: Apache-2.0
# shellcheck shell=bash
#
# scripts/dev/_common.sh — shared helpers for the native macOS dev toolkit (#223).
#
# Sourced by setup-macos.sh / build.sh / test.sh / run-local.sh. Not executable
# on its own. Provides: colored logging, repo-root discovery, Homebrew prefix
# resolution, and the common CMake env (PKG_CONFIG_PATH + CMAKE_PREFIX_PATH) that
# the known-good recipe in docs/BUILDING-MACOS.md depends on.

# --- Repo root (two levels up from scripts/dev/). ----------------------------
# Resolve regardless of caller CWD or symlinks.
_common_self="${BASH_SOURCE[0]}"
REPO_ROOT="$(cd "$(dirname "${_common_self}")/../.." && pwd)"
export REPO_ROOT

# All build trees live under $REPO_ROOT/build (gitignored). One place to change.
BUILD_ROOT="${REPO_ROOT}/build"
export BUILD_ROOT

# --- Logging. ----------------------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  _C_RESET=$'\033[0m'; _C_BOLD=$'\033[1m'
  _C_BLUE=$'\033[34m'; _C_GREEN=$'\033[32m'; _C_YELLOW=$'\033[33m'; _C_RED=$'\033[31m'
else
  _C_RESET=''; _C_BOLD=''; _C_BLUE=''; _C_GREEN=''; _C_YELLOW=''; _C_RED=''
fi

log()  { printf '%s==>%s %s\n' "${_C_BLUE}${_C_BOLD}" "${_C_RESET}" "$*"; }
ok()   { printf '%s  ✓%s %s\n' "${_C_GREEN}" "${_C_RESET}" "$*"; }
warn() { printf '%s  !%s %s\n' "${_C_YELLOW}" "${_C_RESET}" "$*" >&2; }
die()  { printf '%s  ✗ %s%s\n' "${_C_RED}${_C_BOLD}" "$*" "${_C_RESET}" >&2; exit 1; }

# --- Platform + Homebrew guard. ----------------------------------------------
require_macos() {
  [ "$(uname -s)" = "Darwin" ] || die "this toolkit targets macOS (uname -s = $(uname -s)). On Linux use the Docker path in deploy/docker/."
}

# Locate Homebrew and export BREW_PREFIX. Fails loudly if brew is missing.
resolve_brew() {
  if command -v brew >/dev/null 2>&1; then
    BREW_PREFIX="$(brew --prefix)"
  elif [ -x /opt/homebrew/bin/brew ]; then
    BREW_PREFIX="$(/opt/homebrew/bin/brew --prefix)"
    eval "$(/opt/homebrew/bin/brew shellenv)"
  else
    die "Homebrew not found. Install it from https://brew.sh, then run scripts/dev/setup-macos.sh"
  fi
  export BREW_PREFIX
}

# brew --prefix <formula> without shelling out per-lookup where we can avoid it.
brew_prefix_of() { brew --prefix "$1" 2>/dev/null; }

# --- The known-good CMake environment (see docs/BUILDING-MACOS.md). ----------
# libmariadb ships a pkg-config file, not a CMake config → PKG_CONFIG_PATH.
# openssl@3 and flatbuffers are keg-only / find_package(CONFIG) → CMAKE_PREFIX_PATH.
# Exported so `cmake` inherits them; callers just invoke cmake normally.
setup_cmake_env() {
  resolve_brew
  local mariadb_pc openssl_pfx flatc_pfx
  mariadb_pc="$(brew_prefix_of mariadb)/lib/pkgconfig"
  openssl_pfx="$(brew_prefix_of openssl@3)"
  flatc_pfx="$(brew_prefix_of flatbuffers)"

  export PKG_CONFIG_PATH="${mariadb_pc}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  export CMAKE_PREFIX_PATH="${openssl_pfx};${flatc_pfx}${CMAKE_PREFIX_PATH:+;${CMAKE_PREFIX_PATH}}"
}
