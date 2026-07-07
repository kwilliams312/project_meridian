#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/export-client-macos.sh — produce the macOS Apple-Silicon (arm64) export
# of the Meridian Godot client (issue #113).
#
# WHAT it does, using ONLY the pinned toolchain (client/ENGINE_VERSION):
#   1. resolves the pinned Godot 4.7-stable editor (the export host),
#   2. installs the pinned export templates (from the .tpz fetch-deps.sh downloaded)
#      and INJECTS arm64-thinned engine binaries into the macOS template so an
#      `architecture="arm64"` export resolves (the official .tpz ships only
#      universal binaries; Godot's exporter looks up godot_macos_<cfg>.arm64 by
#      exact name and does NOT thin — verified against 4.7 export_plugin.cpp),
#   3. builds (or reuses) the **template_debug** GDExtension framework — the
#      variant an EXPORTED game dlopen()s, NOT the editor variant (#300),
#   4. runs `godot --headless --export-<cfg> "<preset>" <out.app>`,
#   5. asserts the .app is pure arm64 + bundles the template_debug framework +
#      carries an ad-hoc signature (M0: no Developer-ID, no notarization),
#   6. packages the .app into a distributable .zip (via ditto, signature-preserving)
#      and prints/emits EXPORT_VERSION + EXPORT_HASH (sha256 of the .zip) — the
#      coordinates the nightly build manifest pins (scripts/build-manifest.sh
#      --client-version/--client-hash, #62/#307).
#
# #283 note: a *cold* windowed `--editor` import aborts inside MoltenVK on some
# macOS setups; the EXPORT path here uses Godot's headless/dummy renderer and does
# NOT touch MoltenVK, so a cold `--export-debug` on a fresh checkout works (proven
# on Apple M-series). If a future project addition makes the cold headless import
# abort, pass --seed to do the one-time WINDOWED .godot/ seed first (the #290
# approach run-client.sh uses); that needs a GUI session (a self-hosted runner may
# not have one — that is the only step that would need the owner/runner console).
#
# Usage:
#   scripts/export-client-macos.sh                        # debug arm64 → build/export/macos
#   scripts/export-client-macos.sh --target release       # release export
#   scripts/export-client-macos.sh --skip-build           # reuse client/project/bin framework
#   scripts/export-client-macos.sh --emit-env build/client-export.env
#   scripts/export-client-macos.sh --seed                 # force the #283 windowed seed
#   scripts/export-client-macos.sh --help
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIN_FILE="$REPO_ROOT/client/ENGINE_VERSION"
PROJECT="$REPO_ROOT/client/project"
PRESET_FILE="$PROJECT/export_presets.cfg"
BIN_DIR="$REPO_ROOT/client/.godot-bin"

# --- Minimal logging (self-contained; no dev-toolkit dependency). ------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_RESET=$'\033[0m'; C_B=$'\033[1m'; C_BLUE=$'\033[34m'; C_GREEN=$'\033[32m'; C_YEL=$'\033[33m'; C_RED=$'\033[31m'
else C_RESET=''; C_B=''; C_BLUE=''; C_GREEN=''; C_YEL=''; C_RED=''; fi
log()  { printf '%s==>%s %s\n' "${C_BLUE}${C_B}" "${C_RESET}" "$*"; }
ok()   { printf '%s  ok%s %s\n' "${C_GREEN}" "${C_RESET}" "$*"; }
warn() { printf '%s  ! %s%s\n' "${C_YEL}" "$*" "${C_RESET}" >&2; }
die()  { printf '%s  x %s%s\n' "${C_RED}${C_B}" "$*" "${C_RESET}" >&2; exit 1; }

# --- Defaults + arg parsing. -------------------------------------------------
OUT_DIR="$REPO_ROOT/build/export/macos"
TARGET="debug"                 # debug|release
PRESET="macOS arm64"
GODOT_BIN="${GODOT:-}"
SKIP_BUILD=0
SEED=0
EMIT_ENV=""

while [ $# -gt 0 ]; do
  case "$1" in
    --out)      OUT_DIR="$2"; shift 2 ;;
    --target)   TARGET="$2"; shift 2 ;;
    --preset)   PRESET="$2"; shift 2 ;;
    --godot)    GODOT_BIN="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --seed)     SEED=1; shift ;;
    --emit-env) EMIT_ENV="$2"; shift 2 ;;
    -h|--help)  grep -E '^#( |$)' "$0" | sed -E 's/^# ?//'; exit 0 ;;
    *) die "unknown argument '$1' (try --help)" ;;
  esac
done

[ "$(uname -s)" = "Darwin" ] || die "the macOS client export is Apple-Silicon-only (SAD §9.6)"
case "$TARGET" in
  debug)   EXPORT_FLAG="--export-debug";   GDTARGET="template_debug" ;;
  release) EXPORT_FLAG="--export-release"; GDTARGET="template_release" ;;
  *) die "unknown --target '$TARGET' (expected: debug | release)" ;;
esac
[ -f "$PRESET_FILE" ] || die "export preset not found: $PRESET_FILE"

