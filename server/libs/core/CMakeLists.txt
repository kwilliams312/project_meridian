# SPDX-License-Identifier: Apache-2.0
#
# libmeridian-core — shared static library (server SAD §2 "Shared static
# libraries"). M0 skeleton: logging, config, version/build identity. The db /
# proto / game siblings land in later milestones and slot in beside this.
#
# Zero third-party dependencies by design at M0. spdlog/OpenSSL/etc. arrive with
# the real implementations they belong to.

add_library(meridian-core STATIC
  src/version.cpp
  src/log.cpp
  src/config.cpp
)

# Canonical alias so consumers link `meridian::core` — decouples the link name
# from the raw target and matches how the later libs will expose themselves.
add_library(meridian::core ALIAS meridian-core)

target_include_directories(meridian-core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(meridian-core PUBLIC cxx_std_20)

set_target_properties(meridian-core PROPERTIES
  CXX_EXTENSIONS OFF
)
