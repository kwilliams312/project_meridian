# Project Meridian — engine-free unit tests for forge_core cores (issue #134).
# These build WITHOUT godot-cpp: forge_version.* and forge_terrain_stub.* are
# engine-agnostic, so this compiles in any C++17 toolchain and runs in a plain
# `ctest` — the same shape as the client core tests (client/gdextension/meridian/test).

cmake_minimum_required(VERSION 3.22)
project(forge-core-tests LANGUAGES CXX)

enable_testing()

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# --- Terrain-seam alignment stub (Tools SAD §5.2 op 3) ---
add_executable(forge-terrain-stub-test
	forge_terrain_stub_test.cpp
	"${SRC}/forge_terrain_stub.cpp"
)
target_include_directories(forge-terrain-stub-test PRIVATE "${SRC}")
set_target_properties(forge-terrain-stub-test PROPERTIES
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
	target_compile_options(forge-terrain-stub-test PRIVATE /W4)
else()
	target_compile_options(forge-terrain-stub-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME forge-terrain-stub COMMAND forge-terrain-stub-test)

# --- Version core ---
add_executable(forge-version-test
	forge_version_test.cpp
	"${SRC}/forge_version.cpp"
)
target_include_directories(forge-version-test PRIVATE "${SRC}")
set_target_properties(forge-version-test PROPERTIES
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
	target_compile_options(forge-version-test PRIVATE /W4)
else()
	target_compile_options(forge-version-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME forge-version COMMAND forge-version-test)