# --- Pinned engine version (must match the resolved editor + templates). -----
WANT_VERSION="$(grep -E '^GODOT_VERSION=' "$PIN_FILE" | cut -d= -f2)"   # e.g. 4.7-stable
TPL_ID="${WANT_VERSION%-*}.${WANT_VERSION#*-}"                          # 4.7.stable
SHA_TPL="$(grep -E '^SHA512_EXPORT_TEMPLATES=' "$PIN_FILE" | cut -d= -f2)"

# --- Resolve the pinned Godot editor (the export host). ----------------------
# Order mirrors run-client.sh: $GODOT → client/.godot-bin → /Applications → PATH.
resolve_godot() {
  [ -n "$GODOT_BIN" ] && { [ -x "$GODOT_BIN" ] || die "\$GODOT/--godot not executable: $GODOT_BIN"; echo "$GODOT_BIN"; return; }
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
GOT_VERSION="$("$GODOT_BIN" --version 2>/dev/null | head -1)"
case "$GOT_VERSION" in
  ${WANT_VERSION%-*}*) ok "Godot $GOT_VERSION (pin $WANT_VERSION) at $GODOT_BIN" ;;
  *) warn "Godot at $GODOT_BIN reports '$GOT_VERSION', pin is '$WANT_VERSION' — continuing" ;;
esac

# --- Ensure the pinned export templates are installed. -----------------------
TPL_DIR="$HOME/Library/Application Support/Godot/export_templates/$TPL_ID"
MACOS_ZIP="$TPL_DIR/macos.zip"
sha512_of() { if command -v sha512sum >/dev/null 2>&1; then sha512sum "$1" | awk '{print $1}'; else shasum -a 512 "$1" | awk '{print $1}'; fi; }

