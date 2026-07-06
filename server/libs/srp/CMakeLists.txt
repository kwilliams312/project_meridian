# SPDX-License-Identifier: Apache-2.0
#
# meridian-srp — SRP-6a authentication (server SAD §2.1, §5.1). Clean-room from
# RFC 5054 / RFC 2945; no GPL source consulted (CONTRIBUTING.md). Depends on
# OpenSSL (BIGNUM + EVP digests) — the first server lib to take a real dep.

find_package(OpenSSL REQUIRED)

add_library(meridian-srp STATIC
  src/srp.cpp
)
add_library(meridian::srp ALIAS meridian-srp)

target_include_directories(meridian-srp
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(meridian-srp PUBLIC OpenSSL::Crypto)
target_compile_features(meridian-srp PUBLIC cxx_std_20)
set_target_properties(meridian-srp PROPERTIES CXX_EXTENSIONS OFF)

# Unit test: RFC 5054 Appendix B vectors + round-trip + negative cases.
# Dependency-free (assert-based) so it needs no test framework.
add_executable(meridian-srp-test test/srp_test.cpp)
target_link_libraries(meridian-srp-test PRIVATE meridian::srp)
set_target_properties(meridian-srp-test PROPERTIES CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME srp COMMAND meridian-srp-test)
