#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/dev/run-client.sh — boot the Meridian Godot client on macOS under a
# selectable rendering backend (issue #115).
#
# macOS ships on the **native Metal** backend (TD-02 / D-28) — that is the
# default here and what CI/testers use. Godot 4.7 also keeps a **Vulkan
# (MoltenVK)** backend buildable as a *diagnostic fallback* (the escape-hatch
# mirror of the Windows Vulkan fallback): when the Metal path misbehaves, boot
# the exact same test map on MoltenVK to tell a Metal-backend bug apart from an
# engine/content bug. This is NEVER the shipped default — Metal stays default.
#
# It boots the world test map the client enters after char-select
# (res://scenes/world/camera_demo.tscn) under the chosen `--rendering-driver`.
#
# Usage:
#   scripts/dev/run-client.sh                       # Metal (default), interactive
#   scripts/dev/run-client.sh --renderer=vulkan     # Vulkan/MoltenVK fallback
#   scripts/dev/run-client.sh --renderer=metal      # native Metal (explicit)
#   scripts/dev/run-client.sh --renderer=vulkan --verify   # automated boot proof
#                                                          # (+ screenshot, exits 0/1)
#   scripts/dev/run-client.sh --scene res://path.tscn      # boot a different scene
#   scripts/dev/run-client.sh --reseed              # force-reseed .godot/ then run
#   scripts/dev/run-client.sh --help
#
# Backends (macOS `--display-driver macos`): metal | vulkan | opengl3.
#
# #283 note: the `--headless` editor import aborts inside MoltenVK
# (SPIRVToMSLConverter) on macOS, and `--rendering-driver` is IGNORED under
# headless. So this script seeds `.godot/` with a one-time WINDOWED import (a real
# GPU device — exits 0 cleanly) and always runs the client windowed. Do NOT add
# --headless here.
#
# Requires a prior GDExtension build (client/project/bin/ populated):
#   cd client && cmake -B build -DGODOTCPP_TARGET=editor && cmake --build build -j
set -euo pipefail

# shellcheck source=scripts/dev/_common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

RENDERER="metal"          # macOS default is native Metal (TD-02 / D-28). Keep it.
SCENE="res://scenes/world/camera_demo.tscn"
VERIFY=0
RESEED=0
QUIT_AFTER=0              # 0 = run until the user closes the window (interactive).

while [ $# -gt 0 ]; do
  case "$1" in
    --renderer=*) RENDERER="${1#*=}" ;;
    --renderer)   shift; RENDERER="${1:?--renderer needs a value}" ;;
    --scene=*)    SCENE="${1#*=}" ;;
    --scene)      shift; SCENE="${1:?--scene needs a value}" ;;
    --verify)     VERIFY=1 ;;
    --reseed)     RESEED=1 ;;
    -h|--help)
      grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'
      exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
  shift
done

require_macos

case "$RENDERER" in
  metal|vulkan|opengl3) ;;
  *) die "unknown --renderer '$RENDERER' (expected: metal | vulkan | opengl3)" ;;
esac

PROJECT="${REPO_ROOT}/client/project"
[ -f "${PROJECT}/project.godot" ] || die "client project not found at ${PROJECT}"

# --- Resolve the pinned Godot 4.7 editor. ------------------------------------
# Order: $GODOT override → client/.godot-bin (fetch-deps.sh) → /Applications →
# PATH. The editor binary runs the project directly (no export template needed
# for a dev boot); it must match client/ENGINE_VERSION (GODOT_VERSION).
resolve_godot() {
  if [ -n "${GODOT:-}" ] && [ -x "${GODOT}" ]; then echo "${GODOT}"; return; fi
  local c
  for c in \
    "${REPO_ROOT}/client/.godot-bin/Godot.app/Contents/MacOS/Godot" \
    "/Applications/Godot.app/Contents/MacOS/Godot" \
    "${HOME}/Applications/Godot.app/Contents/MacOS/Godot"; do
    [ -x "$c" ] && { echo "$c"; return; }
  done
  command -v godot 2>/dev/null && return
  die "Godot editor not found. Set \$GODOT, or fetch the pinned engine: scripts/fetch-deps.sh"
}
GODOT_BIN="$(resolve_godot)"

WANT_VERSION="$(grep -E '^GODOT_VERSION=' "${REPO_ROOT}/client/ENGINE_VERSION" | cut -d= -f2)"
GOT_VERSION="$("$GODOT_BIN" --version 2>/dev/null | head -1)"
case "$GOT_VERSION" in
  ${WANT_VERSION%-*}*) ok "Godot ${GOT_VERSION} (pin: ${WANT_VERSION}) at ${GODOT_BIN}" ;;
  *) warn "Godot at ${GODOT_BIN} reports '${GOT_VERSION}', pin is '${WANT_VERSION}' — continuing" ;;
esac

# --- Preflight: the compiled GDExtension must be present (map uses C++ nodes). -
if ! ls "${PROJECT}/bin/"libmeridian.macos.*.framework >/dev/null 2>&1; then
  die "GDExtension not built. Run: (cd client && cmake -B build -DGODOTCPP_TARGET=editor && cmake --build build -j)"
fi

# --- Seed .godot/ with a one-time WINDOWED import (never headless, #283). -----
# A cold import registers the extension + imports resources; on macOS the
# headless import aborts in MoltenVK (#283), but a windowed real-device import
# exits 0. We seed under Metal (the default device) regardless of the run
# backend — the cache is backend-neutral.
if [ "$RESEED" -eq 1 ]; then
  log "Reseeding: removing ${PROJECT}/.godot/"
  rm -rf "${PROJECT}/.godot"
fi
if [ ! -f "${PROJECT}/.godot/extension_list.cfg" ]; then
  log "Seeding .godot/ via one-time WINDOWED editor import (avoids #283 headless MoltenVK abort)"
  "$GODOT_BIN" --rendering-driver metal --editor --quit --path "$PROJECT" >/dev/null 2>&1 \
    || die "windowed editor import failed (see run without redirection)"
  ok ".godot/ seeded"
fi

# --- Boot. -------------------------------------------------------------------
if [ "$VERIFY" -eq 1 ]; then
  QUIT_AFTER=120
  log "VERIFY: booting ${SCENE} under --rendering-driver ${RENDERER} (windowed, ${QUIT_AFTER} frames)"
  exec "$GODOT_BIN" --rendering-driver "$RENDERER" --path "$PROJECT" \
    --script res://scenes/world/renderer_boot_verify.gd --quit-after "$QUIT_AFTER"
fi

log "Booting client test map ${SCENE} under --rendering-driver ${RENDERER}"
ok "Metal is the shipped default; '${RENDERER}' selected for this run."
exec "$GODOT_BIN" --rendering-driver "$RENDERER" --path "$PROJECT" "$SCENE"
