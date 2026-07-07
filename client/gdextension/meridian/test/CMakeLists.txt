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
