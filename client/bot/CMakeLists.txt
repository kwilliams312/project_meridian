# Project Meridian — headless BOT client v0 (#111).
#
# An engine-free executable (meridian-bot) + an engine-free bot core, built with
# NO godot-cpp: it links the #99 login core + TLS transport, the #102 movement
# core, and this repo's client-side worldd session (bot_world_session.*), plus the
# SAME crypto/wire stack the servers use so client↔worldd interoperate exactly:
#   - OpenSSL     : SRP-6a + HMAC + TLS 1.3 transport + ChaCha20-Poly1305/HKDF AEAD.
#   - FlatBuffers : auth.fbs / world.fbs codegen (flatc) + the header-only runtime.
#
# Enabled with -DMERIDIAN_BOT=ON (default OFF, like MERIDIAN_CLIENT_TESTS). The bot
# builds in any C++17 toolchain — no Godot runtime. Its engine-free unit tests
# register in ctest; the full login->worldd->move integration harness is
# env-guarded (client/test/run_bot_client_it.sh), mirroring #99's authd harness.

cmake_minimum_required(VERSION 3.22)

# Guard: only build when explicitly requested (keeps the default client build,
# which needs godot-cpp, unaffected).
if(NOT MERIDIAN_BOT)
  return()
endif()

find_package(OpenSSL REQUIRED)
find_package(FlatBuffers CONFIG QUIET)

# Resolve the repo root (client/bot -> repo root is 2 up) + the shared client core.
get_filename_component(MERIDIAN_REPO_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MERIDIAN_CLIENT_SRC "${MERIDIAN_REPO_ROOT}/client/gdextension/meridian/src")
set(MERIDIAN_SCHEMA_NET_DIR "${MERIDIAN_REPO_ROOT}/schema/net")
set(MERIDIAN_SRP_SRC "${MERIDIAN_REPO_ROOT}/server/libs/srp")

# --- flatc: system or the CONFIG-package imported target -------------------
find_program(MERIDIAN_FLATC_EXE flatc)
if(NOT TARGET flatbuffers::flatc AND MERIDIAN_FLATC_EXE)
  add_executable(flatbuffers::flatc IMPORTED GLOBAL)
  set_target_properties(flatbuffers::flatc PROPERTIES
    IMPORTED_LOCATION "${MERIDIAN_FLATC_EXE}")
endif()
if(TARGET flatbuffers::flatc)
  set(MERIDIAN_FLATC flatbuffers::flatc)
elseif(MERIDIAN_FLATC_EXE)
  set(MERIDIAN_FLATC "${MERIDIAN_FLATC_EXE}")
else()
  message(FATAL_ERROR
    "meridian-bot (#111): no flatc found for auth.fbs/world.fbs codegen. "
    "Install flatbuffers (brew install flatbuffers / apt libflatbuffers-dev).")
endif()

# --- flatc --cpp on schema/net/{auth,world}.fbs -> *_generated.h ------------
set(MERIDIAN_BOT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MERIDIAN_BOT_GEN_DIR}")
set(MERIDIAN_BOT_GENERATED
  "${MERIDIAN_BOT_GEN_DIR}/auth_generated.h"
  "${MERIDIAN_BOT_GEN_DIR}/world_generated.h")
add_custom_command(
  OUTPUT ${MERIDIAN_BOT_GENERATED}
  COMMAND ${MERIDIAN_FLATC} --cpp --scoped-enums -o "${MERIDIAN_BOT_GEN_DIR}"
          "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs"
          "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
  DEPENDS "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs"
          "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
  COMMENT "meridian-bot: flatc --cpp on schema/net/{auth,world}.fbs"
  VERBATIM)
add_custom_target(meridian-bot-codegen DEPENDS ${MERIDIAN_BOT_GENERATED})

