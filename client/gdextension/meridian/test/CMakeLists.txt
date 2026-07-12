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

# The SPSC ring + net-thread core tests use std::thread.
find_package(Threads REQUIRED)

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

# --- M1 heightfield ground-sample core (#557, Epic #22 Story D) ---
# The engine-free `IWorldQuery` backend that replaces the M0 flat stub: it bilinear-
# samples the per-chunk heightfield (schema/chunk/chunk.fbs §3.2) the pack ships, so
# client movement matches the server's validation of the SAME bytes by construction
# (Q1(a); movement_constants.h §7). Zero deps (no godot-cpp, no FlatBuffers): it
# operates on an already-decoded grid, so it always builds in a plain ctest. The test
# rebuilds Story-0's known non-flat ramp+bowl surface in memory and asserts bilinear
# parity, the documented-formula parity, NaN holes/OOB, and the shared-edge seam.
add_executable(heightfield-query-test
  heightfield_query_test.cpp
  "${SRC}/heightfield_query.cpp"
)
target_include_directories(heightfield-query-test PRIVATE "${SRC}")
set_target_properties(heightfield-query-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(heightfield-query-test PRIVATE /W4)
else()
  target_compile_options(heightfield-query-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME heightfield-query COMMAND heightfield-query-test)

# --- Schema/protocol VERSION COMPAT core (#98) ---
# The engine-free version-compare rule (version_compat.*): pure logic, ZERO deps (no
# godot-cpp, no OpenSSL, no flatc), so it always builds in a plain ctest. Proves the
# exact-match proto rule (out-of-date on any diff), the advisory content-hash arm
# (warn at M0 / reject under a hard-fail policy), and safe malformed/absent handling.
add_executable(version-compat-test
  version_compat_test.cpp
  "${SRC}/version_compat.cpp"
)
target_include_directories(version-compat-test PRIVATE "${SRC}")
set_target_properties(version-compat-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(version-compat-test PRIVATE /W4)
else()
  target_compile_options(version-compat-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME version-compat COMMAND version-compat-test)

# --- Prediction reconciliation SMOOTHING core (#103) ---
# The engine-free error-offset decay + snap layer on top of the #102 ring-buffer
# rewind/re-simulate: a small server correction is absorbed as a render offset that
# decays to zero over the 100–200 ms window (monotonic, converges); a large one
# snaps. Same engine-free discipline: no godot-cpp, plain C++17; time injected so
# the decay is asserted deterministically. Links the SAME movement_controller.cpp
# core the #102 test uses — client prediction and re-simulation share ONE code path.
add_executable(prediction-reconcile-test
  prediction_reconcile_test.cpp
  "${SRC}/movement_controller.cpp"
)
target_include_directories(prediction-reconcile-test PRIVATE "${SRC}")
set_target_properties(prediction-reconcile-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(prediction-reconcile-test PRIVATE /W4)
else()
  target_compile_options(prediction-reconcile-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME prediction-reconcile COMMAND prediction-reconcile-test)

# --- Deterministic replay harness (#106) ---
# The engine-free desync regression net (Client SAD §9.2 / R5): records a
# (input + server-correction) sequence and replays it through the SAME
# predict()/reconcile()/integrate_tick() path (movement_controller.cpp) so the
# per-tick trace reproduces BIT-IDENTICALLY. Checks record->replay determinism,
# exact serialisation round-trip, tamper detection, and — the regression net —
# that each checked-in fixtures/*.replay golden still replays bit-for-bit. Same
# engine-free discipline: no godot-cpp, plain C++17. Links the SAME core the
# #102/#103 tests use so replay and the live client share ONE code path.
add_executable(replay-harness-test
  replay_harness_test.cpp
  "${SRC}/replay_harness.cpp"
  "${SRC}/movement_controller.cpp"
)
target_include_directories(replay-harness-test PRIVATE "${SRC}")
set_target_properties(replay-harness-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(replay-harness-test PRIVATE /W4)
else()
  target_compile_options(replay-harness-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME replay-harness COMMAND replay-harness-test)

# The golden-fixture GENERATOR (maintenance tool, NOT a ctest): (re)mints the
# checked-in test/fixtures/*.replay from replay_fixture_defs.h. Run once and commit
# the output; the test above then guards it. Kept out of `ctest` so a normal test
# run never rewrites the golden it verifies.
add_executable(replay-fixture-gen
  replay_fixture_gen.cpp
  "${SRC}/replay_harness.cpp"
  "${SRC}/movement_controller.cpp"
)
target_include_directories(replay-fixture-gen PRIVATE "${SRC}")
set_target_properties(replay-fixture-gen PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(replay-fixture-gen PRIVATE /W4)
else()
  target_compile_options(replay-fixture-gen PRIVATE -Wall -Wextra)
endif()

# --- Remote-entity interpolation + clock-sync estimator core (#104) ---
# The engine-free REMOTE-side smoothing core (remote_interpolation.*): buffers
# remote EntityUpdate snapshots and renders an interpolated position at
# `server_now - interp_delay`, plus a filtered client<->server clock-offset
# estimator (Client SAD §2.2 interpolation clock / §5.1 clock sync). Distinct
# from the #102 LOCAL prediction. Same engine-free discipline: no godot-cpp,
# plain C++17; time + snapshots injected so every assertion is deterministic.
# Depends on movement_controller.cpp only for the shared Vec3 value type.
add_executable(remote-interpolation-test
  remote_interpolation_test.cpp
  "${SRC}/remote_interpolation.cpp"
  "${SRC}/movement_controller.cpp"
)
target_include_directories(remote-interpolation-test PRIVATE "${SRC}")
set_target_properties(remote-interpolation-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(remote-interpolation-test PRIVATE /W4)
else()
  target_compile_options(remote-interpolation-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME remote-interpolation COMMAND remote-interpolation-test)

# --- Third-person WoW camera core (#105) ---
# The engine-free WoW-style camera state machine (tps_camera_core.*): steer-vs-
# orbit mode selection, yaw/pitch accumulation + clamps, wheel-zoom clamping, and
# the collision-boom springs (instant pull-in, smooth spring-out). Same engine-
# free discipline: no godot-cpp, plain C++17; the Godot node (meridian_tps_camera)
# is a thin wrapper that only supplies the per-frame collision distance.
add_executable(tps-camera-core-test
  tps_camera_core_test.cpp
  "${SRC}/tps_camera_core.cpp"
)
target_include_directories(tps-camera-core-test PRIVATE "${SRC}")
set_target_properties(tps-camera-core-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(tps-camera-core-test PRIVATE /W4)
else()
  target_compile_options(tps-camera-core-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME tps-camera-core COMMAND tps-camera-core-test)

# --- Settings store + first-run auto-benchmark core (#108) ---
# The engine-free client settings store (settings_core.*): the typed, categorised
# settings model (graphics/audio/input/network) + INI (de)serialisation with
# corrupt-file tolerance, the benchmark->quality-tier mapping, and the first-run-
# once orchestration + CPU-probe benchmark SKELETON. Same engine-free discipline:
# no godot-cpp, plain C++17; the Godot node (meridian_settings) is a thin wrapper
# that only does user:// file I/O + marshals typed values into GDScript.
add_executable(settings-core-test
  settings_core_test.cpp
  "${SRC}/settings_core.cpp"
)
target_include_directories(settings-core-test PRIVATE "${SRC}")
set_target_properties(settings-core-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(settings-core-test PRIVATE /W4)
else()
  target_compile_options(settings-core-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME settings-core COMMAND settings-core-test)

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

# --- Crash report core + upload queue (#109) ---
# The engine-free CLIENT crash channel: the .mcrash report-file (de)serialiser,
# the Sentry-compatible crash envelope (reuses the #167 ingest shape), and the
# upload queue that ships pending reports through the SAME telemetry ITransport
# seam a MockTransport stands in for. Same engine-free discipline: no godot-cpp,
# plain C++17. Links crash_report_core + crash_upload_queue only.
add_executable(crash-report-core-test
  crash_report_core_test.cpp
  "${SRC}/crash_report_core.cpp"
  "${SRC}/crash_upload_queue.cpp"
)
target_include_directories(crash-report-core-test PRIVATE "${SRC}")
set_target_properties(crash-report-core-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(crash-report-core-test PRIVATE /W4)
else()
  target_compile_options(crash-report-core-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME crash-report-core COMMAND crash-report-core-test)

# --- Controlled crash-handler smoke test (#109) ---
# Exercises the REAL POSIX fatal-signal handler: forks a child that installs the
# handler and deliberately crashes (SIGSEGV/SIGABRT), then asserts a parseable
# .mcrash report was written — WITHOUT killing the runner (the crash is in the
# child). POSIX-only; SKIPs (exit 0) elsewhere. Links crash_handler + the cores.
add_executable(crash-handler-smoke-test
  crash_handler_smoke_test.cpp
  "${SRC}/crash_handler.cpp"
  "${SRC}/crash_report_core.cpp"
  "${SRC}/crash_upload_queue.cpp"
)
target_include_directories(crash-handler-smoke-test PRIVATE "${SRC}")
set_target_properties(crash-handler-smoke-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(crash-handler-smoke-test PRIVATE /W4)
else()
  target_compile_options(crash-handler-smoke-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME crash-handler-smoke COMMAND crash-handler-smoke-test)

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

# --- IF-6/IF-8 fail-closed CHUNK-PACK verify core (#554) ---
# The engine-free packaging system that sits on top of the #107 pack-manifest
# verify: it loads the IF-6 zone chunk index (`<zone>.chunks.json`), resolves
# every per-chunk ref through the IF-8 asset-id table (`<zone>.assets.json`), and
# proves the pack is COMPLETE + INTACT (presence + per-chunk both-payloads BLAKE3)
# before the world-enter path proceeds. Same engine-free discipline: no godot-cpp,
# plain C++17. It REUSES the ONE vendored BLAKE3 the mcc emitter uses
# (tools/mcc/src/hash/blake3.*) — no second BLAKE3 in the tree (#554 directive) —
# so the recomputed per-chunk hash and the manifest hash agree byte-for-byte. The
# golden fixture (test/fixtures/chunkpack/) is Story-0's `mcc chunk-emit` output.
get_filename_component(MERIDIAN_MCC_HASH_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tools/mcc/src" ABSOLUTE)
add_executable(chunk-pack-core-test
  chunk_pack_core_test.cpp
  "${SRC}/chunk_pack_core.cpp"
  "${MERIDIAN_MCC_HASH_DIR}/hash/blake3.cpp"
)
target_include_directories(chunk-pack-core-test PRIVATE "${SRC}" "${MERIDIAN_MCC_HASH_DIR}")
set_target_properties(chunk-pack-core-test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(chunk-pack-core-test PRIVATE /W4)
else()
  target_compile_options(chunk-pack-core-test PRIVATE -Wall -Wextra)
endif()

add_test(NAME chunk-pack-core COMMAND chunk-pack-core-test)

# --- IF-1/IF-2 client login core (#99) ---
# The engine-free client login core (login_core.*, srp_client_core.*): the CLIENT
# side of the IF-1 login state machine authd drives (#79) + the IF-2 WorldHello
# kickoff (#84). Unlike the pure-C++17 cores above, this one takes REAL deps — the
# SAME crypto/wire stack the server uses so the two interoperate exactly:
#   - OpenSSL      : SRP-6a BIGNUM/EVP + HMAC + (TLS transport, compiled here).
#   - FlatBuffers  : auth.fbs / world.fbs codegen (via flatc) + the runtime.
#   - meridian-srp : the SERVER-side ServerSession, so the mock server in the test
#                    runs a genuine SRP exchange against the core's client side.
# This mirrors the server login_flow_test discipline (a real SRP handshake), but
# WITHOUT a database — the wire interop over a live authd is the separate
# integration test (client/test/authd_login_it, gated on MERIDIAN_DB_*).
find_package(OpenSSL)
find_package(FlatBuffers CONFIG QUIET)

# Resolve the repo root (client/gdextension/meridian/test -> repo root is 5 up).
get_filename_component(MERIDIAN_REPO_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(MERIDIAN_SCHEMA_NET_DIR "${MERIDIAN_REPO_ROOT}/schema/net")
set(MERIDIAN_SRP_SRC "${MERIDIAN_REPO_ROOT}/server/libs/srp")

# The login-core test needs OpenSSL + a `flatc` (system or fetched). If neither
# OpenSSL nor flatc is available, skip THIS target only (the pure cores above
# still build) — honest degradation on a bare toolchain.
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
endif()

if(OpenSSL_FOUND AND DEFINED MERIDIAN_FLATC)
  # flatc --cpp on schema/net/{auth,world}.fbs -> *_generated.h in the build tree.
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
    DEPENDS "${MERIDIAN_SCHEMA_NET_DIR}/auth.fbs"
            "${MERIDIAN_SCHEMA_NET_DIR}/world.fbs"
    COMMENT "login-core-test: flatc --cpp on schema/net/{auth,world}.fbs"
    VERBATIM)
  add_custom_target(login-core-codegen DEPENDS ${MERIDIAN_LOGIN_GENERATED})

  add_executable(login-core-test
    login_core_test.cpp
    "${SRC}/login_core.cpp"
    "${SRC}/srp_client_core.cpp"
    "${SRC}/login_transport.cpp"      # compiled for coverage (not run: no live socket)
    "${SRC}/version_compat.cpp"       # #98 schema-version compat (login_core dep)
    "${MERIDIAN_SRP_SRC}/src/srp.cpp" # server-side ServerSession for the mock
  )
  add_dependencies(login-core-test login-core-codegen)
  target_include_directories(login-core-test PRIVATE
    "${SRC}"
    "${MERIDIAN_LOGIN_GEN_DIR}"
    "${MERIDIAN_SRP_SRC}/include")

  # The flatbuffers runtime headers: prefer the CONFIG package; else assume the
  # system include path already has <flatbuffers/flatbuffers.h> (Homebrew/apt).
  if(TARGET flatbuffers::flatbuffers)
    target_link_libraries(login-core-test PRIVATE flatbuffers::flatbuffers)
  endif()
  target_link_libraries(login-core-test PRIVATE OpenSSL::SSL OpenSSL::Crypto)

  set_target_properties(login-core-test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(login-core-test PRIVATE /W4)
  else()
    target_compile_options(login-core-test PRIVATE -Wall -Wextra)
  endif()

  add_test(NAME login-core COMMAND login-core-test)

  # --- Live-authd integration test (#99) ---
  # THE interop proof: the client login CORE + TlsLoginTransport driven over a
  # REAL TLS socket against a RUNNING authd (client/test/run_authd_login_it.sh
  # boots MariaDB on a UNIQUE socket/port, seeds an account+realm, launches the
  # real authd, and runs this pointed at it). Inert (SKIP, exit 0) without the
  # harness's --host/--port args, so it is safe in a plain ctest. Registered as a
  # ctest only for `ctest -R authd-login-it` from the harness; the harness passes
  # the connection args, so the ctest registration here is a no-arg SKIP.
  add_executable(authd-login-it
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../test/authd_login_it.cpp"
    "${SRC}/login_core.cpp"
    "${SRC}/srp_client_core.cpp"
    "${SRC}/login_transport.cpp"
    "${SRC}/version_compat.cpp"  # #98 schema-version compat (login_core dep)
  )
  add_dependencies(authd-login-it login-core-codegen)
  target_include_directories(authd-login-it PRIVATE
    "${SRC}" "${MERIDIAN_LOGIN_GEN_DIR}")
  if(TARGET flatbuffers::flatbuffers)
    target_link_libraries(authd-login-it PRIVATE flatbuffers::flatbuffers)
  endif()
  target_link_libraries(authd-login-it PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  set_target_properties(authd-login-it PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(NOT MSVC)
    target_compile_options(authd-login-it PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME authd-login-it COMMAND authd-login-it)  # no args -> SKIP

  # --- #510 realm-list SOAK integration test ---
  # A many-account/many-session soak over the SAME real client core + TLS transport
  # as authd-login-it, driven by run_authd_realm_list_soak_it.sh. It exists because
  # the single-shot IT above passes on loopback while the live dev realm sees an
  # intermittent, per-account "empty realm list" at the RealmListRequest->RealmList
  # step (#510). Inert (SKIP) with no args; the harness passes --host/--port +
  # --soak-prefix/--soak-count so CI would catch a re-introduced intermittent
  # realm-list regression.
  add_executable(authd-realm-list-soak-it
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../test/authd_realm_list_soak_it.cpp"
    "${SRC}/login_core.cpp"
    "${SRC}/srp_client_core.cpp"
    "${SRC}/login_transport.cpp"
    "${SRC}/version_compat.cpp"  # #98 schema-version compat (login_core dep)
  )
  add_dependencies(authd-realm-list-soak-it login-core-codegen)
  target_include_directories(authd-realm-list-soak-it PRIVATE
    "${SRC}" "${MERIDIAN_LOGIN_GEN_DIR}")
  if(TARGET flatbuffers::flatbuffers)
    target_link_libraries(authd-realm-list-soak-it PRIVATE flatbuffers::flatbuffers)
  endif()
  target_link_libraries(authd-realm-list-soak-it PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  set_target_properties(authd-realm-list-soak-it PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(NOT MSVC)
    target_compile_options(authd-realm-list-soak-it PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME authd-realm-list-soak-it COMMAND authd-realm-list-soak-it)  # no args -> SKIP
else()
  message(STATUS
    "login-core-test SKIPPED: needs OpenSSL + flatc (OpenSSL_FOUND=${OpenSSL_FOUND}).")
endif()

# --- Lock-free SPSC ring core (#97) ---
# The header-only single-producer/single-consumer ring (spsc_ring.h) — the ONE
# cross-thread channel between the net thread and the Godot main thread. Engine-free
# and net-core-free: it links nothing but the threading runtime, so it always builds
# in the plain MERIDIAN_CLIENT_TESTS suite. The test asserts FIFO/full/empty,
# wrap-around, and CONCURRENT producer/consumer correctness (no lost/duplicated msgs).
add_executable(spsc-ring-test spsc_ring_test.cpp)
target_include_directories(spsc-ring-test PRIVATE "${SRC}")
target_link_libraries(spsc-ring-test PRIVATE Threads::Threads)
set_target_properties(spsc-ring-test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(spsc-ring-test PRIVATE /W4)
else()
  target_compile_options(spsc-ring-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME spsc-ring COMMAND spsc-ring-test)

# --- Net-thread core integration (#97) ---
# Drives net::NetThreadCore against a MOCK clientnet::ITransport over the dedicated
# thread (login->world->move, priority send order, connect-failure lifecycle). Needs
# the #95 net core (meridian-clientnet) for the framing/wire_frame/codec/AEAD stack,
# so it builds ONLY when that target exists (configure with BOTH -DMERIDIAN_CLIENT_TESTS=ON
# and -DMERIDIAN_CLIENT_NET=ON). Without the net core it is skipped (the SPSC test above
# still covers the lock-free primitive in a plain tests-only build).
if(TARGET meridian-clientnet)
  add_executable(net-thread-core-test
    net_thread_core_test.cpp
    "${SRC}/net_thread_core.cpp"
  )
  target_include_directories(net-thread-core-test PRIVATE "${SRC}")
  target_link_libraries(net-thread-core-test PRIVATE meridian-clientnet Threads::Threads)
  set_target_properties(net-thread-core-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(net-thread-core-test PRIVATE /W4)
  else()
    target_compile_options(net-thread-core-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME net-thread-core COMMAND net-thread-core-test)
else()
  message(STATUS
    "net-thread-core-test SKIPPED: needs meridian-clientnet (-DMERIDIAN_CLIENT_NET=ON).")
endif()

# --- Heightfield SAME-BYTES decode proof (#557) ---
# The Q1(a) closing test: decode the Heightfield straight out of the REAL shipped
# `<cx>_<cz>.chunk.bin` (Story-0 fixture, test/fixtures/chunkpack/) via the flatc-
# generated chunk.fbs bindings, then bilinear-sample the decoded grid — proving the
# client reads the SAME bytes worldd validates against. It needs a `flatc` + the
# FlatBuffers runtime, so (like login-core) it is gated on their availability; the
# pure heightfield-query-test above always runs regardless. Reuses MERIDIAN_FLATC /
# MERIDIAN_REPO_ROOT resolved by the login-core block above.
if(DEFINED MERIDIAN_FLATC)
  set(MERIDIAN_CHUNK_FBS "${MERIDIAN_REPO_ROOT}/schema/chunk/chunk.fbs")
  set(MERIDIAN_CHUNK_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-chunk")
  file(MAKE_DIRECTORY "${MERIDIAN_CHUNK_GEN_DIR}")
  set(MERIDIAN_CHUNK_GENERATED "${MERIDIAN_CHUNK_GEN_DIR}/chunk_generated.h")
  add_custom_command(
    OUTPUT ${MERIDIAN_CHUNK_GENERATED}
    COMMAND ${MERIDIAN_FLATC} --cpp --scoped-enums -o "${MERIDIAN_CHUNK_GEN_DIR}"
            "${MERIDIAN_CHUNK_FBS}"
    DEPENDS "${MERIDIAN_CHUNK_FBS}"
    COMMENT "heightfield-decode-test: flatc --cpp on schema/chunk/chunk.fbs"
    VERBATIM)
  add_custom_target(heightfield-chunk-codegen DEPENDS ${MERIDIAN_CHUNK_GENERATED})

  add_executable(heightfield-decode-test
    heightfield_decode_test.cpp
    "${SRC}/heightfield_query.cpp"
    "${SRC}/heightfield_chunk_decode.cpp"
  )
  add_dependencies(heightfield-decode-test heightfield-chunk-codegen)
  target_include_directories(heightfield-decode-test PRIVATE
    "${SRC}" "${MERIDIAN_CHUNK_GEN_DIR}")
  if(TARGET flatbuffers::flatbuffers)
    target_link_libraries(heightfield-decode-test PRIVATE flatbuffers::flatbuffers)
  endif()
  set_target_properties(heightfield-decode-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(heightfield-decode-test PRIVATE /W4)
  else()
    target_compile_options(heightfield-decode-test PRIVATE -Wall -Wextra)
  endif()

  add_test(NAME heightfield-decode COMMAND heightfield-decode-test)
else()
  message(STATUS
    "heightfield-decode-test SKIPPED: needs flatc (schema/chunk/chunk.fbs codegen).")
endif()
