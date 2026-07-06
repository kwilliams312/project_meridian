# SPDX-License-Identifier: Apache-2.0
#
# meridian-net — TLS 1.3 listener for authd's IF-1 auth protocol (server SAD
# §5.1 transport + framing, §2.1 acceptor model). Clean-room from the SAD and the
# OpenSSL public API docs; no GPL source consulted (CONTRIBUTING.md).
#
# Dependency-light by design: BSD sockets + OpenSSL only (OpenSSL::SSL pulls in
# OpenSSL::Crypto). No Asio/Boost at M0 — the caller supplies the acceptor/worker
# threading around TlsListener::accept().

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(meridian-net STATIC
  src/tls_listener.cpp
)
add_library(meridian::net ALIAS meridian-net)

target_include_directories(meridian-net
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(meridian-net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(meridian-net PUBLIC cxx_std_20)
set_target_properties(meridian-net PROPERTIES CXX_EXTENSIONS OFF)

# Loopback integration test: self-signed cert generated in-process (OpenSSL API,
# no external fixtures), listener on an ephemeral port on a background thread, a
# real TLS 1.3 client over loopback. Asserts TLSv1.3, a byte-for-byte framed
# echo round-trip, and rejection of an oversize (>8 KiB) length prefix.
add_executable(meridian-net-test test/tls_listener_test.cpp)
target_link_libraries(meridian-net-test PRIVATE meridian::net Threads::Threads)
set_target_properties(meridian-net-test PROPERTIES CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME net-tls COMMAND meridian-net-test)
