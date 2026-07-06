# Project Meridian — client GDExtension build (CMake path).
#
# This is the CI-friendly build path (no SCons dependency). The canonical
# godot-cpp SCons path is also provided (client/SConstruct) for parity with
# upstream docs. Both build the same sources into client/project/bin/.
#
# Usage (see client/README.md for full Win + macOS-arm64 instructions):
#   cmake -B build -DGODOTCPP_TARGET=template_debug
#   cmake --build build -j
#
# godot-cpp is vendored as a submodule at client/godot-cpp, pinned to the
# 4.6-stable sync commit (client/ENGINE_VERSION: GODOT_CPP_COMMIT). Run
# `git submodule update --init --recursive` first.

cmake_minimum_required(VERSION 3.22)

project(meridian-client LANGUAGES CXX)

# --- Vendored godot-cpp (pinned submodule) ---
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/godot-cpp/CMakeLists.txt")
	message(FATAL_ERROR
		"godot-cpp submodule missing. Run: git submodule update --init --recursive")
endif()

# godot-cpp defines its own options (GODOTCPP_TARGET, platform, arch, etc.) and
# the `godot-cpp` static-library target we link against.
add_subdirectory(godot-cpp)

# --- Meridian GDExtension module (bootstrap: placeholder MeridianClient only) ---
set(TARGET_NAME "meridian")

add_library(${TARGET_NAME} SHARED)

file(GLOB_RECURSE MERIDIAN_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/gdextension/meridian/src/*.cpp")

target_sources(${TARGET_NAME} PRIVATE ${MERIDIAN_SOURCES})

target_link_libraries(${TARGET_NAME} PRIVATE godot-cpp)

# Reuse godot-cpp's computed filename suffix (e.g.
# ".windows.template_debug.x86_64") so the output matches the paths declared in
# project/meridian.gdextension.
get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)

set(OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/project/bin")

set_target_properties(
	${TARGET_NAME}
	PROPERTIES
		CXX_STANDARD 17
		CXX_EXTENSIONS OFF
		POSITION_INDEPENDENT_CODE ON
		PREFIX "lib"
		OUTPUT_NAME "meridian${GODOTCPP_SUFFIX}"
		LIBRARY_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
		RUNTIME_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
		PDB_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
)

# macOS: Godot loads a .framework bundle (arm64 only per SAD §9.6).
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
	set_target_properties(
		${TARGET_NAME}
		PROPERTIES
			SUFFIX ""
			LIBRARY_OUTPUT_DIRECTORY
				"${OUTPUT_DIR}/libmeridian.macos.${GODOTCPP_TARGET}.framework"
	)
endif()
