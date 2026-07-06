// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — version & build identity implementation.
// Clean-room per CONTRIBUTING.md.

#include "meridian/core/version.hpp"

#include <string>

namespace meridian::core {

namespace {

// Compiler identity, resolved at compile time. Kept in build_info() output so a
// crash report or `--version` pins the toolchain that produced the binary.
std::string compiler_id() {
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." +
           std::to_string(__clang_minor__) + "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) +
           "." + std::to_string(__GNUC_PATCHLEVEL__);
#else
    return "unknown";
#endif
}

// C++ standard the translation unit was compiled against.
std::string cxx_standard() {
#if defined(__cplusplus)
    if (__cplusplus >= 202002L) return "C++20";
    return "pre-C++20";
#else
    return "unknown";
#endif
}

}  // namespace

std::string version_string() {
    return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
           std::to_string(kVersionPatch);
}

std::string build_info() {
    // __DATE__/__TIME__ stamp the build wall-clock. Reproducible-build hardening
    // (SOURCE_DATE_EPOCH) is a packaging concern for D-30, not the M0 skeleton.
    std::string out;
    out += kProjectName;
    out += " ";
    out += version_string();
    out += " (";
    out += kMilestone;
    out += ")";
    out += "\n  compiler: ";
    out += compiler_id();
    out += "\n  standard: ";
    out += cxx_standard();
    out += "\n  built:    ";
    out += __DATE__;
    out += " ";
    out += __TIME__;
    return out;
}

}  // namespace meridian::core
