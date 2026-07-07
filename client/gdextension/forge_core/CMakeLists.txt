# Project Meridian — Forge `forge_core` GDExtension build (issue #134).
#
# forge_core is the native (C++20) half of the Forge editor plugin (Tools SAD §5).
# This build is DELIBERATELY ISOLATED from the client extension build
# (client/CMakeLists.txt): forge_core links ONLY godot-cpp — no OpenSSL, no
# FlatBuffers, no net core — so the editor tool never drags in the game client's
# runtime deps, and enabling/building Forge cannot break the client boot.
#
# godot-cpp is the SAME vendored submodule the client uses (client/godot-cpp,
# pinned in client/ENGINE_VERSION: GODOT_CPP_COMMIT). Run
# `git submodule update --init --recursive` first.
#
# Usage (macOS Apple Silicon dev box — the de-risk target for #134):
#   cmake -B build -DGODOTCPP_TARGET=editor
#   cmake --build build -j
#   # -> client/forge/project/bin/libforge_core.macos.editor.framework
#
# Engine-free core unit tests (no godot-cpp needed):
#   cmake -B build-tests -DFORGE_CORE_TESTS=ON && cmake --build build-tests -j
#   ctest --test-dir build-tests --output-on-failure

cmake_minimum_required(VERSION 3.22)

project(forge-core LANGUAGES CXX)

# --- Engine-free core tests (mirrors client's MERIDIAN_CLIENT_TESTS, #102) ------
# The forge_core cores (forge_version.*, forge_terrain_stub.*) are engine-agnostic
# and unit-tested WITHOUT a Godot runtime. Enable with -DFORGE_CORE_TESTS=ON to
# build only the plain-C++ tests (no godot-cpp). Default OFF so the normal
# extension build is unchanged.
option(FORGE_CORE_TESTS "Build engine-free forge_core unit tests" OFF)

if(FORGE_CORE_TESTS)
	enable_testing()
	add_subdirectory(test)
	# Tests-only configure: skip the godot-cpp-dependent extension target so a CI
	# test job needs no Godot toolchain.
	return()
endif()

# --- Vendored godot-cpp (the client's pinned submodule) ------------------------
get_filename_component(FORGE_GODOT_CPP_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/../../godot-cpp" ABSOLUTE)
if(NOT EXISTS "${FORGE_GODOT_CPP_DIR}/CMakeLists.txt")
	message(FATAL_ERROR
		"godot-cpp submodule missing at ${FORGE_GODOT_CPP_DIR}. "
		"Run: git submodule update --init --recursive")
endif()

# godot-cpp defines the GODOTCPP_TARGET/platform/arch options and the `godot-cpp`
# static-library target. Build it in a subdir of THIS build tree so the client and
# forge_core builds never share/clobber a godot-cpp binary dir.
add_subdirectory("${FORGE_GODOT_CPP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp")

# --- forge_core GDExtension module ---------------------------------------------
set(TARGET_NAME "forge_core")

add_library(${TARGET_NAME} SHARED)

file(GLOB_RECURSE FORGE_CORE_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

target_sources(${TARGET_NAME} PRIVATE ${FORGE_CORE_SOURCES})
target_link_libraries(${TARGET_NAME} PRIVATE godot-cpp)

# Reuse godot-cpp's computed filename suffix (e.g. ".macos.editor") so the output
# matches the paths declared in forge/project/forge_core.gdextension.
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)

# Output straight into the Forge Godot project's bin/ (gitignored artifact).
get_filename_component(FORGE_PROJECT_BIN
	"${CMAKE_CURRENT_SOURCE_DIR}/../../forge/project/bin" ABSOLUTE)

set_target_properties(
	${TARGET_NAME}
	PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF
		POSITION_INDEPENDENT_CODE ON
		PREFIX "lib"
		OUTPUT_NAME "forge_core${GODOTCPP_SUFFIX}"
		LIBRARY_OUTPUT_DIRECTORY "$<1:${FORGE_PROJECT_BIN}>"
		RUNTIME_OUTPUT_DIRECTORY "$<1:${FORGE_PROJECT_BIN}>"
		PDB_OUTPUT_DIRECTORY "$<1:${FORGE_PROJECT_BIN}>"
)

# macOS: Godot loads a .framework bundle (arm64 only per Client SAD §9.6). Godot's
# macOS loader dlopen()s the `.framework` directory declared in the manifest and
# resolves the inner binary by the framework's own stem
# (`<name>.framework/<name>`). So the binary inside must be named
# `libforge_core.macos.<target>` — WITHOUT the arch suffix that GODOTCPP_SUFFIX
# carries — otherwise dlopen fails with "not a file" and the extension never loads.
# (This is the exact #263 packaging fix mirrored for forge_core.)
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
	set_target_properties(
		${TARGET_NAME}
		PROPERTIES
			SUFFIX ""
			OUTPUT_NAME "forge_core.macos.${GODOTCPP_TARGET}"
			LIBRARY_OUTPUT_DIRECTORY
				"${FORGE_PROJECT_BIN}/libforge_core.macos.${GODOTCPP_TARGET}.framework"
	)
endif()
