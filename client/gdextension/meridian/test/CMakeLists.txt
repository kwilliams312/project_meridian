# Project Meridian — engine-free unit tests for the client GDExtension cores
# (#102). These build WITHOUT godot-cpp: the movement controller core
# (movement_controller.*) and the #101 headers are engine-agnostic (Client SAD
# §9.2), so this test compiles in any C++17 toolchain and runs in a plain
# `ctest` — the same shape as the server tests (server/**/test). Wired into a
# client-test CI job by #61 when the runner lands; kept out of the GDExtension
# link so `cmake --build` of the extension never needs a Godot runtime to test.

cmake_minimum_required(VERSION 3.22)
project(meridian-client-tests LANGUAGES CXX)

enable_testing()

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(movement-controller-test
  movement_controller_test.cpp
  "${SRC}/movement_controller.cpp"
)
target_include_directories(movement-controller-test PRIVATE "${SRC}")
set_target_properties(movement-controller-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(movement-controller-test PRIVATE /W4)
else()
  target_compile_options(movement-controller-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME movement-controller COMMAND movement-controller-test)

# --- Telemetry log capture core (#168) ---
# The engine-free ERROR/CRITICAL log capture + shipping core (Client SAD §9.2 /
# D-29 client channel). Same engine-free discipline: no godot-cpp, plain C++17.
add_executable(telemetry-log-capture-test
  telemetry_log_capture_test.cpp
  "${SRC}/telemetry_log_capture.cpp"
  "${SRC}/telemetry_log_core.cpp"
)
target_include_directories(telemetry-log-capture-test PRIVATE "${SRC}")
set_target_properties(telemetry-log-capture-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(telemetry-log-capture-test PRIVATE /W4)
else()
  target_compile_options(telemetry-log-capture-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME telemetry-log-capture COMMAND telemetry-log-capture-test)

# --- IF-5 pack-manifest verify core (#107) ---
# The engine-free client boot-scene pack-mount manifest verifier (Client SAD §2.3
# / §5.2; the client counterpart to worldd's #89 world-DB boot check). Same
# engine-free discipline: no godot-cpp, plain C++17, hand-rolled JSON reader. The
# round-trip test reads a REAL emit-pck manifest fixture (test/fixtures/), so the
# verifier and the mcc emitter are proven to agree on the exact JSON shape.
add_executable(pack-manifest-core-test
  pack_manifest_core_test.cpp
  "${SRC}/pack_manifest_core.cpp"
)
target_include_directories(pack-manifest-core-test PRIVATE "${SRC}")
set_target_properties(pack-manifest-core-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(pack-manifest-core-test PRIVATE /W4)
else()
  target_compile_options(pack-manifest-core-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME pack-manifest-core COMMAND pack-manifest-core-test)
