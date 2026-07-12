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
# 4.7-stable sync commit (client/ENGINE_VERSION: GODOT_CPP_COMMIT). Run
# `git submodule update --init --recursive` first.

cmake_minimum_required(VERSION 3.22)

project(meridian-client LANGUAGES CXX)

# --- Engine-free core tests (#102) ---
# The GDExtension cores (e.g. the movement controller, movement_controller.*) are
# engine-agnostic (Client SAD §9.2) and unit-tested WITHOUT a Godot runtime.
# Enable with -DMERIDIAN_CLIENT_TESTS=ON to build only the plain-C++ tests (no
# godot-cpp needed). Wired into a client-test CI job by #61. Default OFF so the
# normal extension build is unchanged.
option(MERIDIAN_CLIENT_TESTS "Build engine-free client core unit tests" OFF)

# --- Engine-free headless BOT client v0 (#111) ---
# The headless bot (client/bot/, meridian-bot) is engine-free too: it links the
# #99 login core + #102 movement core + this repo's client worldd session and
# drives the FULL client protocol against the real servers with NO Godot. Enable
# with -DMERIDIAN_BOT=ON (default OFF). It builds independently of the GDExtension
# (no godot-cpp), so a CI test job / a load harness needs no Godot toolchain.
option(MERIDIAN_BOT "Build the headless bot client (meridian-bot) + its tests" OFF)

# --- Engine-agnostic client NET CORE (#95) ---
# The net core (client/net/, meridian-clientnet) is a standalone static library —
# plain sockets + OpenSSL TLS 1.3, u32-LE framing, IF-2 world-frame codec, the
# per-session AEAD channel, and the FlatBuffers codec — that links WITHOUT
# godot-cpp. The bot and (later) the Godot client consume it; its framing/codec/
# AEAD round-trip tests register in ctest (clientnet). Enable with
# -DMERIDIAN_CLIENT_NET=ON (default OFF). Builds in any C++17 toolchain.
option(MERIDIAN_CLIENT_NET "Build the engine-agnostic client net core + its tests" OFF)

# --- Headless client-world probe (#301) ---
# meridian-client-probe drives the GUI client's WORLD-SESSION net path (the #97
# net-thread core + the meridian-clientnet stack — the SAME code scenes/world/world.gd
# runs) against a real authd + worldd with NO Godot, and reports the #87 AoI relay
# frames it receives about a bot: the see-a-bot-move proof for the GUI net path
# (client/test/run_client_sees_bot_it.sh). It needs the net core, so enabling it
# force-enables MERIDIAN_CLIENT_NET below. Default OFF.
option(MERIDIAN_CLIENT_PROBE "Build the headless client-world probe (meridian-client-probe)" OFF)
if(MERIDIAN_CLIENT_PROBE)
	set(MERIDIAN_CLIENT_NET ON)
endif()

if(MERIDIAN_CLIENT_TESTS OR MERIDIAN_BOT OR MERIDIAN_CLIENT_NET OR MERIDIAN_CLIENT_PROBE)
	enable_testing()
	# Add the net core FIRST so its `meridian-clientnet` target exists when the client
	# test subdir configures — the #97 net-thread-core test links it when present
	# (built with both -DMERIDIAN_CLIENT_TESTS=ON and -DMERIDIAN_CLIENT_NET=ON).
	if(MERIDIAN_CLIENT_NET)
		add_subdirectory(net)
	endif()
	if(MERIDIAN_CLIENT_TESTS)
		add_subdirectory(gdextension/meridian/test)
	endif()
	if(MERIDIAN_BOT)
		add_subdirectory(bot)
	endif()
	# The probe rides meridian-clientnet (added above); configure it last so the
	# target exists. Same engine-free discipline — no godot-cpp needed.
	if(MERIDIAN_CLIENT_PROBE)
		add_subdirectory(probe)
	endif()
	# Tests/bot-only configure: skip the godot-cpp-dependent GDExtension target so a
	# CI test / bot job needs no Godot toolchain.
	return()
endif()

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

