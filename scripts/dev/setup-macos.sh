#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/setup-macos.sh — one-time native macOS dev environment setup (#223).
#
# Installs (via Homebrew) and verifies the toolchain the server/mcc/tests need,
# sets up git-lfs, and (with --client) inits the pinned godot-cpp submodule for
# the GDExtension build. Idempotent: safe to re-run; only installs what's missing.
#
# This is the LOCAL substitute for a macOS CI runner (that runner is #61 / A-16,
# not yet provisioned). See docs/BUILDING-MACOS.md for the full picture.
#
# Usage:
#   scripts/dev/setup-macos.sh            # server + mcc + tests toolchain
#   scripts/dev/setup-macos.sh --client   # also init godot-cpp submodule (#158)
#   scripts/dev/setup-macos.sh --help
set -euo pipefail

# shellcheck source=scripts/dev/_common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

WITH_CLIENT=0
for arg in "$@"; do
  case "$arg" in
    --client) WITH_CLIENT=1 ;;
    -h|--help)
      grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'
      exit 0 ;;
    *) die "unknown argument '$arg' (try --help)" ;;
  esac
done

require_macos
resolve_brew
ok "Homebrew at ${BREW_PREFIX} ($(uname -m))"

# --- Homebrew formulae the recipe needs. -------------------------------------
# server/mcc build + local DB run: cmake ninja openssl@3 flatbuffers mariadb.
# assets: git-lfs. Python validator/tests: uv (astral).
BREW_FORMULAE=(cmake ninja openssl@3 flatbuffers mariadb git-lfs uv)

log "Checking Homebrew formulae: ${BREW_FORMULAE[*]}"
to_install=()
for f in "${BREW_FORMULAE[@]}"; do
  if brew list --versions "$f" >/dev/null 2>&1; then
    ok "$f $(brew list --versions "$f" | awk '{print $2}')"
  else
    warn "$f not installed — will install"
    to_install+=("$f")
  fi
done

if [ "${#to_install[@]}" -gt 0 ]; then
  log "Installing: ${to_install[*]}"
  brew install "${to_install[@]}"
else
  ok "All Homebrew formulae already present."
fi

# --- Verify the tools are actually on PATH / usable. -------------------------
log "Verifying tool versions"
check_tool() {
  local name="$1" cmd="$2"
  if command -v "$cmd" >/dev/null 2>&1; then
    ok "$name — $("$cmd" --version 2>&1 | head -1)"
  else
    die "$name ($cmd) not found on PATH after install. Check 'brew doctor'."
  fi
}
check_tool "cmake"       cmake
check_tool "ninja"       ninja
check_tool "flatc"       flatc
check_tool "uv"          uv
check_tool "git-lfs"     git-lfs
check_tool "mariadbd"    mariadbd
check_tool "openssl"     openssl
check_tool "pkg-config"  pkg-config
# mariadb-install-db has no --version flag; just confirm it is on PATH.
command -v mariadb-install-db >/dev/null 2>&1 \
  && ok "mariadb-install-db — $(command -v mariadb-install-db)" \
  || die "mariadb-install-db not found on PATH. Is 'mariadb' installed?"

# libmariadb pkg-config must be discoverable with the recipe's PKG_CONFIG_PATH.
setup_cmake_env
if pkg-config --exists libmariadb; then
  ok "libmariadb via pkg-config ($(pkg-config --modversion libmariadb))"
else
  die "libmariadb not found via pkg-config even with PKG_CONFIG_PATH=${PKG_CONFIG_PATH}. Is 'mariadb' installed?"
fi

# --- git-lfs one-time install (assets). --------------------------------------
log "Setting up git-lfs"
git -C "${REPO_ROOT}" lfs install --local >/dev/null 2>&1 && ok "git-lfs hooks installed (local)" \
  || warn "git lfs install failed (non-fatal; only needed for LFS-tracked assets)"

# --- Python toolchain for the validator/tests. -------------------------------
log "Syncing Python env (uv)"
( cd "${REPO_ROOT}" && uv sync --group dev >/dev/null 2>&1 ) \
  && ok "uv env synced ($(cd "${REPO_ROOT}" && uv run python --version 2>&1))" \
  || warn "uv sync failed — the Python suite (test.sh) may not run until resolved"

# --- Optional: client GDExtension prerequisites (#158). ----------------------
if [ "$WITH_CLIENT" -eq 1 ]; then
  [ "$(uname -m)" = "arm64" ] || warn "client target is Apple-Silicon-only (SAD §9.6); this host is $(uname -m)."
  log "Initializing pinned godot-cpp submodule (client GDExtension, #158)"
  if [ -f "${REPO_ROOT}/scripts/fetch-deps.sh" ]; then
    ( cd "${REPO_ROOT}" && bash scripts/fetch-deps.sh --submodule-only ) \
      && ok "godot-cpp submodule ready" \
      || die "fetch-deps.sh --submodule-only failed"
  else
    ( cd "${REPO_ROOT}" && git submodule update --init --recursive client/godot-cpp ) \
      && ok "godot-cpp submodule ready" \
      || die "git submodule update failed"
  fi
  ok "Client prerequisites ready. Build with: scripts/dev/build.sh --client"
fi

echo
ok "Setup complete. Next: scripts/dev/build.sh   (then scripts/dev/test.sh)"
