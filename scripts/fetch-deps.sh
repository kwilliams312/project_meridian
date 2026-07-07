#!/usr/bin/env bash
# Project Meridian — fetch & verify the pinned Godot engine binaries (issue #158).
#
# Downloads the pinned Godot 4.7-stable editor + export templates for the host
# platform into client/.godot-bin/ (gitignored) and verifies each against the
# SHA-512 sums pinned in client/ENGINE_VERSION. Multi-GB binaries are NEVER
# committed — this script is how a dev/CI obtains them reproducibly.
#
# It also inits the vendored godot-cpp submodule at its pinned commit.
#
# Usage:
#   scripts/fetch-deps.sh            # host platform editor + export templates
#   scripts/fetch-deps.sh --submodule-only
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIN_FILE="$REPO_ROOT/client/ENGINE_VERSION"
BIN_DIR="$REPO_ROOT/client/.godot-bin"
BASE_URL="https://github.com/godotengine/godot/releases/download"

# --- Load the pins (KEY=VALUE lines from ENGINE_VERSION) ---
[ -f "$PIN_FILE" ] || { echo "ERROR: $PIN_FILE not found" >&2; exit 1; }
# shellcheck disable=SC1090
GODOT_VERSION="$(grep -E '^GODOT_VERSION=' "$PIN_FILE" | cut -d= -f2)"
GODOT_CPP_COMMIT="$(grep -E '^GODOT_CPP_COMMIT=' "$PIN_FILE" | cut -d= -f2)"
SHA_WIN="$(grep -E '^SHA512_WIN64_EDITOR=' "$PIN_FILE" | cut -d= -f2)"
SHA_MAC="$(grep -E '^SHA512_MACOS_EDITOR=' "$PIN_FILE" | cut -d= -f2)"
SHA_TPL="$(grep -E '^SHA512_EXPORT_TEMPLATES=' "$PIN_FILE" | cut -d= -f2)"

# --- Always ensure the godot-cpp submodule is at its pinned commit ---
echo "==> Syncing vendored godot-cpp submodule (pin: $GODOT_CPP_COMMIT)"
git -C "$REPO_ROOT" submodule update --init --recursive client/godot-cpp
ACTUAL_CPP="$(git -C "$REPO_ROOT/client/godot-cpp" rev-parse HEAD)"
if [ "$ACTUAL_CPP" != "$GODOT_CPP_COMMIT" ]; then
	echo "ERROR: godot-cpp at $ACTUAL_CPP, expected $GODOT_CPP_COMMIT" >&2
	exit 1
fi
echo "    godot-cpp OK @ $ACTUAL_CPP"

if [ "${1:-}" = "--submodule-only" ]; then
	echo "==> --submodule-only: skipping engine binary download."
	exit 0
fi

mkdir -p "$BIN_DIR"

# sha512 helper (macOS ships shasum; Linux ships sha512sum)
sha512_of() {
	if command -v sha512sum >/dev/null 2>&1; then sha512sum "$1" | awk '{print $1}';
	else shasum -a 512 "$1" | awk '{print $1}'; fi
}

fetch_verify() {
	local filename="$1" expected="$2"
	local dest="$BIN_DIR/$filename"
	if [ -f "$dest" ] && [ "$(sha512_of "$dest")" = "$expected" ]; then
		echo "    cached + verified: $filename"; return 0
	fi
	echo "==> Downloading $filename"
	curl -fL --retry 3 -o "$dest" "$BASE_URL/$GODOT_VERSION/$filename"
	local actual; actual="$(sha512_of "$dest")"
	if [ "$actual" != "$expected" ]; then
		echo "ERROR: SHA-512 mismatch for $filename" >&2
		echo "  expected $expected" >&2
		echo "  actual   $actual" >&2
		rm -f "$dest"; exit 1
	fi
	echo "    verified: $filename"
}

# Editor for the host platform + the shared export templates (.tpz).
case "$(uname -s)" in
	Darwin) fetch_verify "Godot_v${GODOT_VERSION}_macos.universal.zip" "$SHA_MAC" ;;
	Linux|MINGW*|MSYS*|CYGWIN*) fetch_verify "Godot_v${GODOT_VERSION}_win64.exe.zip" "$SHA_WIN" ;;
	*) echo "WARN: unknown host $(uname -s); fetching both editors";
	   fetch_verify "Godot_v${GODOT_VERSION}_win64.exe.zip" "$SHA_WIN";
	   fetch_verify "Godot_v${GODOT_VERSION}_macos.universal.zip" "$SHA_MAC" ;;
esac
fetch_verify "Godot_v${GODOT_VERSION}_export_templates.tpz" "$SHA_TPL"

echo "==> Done. Pinned Godot $GODOT_VERSION binaries in $BIN_DIR (gitignored)."