# --- The engine-free bot core (a static lib so the CLI + the tests share it) --
add_library(meridian-bot-core STATIC
  bot_world_session.cpp
  bot_core.cpp
  "${MERIDIAN_CLIENT_SRC}/login_core.cpp"
  "${MERIDIAN_CLIENT_SRC}/srp_client_core.cpp"
  "${MERIDIAN_CLIENT_SRC}/login_transport.cpp"
  "${MERIDIAN_CLIENT_SRC}/movement_controller.cpp"
)
add_dependencies(meridian-bot-core meridian-bot-codegen)
target_include_directories(meridian-bot-core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${MERIDIAN_CLIENT_SRC}"
  "${MERIDIAN_BOT_GEN_DIR}")
if(TARGET flatbuffers::flatbuffers)
  target_link_libraries(meridian-bot-core PUBLIC flatbuffers::flatbuffers)
endif()
target_link_libraries(meridian-bot-core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(meridian-bot-core PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(meridian-bot-core PRIVATE /W4)
else()
  target_compile_options(meridian-bot-core PRIVATE -Wall -Wextra)
endif()

# --- The CLI wrapper --------------------------------------------------------
add_executable(meridian-bot bot_main.cpp)
target_link_libraries(meridian-bot PRIVATE meridian-bot-core)
set_target_properties(meridian-bot PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(meridian-bot PRIVATE -Wall -Wextra)
endif()

# --- The TWO-BOT AoI E2E driver (#248) --------------------------------------
# Composes the single-bot core TWICE, concurrently (one std::thread per bot), with
# a rendezvous barrier between enter-world and movement so BOTH bots are in-world
# before either moves. Proves the IT-M0 headline: two clients see each other move
# (mutual EntityEnter + EntityUpdate via the #87 relay). Driven over real TLS by
# client/test/run_two_bot_it.sh against a live authd + worldd.
find_package(Threads REQUIRED)
add_executable(meridian-two-bot bot_two_main.cpp)
target_link_libraries(meridian-two-bot PRIVATE meridian-bot-core Threads::Threads)
set_target_properties(meridian-two-bot PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(meridian-two-bot PRIVATE -Wall -Wextra)
endif()

# --- Engine-free unit tests (ctest) -----------------------------------------
enable_testing()

# The AEAD-interop test cross-checks the client ClientWorldSession against an
# INDEPENDENT reference implementation of worldd #84's scheme (a second OpenSSL
# code path — EVP_PKEY HKDF instead of EVP_KDF, explicit nonce assembly), so
# agreement proves the two ends derive the same keys + nonces + open each other's
# frames WITHOUT sharing the seal/open code. (The end-to-end proof that the client
# session_key matches the REAL server's is the integration harness, where real
# worldd consumes the bot's grant.)
add_executable(bot-world-session-test
  test/bot_world_session_test.cpp
  bot_world_session.cpp
  bot_core.cpp
  "${MERIDIAN_CLIENT_SRC}/login_core.cpp"
  "${MERIDIAN_CLIENT_SRC}/srp_client_core.cpp"
  "${MERIDIAN_CLIENT_SRC}/login_transport.cpp"
  "${MERIDIAN_CLIENT_SRC}/movement_controller.cpp"
)
add_dependencies(bot-world-session-test meridian-bot-codegen)
target_include_directories(bot-world-session-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${MERIDIAN_CLIENT_SRC}"
  "${MERIDIAN_BOT_GEN_DIR}")
if(TARGET flatbuffers::flatbuffers)
  target_link_libraries(bot-world-session-test PRIVATE flatbuffers::flatbuffers)
endif()
target_link_libraries(bot-world-session-test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(bot-world-session-test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(bot-world-session-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME bot-world-session COMMAND bot-world-session-test)

# --- Live-worldd integration test (env-guarded, driven by the shell harness) --
# The interop proof: the bot core + TlsLoginTransport driven over a REAL TLS
# socket against a RUNNING authd + worldd (client/test/run_bot_client_it.sh boots
# MariaDB, seeds an account+realm, launches both daemons, and runs meridian-bot).
# There is no separate ctest for it — the harness runs the meridian-bot binary
# directly, exactly as #99's harness runs authd-login-it.
