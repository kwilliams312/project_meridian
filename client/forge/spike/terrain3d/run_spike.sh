#!/usr/bin/env bash
# run_spike.sh — run the headless Terrain3D spike harness (issue #132, #283).
#
# Verifies the Terrain3D GDExtension loads in the pinned Godot 4.7 and that the
# ITerrainBackend seam works, then prints measured numbers. No display needed.
#
# Usage:
#   GODOT=/Applications/Godot.app/Contents/MacOS/Godot ./run_spike.sh
# (defaults to that path on macOS). Exits non-zero if any check fails.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GODOT="${GODOT:-/Applications/Godot.app/Contents/MacOS/Godot}"

if [[ ! -x "$GODOT" ]]; then
	echo "Godot 4.7 binary not found at: $GODOT" >&2
	echo "Set GODOT=/path/to/godot (must be 4.7-stable, client/ENGINE_VERSION)." >&2
	exit 2
fi

if [[ ! -d "$HERE/addons/terrain_3d/bin" ]] || ! ls "$HERE"/addons/terrain_3d/bin/* >/dev/null 2>&1; then
	echo "Terrain3D binary missing — build it first: ./build_terrain3d.sh" >&2
	exit 2
fi

exec "$GODOT" --headless --path "$HERE" --script res://spike/spike_harness.gd -- --report