if [ ! -f "$MACOS_ZIP" ] || [ ! -f "$TPL_DIR/version.txt" ]; then
  log "Installing pinned export templates → $TPL_DIR"
  TPZ="$BIN_DIR/Godot_v${WANT_VERSION}_export_templates.tpz"
  [ -f "$TPZ" ] || die "template archive not found: $TPZ (run scripts/fetch-deps.sh first)"
  [ "$(sha512_of "$TPZ")" = "$SHA_TPL" ] || die "export-template .tpz SHA-512 mismatch vs ENGINE_VERSION — refusing to install"
  tmp="$(mktemp -d)"; unzip -q "$TPZ" -d "$tmp"
  mkdir -p "$TPL_DIR"; cp -f "$tmp"/templates/* "$TPL_DIR"/
  rm -rf "$tmp"
  ok "templates installed ($(cat "$TPL_DIR/version.txt"))"
else
  ok "export templates present ($(cat "$TPL_DIR/version.txt")) at $TPL_DIR"
fi
[ "$(cat "$TPL_DIR/version.txt")" = "$TPL_ID" ] || die "installed template version '$(cat "$TPL_DIR/version.txt")' != pin '$TPL_ID'"

# --- Inject arm64-thinned engine binaries into the macOS template (idempotent).
# The official .tpz ships only godot_macos_<cfg>.universal; an arm64 export needs
# godot_macos_<cfg>.arm64 by exact name (no thinning in the exporter). We derive
# them with lipo and add them into the installed macos.zip alongside universal.
if [ -n "$(unzip -l "$MACOS_ZIP" | grep 'godot_macos_debug\.arm64' || true)" ]; then
  ok "arm64 template binaries already present in macos.zip"
else
  log "Injecting arm64-thinned engine binaries into macos.zip"
  tmp="$(mktemp -d)"; ( cd "$tmp" && unzip -q "$MACOS_ZIP" 'macos_template.app/Contents/MacOS/*' )
  m="$tmp/macos_template.app/Contents/MacOS"
  for cfg in debug release; do
    lipo -thin arm64 "$m/godot_macos_${cfg}.universal" -output "$m/godot_macos_${cfg}.arm64" \
      || die "lipo thin arm64 failed for godot_macos_${cfg}.universal"
  done
  ( cd "$tmp" && zip -q "$MACOS_ZIP" \
      macos_template.app/Contents/MacOS/godot_macos_debug.arm64 \
      macos_template.app/Contents/MacOS/godot_macos_release.arm64 )
  rm -rf "$tmp"
  ok "arm64 binaries injected"
fi

# --- Ensure the template_<cfg> GDExtension framework is built. ---------------
FW="$PROJECT/bin/libmeridian.macos.${GDTARGET}.framework"
FW_BIN="$FW/libmeridian.macos.${GDTARGET}"
if [ -f "$FW_BIN" ] && [ "$SKIP_BUILD" -eq 1 ]; then
  ok "reusing framework $FW (--skip-build)"
elif [ "$SKIP_BUILD" -eq 1 ]; then
  die "--skip-build set but framework missing: $FW_BIN"
else
  log "Building $GDTARGET GDExtension framework (arm64)"
  [ -f "$REPO_ROOT/client/godot-cpp/CMakeLists.txt" ] || die "godot-cpp submodule missing — run scripts/fetch-deps.sh --submodule-only"
  # shellcheck source=scripts/dev/_common.sh
  source "$REPO_ROOT/scripts/dev/_common.sh"
  setup_cmake_env
  btree="$REPO_ROOT/build/client-gdext-${GDTARGET}"
  arch -arm64 cmake -S "$REPO_ROOT/client" -B "$btree" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DGODOTCPP_TARGET="$GDTARGET" >/dev/null
  arch -arm64 cmake --build "$btree" -j "$(sysctl -n hw.ncpu)" >/dev/null
  ok "framework built → $FW"
fi
[ -f "$FW_BIN" ] || die "GDExtension framework not produced: $FW_BIN"
case "$(lipo -archs "$FW_BIN")" in *arm64*) ;; *) die "framework is not arm64: $(lipo -archs "$FW_BIN")" ;; esac

# --- Optional one-time windowed .godot/ seed (#283 fallback; default off). ----
if [ "$SEED" -eq 1 ] && [ ! -f "$PROJECT/.godot/extension_list.cfg" ]; then
  log "Seeding .godot/ via one-time WINDOWED editor import (#283 fallback)"
  "$GODOT_BIN" --rendering-driver metal --editor --quit --path "$PROJECT" >/dev/null 2>&1 \
    || die "windowed editor seed failed (needs a GUI session)"
  ok ".godot/ seeded"
fi

# --- Export. -----------------------------------------------------------------
APP_NAME="ProjectMeridian.app"
OUT_APP="$OUT_DIR/$APP_NAME"
rm -rf "$OUT_DIR"; mkdir -p "$OUT_DIR"
log "Exporting: $GODOT_BIN --headless $EXPORT_FLAG \"$PRESET\" → $OUT_APP"
"$GODOT_BIN" --headless --path "$PROJECT" "$EXPORT_FLAG" "$PRESET" "$OUT_APP" \
  || die "godot export failed (exit $?)"
[ -d "$OUT_APP" ] || die "export produced no .app at $OUT_APP"

# --- Assert the artifact. ----------------------------------------------------
ENGINE_BIN="$(find "$OUT_APP/Contents/MacOS" -type f -maxdepth 1 | head -1)"
case "$(lipo -archs "$ENGINE_BIN")" in
  arm64) ok "engine binary is pure arm64" ;;
  *) die "engine binary is not pure arm64: $(lipo -archs "$ENGINE_BIN")" ;;
esac
BUNDLED_FW="$OUT_APP/Contents/Frameworks/libmeridian.macos.${GDTARGET}.framework/libmeridian.macos.${GDTARGET}"
[ -f "$BUNDLED_FW" ] || die "exported .app does NOT bundle the $GDTARGET GDExtension framework"
case "$(lipo -archs "$BUNDLED_FW")" in *arm64*) ok "bundled $GDTARGET framework is arm64" ;; *) die "bundled framework not arm64" ;; esac
# Any x86_64 Mach-O anywhere in the bundle is a violation of the arm64-only rule.
while IFS= read -r f; do
  file "$f" | grep -q 'Mach-O' || continue
  case "$(lipo -archs "$f" 2>/dev/null)" in *x86_64*) die "x86_64 slice found in bundle: ${f#"$OUT_APP"}" ;; esac
done < <(find "$OUT_APP" -type f)
ok "no x86_64 slices — arm64-only bundle"
CODESIGN_INFO="$(codesign -dv "$OUT_APP" 2>&1 || true)"
case "$CODESIGN_INFO" in
  *Signature=adhoc*) ok "ad-hoc code signature present (M0)" ;;
  *) warn "expected an ad-hoc signature; codesign summary:"; printf '%s\n' "$CODESIGN_INFO" | grep -i signature >&2 || true ;;
esac
if codesign --verify --strict "$OUT_APP" 2>/dev/null; then ok "code signature verifies"; else warn "codesign --verify reported issues"; fi

# --- Package + hash. ---------------------------------------------------------
ZIP="$OUT_DIR/ProjectMeridian-macos-arm64.zip"
rm -f "$ZIP"
ditto -c -k --keepParent "$OUT_APP" "$ZIP"      # signature-preserving zip
EXPORT_HASH="$(shasum -a 256 "$ZIP" | awk '{print $1}')"
# Version: the app short_version from the preset (semver of the client export).
SHORT_VER="$(grep -E '^application/short_version=' "$PRESET_FILE" | head -1 | sed -E 's/.*="(.*)"/\1/')"
EXPORT_VERSION="${SHORT_VER:-0.0.0}"

echo
ok "macOS arm64 export complete"
echo "  app          : $OUT_APP"
echo "  artifact     : $ZIP"
echo "  EXPORT_VERSION : $EXPORT_VERSION"
echo "  EXPORT_HASH    : $EXPORT_HASH"

if [ -n "$EMIT_ENV" ]; then
  {
    echo "EXPORT_VERSION=$EXPORT_VERSION"
    echo "EXPORT_HASH=$EXPORT_HASH"
    echo "EXPORT_ZIP=$ZIP"
    echo "EXPORT_APP=$OUT_APP"
  } > "$EMIT_ENV"
  ok "wrote export coordinates → $EMIT_ENV"
fi
