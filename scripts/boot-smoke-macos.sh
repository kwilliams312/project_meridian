#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/boot-smoke-macos.sh — macOS client boot smoke (issue #114).
#
# The minimum "the exported client actually runs on a Mac" gate: launch the
# client, prove it reaches its MAIN SCENE (the login screen — project.godot
# run/main_scene) without crashing, and exit 0. Success is asserted from TWO
# signals emitted by the boot: the deterministic marker line the login scene
# prints on _ready() —
#
#     [boot] MERIDIAN_BOOT_OK scene=login adapter="…" api="…"
#
# (see client/project/scenes/login/login_screen.gd) — plus a clean process exit.
#
# TWO boot paths, chosen by --driver:
#
#   metal  (default) — a WINDOWED boot on the native Metal backend (the shipped
#          default, TD-02 / D-28). Uses a REAL GPU device, so the marker's
#          adapter=… is non-empty; the smoke additionally asserts that. This is
#          the real "boots on Metal" gate. It needs a window-server session (a
#          logged-in GUI / Aqua session) and a real Metal GPU — both true on the
#          self-hosted M2 Max runner.
#
#   headless — a `--headless` boot on Godot's DUMMY rendering driver. This is the
#          window-server-free fallback: it proves the client boots and reaches
#          the main scene without a crash, but it does NOT exercise Metal
#          (`--headless` ignores --rendering-driver and uses the dummy backend —
#          issue #283). adapter=… is empty here, so Metal is NOT asserted.
#
# TWO targets:
#
#   --app <ProjectMeridian.app>  — boot the #113 EXPORTED bundle. The exporter
#          bakes all resources into the .pck, so there is NO editor/import step
#          and NO .godot/ seed needed — the export path sidesteps the #283
#          MoltenVK cold-import abort entirely. This is what CI runs against the
#          client-export artifact.
#
#   (no --app)                   — boot client/project directly via the pinned
#          Godot editor (a dev-box convenience). Needs the editor GDExtension
#          framework built (client/project/bin/) and a seeded .godot/ (seeded
#          once WINDOWED, never headless, per #283 — pass --seed to do it).
#
# Usage:
#   scripts/boot-smoke-macos.sh --app build/export/macos/ProjectMeridian.app
#   scripts/boot-smoke-macos.sh --app <app> --driver headless
#   scripts/boot-smoke-macos.sh                       # project boot, Metal, windowed
#   scripts/boot-smoke-macos.sh --seed                # project boot, seed .godot/ first
#   scripts/boot-smoke-macos.sh --frames 120 --help
#
# Exits 0 on a clean boot-to-main-scene (+ real Metal device in metal mode),
# 1 on any failed assertion.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$REPO_ROOT/client/project"
BIN_DIR="$REPO_ROOT/client/.godot-bin"
PIN_FILE="$REPO_ROOT/client/ENGINE_VERSION"

# --- Minimal self-contained logging (no dev-toolkit dependency). -------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_RESET=$'\033[0m'; C_B=$'\033[1m'; C_BLUE=$'\033[34m'; C_GREEN=$'\033[32m'; C_YEL=$'\033[33m'; C_RED=$'\033[31m'
else C_RESET=''; C_B=''; C_BLUE=''; C_GREEN=''; C_YEL=''; C_RED=''; fi
log()  { printf '%s==>%s %s\n' "${C_BLUE}${C_B}" "${C_RESET}" "$*"; }
ok()   { printf '%s  ok%s %s\n' "${C_GREEN}" "${C_RESET}" "$*"; }
warn() { printf '%s  ! %s%s\n' "${C_YEL}" "$*" "${C_RESET}" >&2; }
die()  { printf '%s  x %s%s\n' "${C_RED}${C_B}" "$*" "${C_RESET}" >&2; exit 1; }

# --- Defaults + arg parsing. -------------------------------------------------
APP=""
DRIVER="metal"        # metal (windowed, real device) | headless (dummy driver)
FRAMES=90             # quit after N frames — enough to reach _ready + render.
GODOT_BIN="${GODOT:-}"
SEED=0

while [ $# -gt 0 ]; do
  case "$1" in
    --app)     APP="$2"; shift 2 ;;
    --app=*)   APP="${1#*=}"; shift ;;
    --driver)  DRIVER="$2"; shift 2 ;;
    --driver=*) DRIVER="${1#*=}"; shift ;;
    --frames)  FRAMES="$2"; shift 2 ;;
    --frames=*) FRAMES="${1#*=}"; shift ;;
    --godot)   GODOT_BIN="$2"; shift 2 ;;
    --seed)    SEED=1; shift ;;
    -h|--help) grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'; exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
done

[ "$(uname -s)" = "Darwin" ] || die "the macOS boot smoke is Apple-Silicon/macOS only (SAD §9.6)"
case "$DRIVER" in
  metal|headless) ;;
  *) die "unknown --driver '$DRIVER' (expected: metal | headless)" ;;
esac
case "$FRAMES" in ''|*[!0-9]*) die "--frames must be a positive integer" ;; esac

# --- Build the launch command for the chosen target. -------------------------
# Common engine flags: quit deterministically after N frames, and (metal only)
# request the native Metal backend. --headless forces the dummy driver and
# IGNORES --rendering-driver, so we never combine the two.
DRIVER_ARGS=()
if [ "$DRIVER" = "metal" ]; then
  DRIVER_ARGS=(--rendering-driver metal)
else
  DRIVER_ARGS=(--headless)
fi

