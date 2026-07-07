#!/usr/bin/env bash
# build_terrain3d.sh — build the vendored Terrain3D fork against the pinned
# godot-cpp 4.7 commit (issue #132, validates Tools PRD R7 GDExtension churn).
#
# Reproduces the spike binary at client/forge/spike/terrain3d/addons/terrain_3d/bin/.
# The binary itself is a build artifact (gitignored, like client/project/bin/) —
# this script is the source of truth for regenerating it.
#
# Requires: python3 + scons (pip install scons), a C++ toolchain. On first run
# it clones godot-cpp at the commit pinned in client/ENGINE_VERSION (GODOT_CPP_COMMIT)
# so the extension is ABI-matched to the pinned engine.
#
# Usage:  ./build_terrain3d.sh [macos|linux|windows] [arm64|x86_64]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../../.." && pwd)"
PLATFORM="${1:-macos}"
ARCH="${2:-arm64}"

# Pinned godot-cpp commit — single source of truth is client/ENGINE_VERSION.
GODOT_CPP_COMMIT="$(grep '^GODOT_CPP_COMMIT=' "$REPO_ROOT/client/ENGINE_VERSION" | cut -d= -f2)"
echo "Pinned godot-cpp commit (from client/ENGINE_VERSION): $GODOT_CPP_COMMIT"

SRC="$HERE/terrain3d_src"
BUILD="$HERE/.build"        # gitignored scratch build tree
mkdir -p "$BUILD"

# 1. Fetch godot-cpp at the pinned 4.7 commit (shallow).
if [[ ! -d "$BUILD/godot-cpp/.git" ]]; then
	echo "Cloning godot-cpp @ $GODOT_CPP_COMMIT ..."
	git init -q "$BUILD/godot-cpp"
	git -C "$BUILD/godot-cpp" remote add origin https://github.com/godotengine/godot-cpp.git
	git -C "$BUILD/godot-cpp" fetch --depth 1 origin "$GODOT_CPP_COMMIT"
	git -C "$BUILD/godot-cpp" checkout -q FETCH_HEAD
fi

# 2. Assemble the Terrain3D build tree (source + pinned godot-cpp).
rm -rf "$BUILD/t3d"
mkdir -p "$BUILD/t3d"
cp -R "$SRC/src" "$BUILD/t3d/src"
cp -R "$SRC/doc" "$BUILD/t3d/doc"
cp "$SRC/SConstruct" "$BUILD/t3d/SConstruct"
mkdir -p "$BUILD/t3d/project/addons/terrain_3d"
cp "$HERE/addons/terrain_3d/terrain.gdextension" "$BUILD/t3d/project/addons/terrain_3d/"
ln -sfn "$BUILD/godot-cpp" "$BUILD/t3d/godot-cpp"

# 3. Build (target=editor so the extension loads in the editor + headless).
echo "Building Terrain3D: platform=$PLATFORM arch=$ARCH target=editor ..."
( cd "$BUILD/t3d" && scons platform="$PLATFORM" arch="$ARCH" target=editor -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" )

# 4. Copy the built library into the addon bin/.
mkdir -p "$HERE/addons/terrain_3d/bin"
cp -R "$BUILD/t3d/project/addons/terrain_3d/bin/"* "$HERE/addons/terrain_3d/bin/"
echo "Done. Binary at: $HERE/addons/terrain_3d/bin/"
ls -la "$HERE/addons/terrain_3d/bin/"