# --- Vendored BLAKE3 REUSED for the #554 chunk-pack integrity verify ---
# chunk_pack_core.* recomputes each chunk's both-payloads BLAKE3 to match the IF-6
# manifest hash. It REUSES the ONE vendored BLAKE3 the mcc emitter uses
# (tools/mcc/src/hash) — there is no second BLAKE3 in the tree (issue #554
# directive). Compile that source into the extension + expose its include dir so
# `#include "hash/blake3.h"` resolves.
get_filename_component(MERIDIAN_MCC_HASH_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/../tools/mcc/src" ABSOLUTE)
target_sources(${TARGET_NAME} PRIVATE "${MERIDIAN_MCC_HASH_DIR}/hash/blake3.cpp")
target_include_directories(${TARGET_NAME} PRIVATE "${MERIDIAN_MCC_HASH_DIR}")

target_link_libraries(${TARGET_NAME} PRIVATE godot-cpp)

# --- Engine-agnostic client NET CORE, linked into the GDExtension (#97) ---
# The dedicated net-thread module (net_thread_core.* / meridian_net_thread.*) binds
# the #95 net core (meridian-clientnet — framing, the IF-2 wire_frame codec, the
# FlatBuffers codec, the AEAD WorldSession) so the extension speaks the SAME wire
# stack as the bot/servers. The net-core subdir is normally gated behind
# -DMERIDIAN_CLIENT_NET (tests/bot builds); the extension build force-enables it and
# links the library so the net thread has its net stack. meridian-clientnet exposes
# its include dir PUBLIC, so the net-thread sources resolve <meridian/clientnet/*>.
set(MERIDIAN_CLIENT_NET ON)
add_subdirectory(net)
target_link_libraries(${TARGET_NAME} PRIVATE meridian-clientnet)

# --- IF-1/IF-2 login core deps (#99) ---
# The login core (login_core.* / srp_client_core.* / login_transport.*) — the
# CLIENT side of the IF-1 login authd drives (#79) + the IF-2 WorldHello (#84) —
# takes the SAME crypto/wire stack the server uses so the two interoperate:
#   - OpenSSL     : SRP-6a BIGNUM/EVP + HMAC + TLS 1.3 client transport.
#   - FlatBuffers : auth.fbs / world.fbs codegen (flatc) + the header-only runtime.
# These are provisioned find-first / fetch-fallback exactly like server/libs/proto,
# so the extension builds on a dev box (Homebrew) AND a bare runner.
find_package(OpenSSL REQUIRED)
find_package(FlatBuffers CONFIG QUIET)

find_program(MERIDIAN_FLATC_EXE flatc)
if(TARGET flatbuffers::flatc)
	set(MERIDIAN_FLATC flatbuffers::flatc)
elseif(MERIDIAN_FLATC_EXE)
	set(MERIDIAN_FLATC "${MERIDIAN_FLATC_EXE}")
else()
	message(FATAL_ERROR
		"meridian client: no flatc found for auth.fbs/world.fbs codegen (#99). "
		"Install flatbuffers (brew install flatbuffers / apt libflatbuffers-dev).")
endif()

get_filename_component(MERIDIAN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(MERIDIAN_SCHEMA_NET_DIR "${MERIDIAN_REPO_ROOT}/schema/net")
set(MERIDIAN_LOGIN_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MERIDIAN_LOGIN_GEN_DIR}")
set(MERIDIAN_LOGIN_GENERATED
	"${MERIDIAN_LOGIN_GEN_DIR}/auth_generated.h"
	"${MERIDIAN_LOGIN_GEN_DIR}/world_generated.h")
add_custom_command(
	OUTPUT ${MERIDIAN_LOGIN_GENERATED}
	COMMAND ${MERIDIAN_FLATC} --cpp --scoped-enums -o "${MERIDIAN_LOGIN_GEN_DIR}"
			"${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs"
			"${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
	DEPENDS "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs" "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
	COMMENT "meridian client: flatc --cpp on schema/net/{auth,world}.fbs (#99)"
	VERBATIM)
add_custom_target(meridian-client-codegen DEPENDS ${MERIDIAN_LOGIN_GENERATED})
add_dependencies(${TARGET_NAME} meridian-client-codegen)

target_include_directories(${TARGET_NAME} PRIVATE "${MERIDIAN_LOGIN_GEN_DIR}")
if(TARGET flatbuffers::flatbuffers)
	target_link_libraries(${TARGET_NAME} PRIVATE flatbuffers::flatbuffers)
endif()
target_link_libraries(${TARGET_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)

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

# macOS: Godot loads a .framework bundle (arm64 only per SAD §9.6). Godot's
# macOS loader dlopen()s the `.framework` directory declared in
# project/meridian.gdextension and resolves the inner binary by the framework's
# own stem (`<name>.framework/<name>`). So the binary inside must be named
# `libmeridian.macos.<target>` — WITHOUT the arch suffix that GODOTCPP_SUFFIX
# carries — otherwise dlopen fails with "not a file" and the extension never
# loads. (Matches the SCons build's framework layout.)
if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
	set_target_properties(
		${TARGET_NAME}
		PROPERTIES
			SUFFIX ""
			OUTPUT_NAME "meridian.macos.${GODOTCPP_TARGET}"
			LIBRARY_OUTPUT_DIRECTORY
				"${OUTPUT_DIR}/libmeridian.macos.${GODOTCPP_TARGET}.framework"
	)
endif()