CMD=()
if [ -n "$APP" ]; then
  # --- Target: the #113 exported .app (resources baked; no .godot/ seed). -----
  [ -d "$APP" ] || die "exported .app not found: $APP"
  ENGINE_BIN="$(find "$APP/Contents/MacOS" -maxdepth 1 -type f -perm +111 | head -1)"
  [ -n "$ENGINE_BIN" ] && [ -x "$ENGINE_BIN" ] || die "no engine binary under $APP/Contents/MacOS"
  log "Target: exported bundle $(basename "$APP")"
  ok "engine binary: $ENGINE_BIN ($(lipo -archs "$ENGINE_BIN" 2>/dev/null || echo '?'))"
  CMD=("$ENGINE_BIN" "${DRIVER_ARGS[@]}" --quit-after "$FRAMES")
else
  # --- Target: client/project via the pinned editor (dev-box convenience). ----
  [ -f "$PROJECT/project.godot" ] || die "client project not found at $PROJECT"

  resolve_godot() {
    if [ -n "$GODOT_BIN" ] && [ -x "$GODOT_BIN" ]; then echo "$GODOT_BIN"; return; fi
    local c
    for c in \
      "$BIN_DIR/Godot.app/Contents/MacOS/Godot" \
      "/Applications/Godot.app/Contents/MacOS/Godot" \
      "$HOME/Applications/Godot.app/Contents/MacOS/Godot"; do
      [ -x "$c" ] && { echo "$c"; return; }
    done
    command -v godot 2>/dev/null && return
    die "Godot editor not found. Set \$GODOT, or fetch the pinned engine: scripts/fetch-deps.sh"
  }
  GODOT_BIN="$(resolve_godot)"
  log "Target: client/project via $GODOT_BIN"
  if [ -f "$PIN_FILE" ]; then
    WANT="$(grep -E '^GODOT_VERSION=' "$PIN_FILE" | cut -d= -f2)"
    GOT="$("$GODOT_BIN" --version 2>/dev/null | head -1)"
    case "$GOT" in ${WANT%-*}*) ok "Godot $GOT (pin $WANT)" ;; *) warn "Godot reports '$GOT', pin is '$WANT' — continuing" ;; esac
  fi

  # The login scene instantiates the MeridianLogin/LoginFlow GDExtension; a
  # dev-box project boot needs the EDITOR framework variant present.
  ls "$PROJECT/bin/"libmeridian.macos.editor.framework/libmeridian.macos.editor >/dev/null 2>&1 \
    || die "editor GDExtension not built. Run: (cd client && cmake -B build -DGODOTCPP_TARGET=editor && cmake --build build -j)"

  # .godot/ must be seeded. The cold import is done ONCE, WINDOWED (a real device
  # — never headless, per #283). Only metal (windowed) mode can seed.
  if [ ! -f "$PROJECT/.godot/extension_list.cfg" ]; then
    if [ "$SEED" -eq 1 ] || [ "$DRIVER" = "metal" ]; then
      log "Seeding .godot/ via one-time WINDOWED editor import (avoids #283 headless MoltenVK abort)"
      warn "the editor import NORMALISES client/project/project.godot (drops comments); \`git checkout\` it afterwards if you don't want that. CI never seeds — it boots the baked export."
      "$GODOT_BIN" --rendering-driver metal --editor --quit --path "$PROJECT" >/dev/null 2>&1 \
        || die "windowed editor import failed (needs a GUI session; run without redirection to see why)"
      ok ".godot/ seeded"
    else
      die ".godot/ not seeded and --driver headless cannot seed it (#283). Seed once with --seed (needs a GUI session)."
    fi
  fi
  CMD=("$GODOT_BIN" "${DRIVER_ARGS[@]}" --path "$PROJECT" --quit-after "$FRAMES")
fi

# --- Boot + capture. ---------------------------------------------------------
LOG="$(mktemp -t meridian-boot-smoke.XXXXXX)"
trap 'rm -f "$LOG"' EXIT
log "Booting (driver=$DRIVER, quit-after=$FRAMES frames): ${CMD[*]}"
set +e
"${CMD[@]}" >"$LOG" 2>&1
RC=$?
set -e

echo "----- boot log (tail) -----"
tail -n 20 "$LOG" || true
echo "---------------------------"

# --- Assert. -----------------------------------------------------------------
fails=0
check() { printf '  [%s] %s\n' "$([ "$1" -eq 0 ] && echo PASS || echo FAIL)" "$2"; [ "$1" -eq 0 ] || fails=$((fails+1)); }

# 1. Clean process exit (a crash / failed assert leaves nonzero).
check "$( [ "$RC" -eq 0 ] && echo 0 || echo 1 )" "process exited 0 (got $RC)"

# 2. The main scene reached _ready and printed the boot marker.
MARKER="$(grep -m1 'MERIDIAN_BOOT_OK' "$LOG" || true)"
check "$( [ -n "$MARKER" ] && echo 0 || echo 1 )" "reached main scene (MERIDIAN_BOOT_OK marker present)"
[ -n "$MARKER" ] && echo "      $MARKER"

# 3. In metal mode, a REAL Metal device must have come up (empty adapter == the
#    headless dummy driver, which would mean Metal was never exercised).
if [ "$DRIVER" = "metal" ] && [ -n "$MARKER" ]; then
  ADAPTER="$(printf '%s\n' "$MARKER" | sed -E 's/.*adapter="([^"]*)".*/\1/')"
  check "$( [ -n "$ADAPTER" ] && echo 0 || echo 1 )" "real Metal GPU device present (adapter=\"$ADAPTER\")"
fi

echo
if [ "$fails" -eq 0 ]; then
  ok "macOS boot smoke PASSED (driver=$DRIVER)"
  exit 0
else
  die "macOS boot smoke FAILED ($fails check(s), driver=$DRIVER)"
fi
