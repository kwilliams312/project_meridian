// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-core — version & build identity.
//
// Clean-room: designed from the server SAD §2 (shared static libraries) and the
// server PRD §6 (observability). No GPL source consulted; no CMaNGOS/TrinityCore
// code copied. See CONTRIBUTING.md.
//
// This is the M0 skeleton. Version constants live here so every daemon
// (authd/worldd, and later gatewayd/servicesd/coordd) reports one identity, and
// so log/metric context (D-23 realm/zone/shard tagging) can stamp a build id.

#ifndef MERIDIAN_CORE_VERSION_HPP
#define MERIDIAN_CORE_VERSION_HPP

#include <string>

namespace meridian::core {

// Semantic version of the server tree. Pre-M0; bumped as milestones land.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

// Milestone label (server SAD §7 build plan). "M0" == build skeleton phase.
inline constexpr const char* kMilestone = "M0";

// Human-readable project name shared by every daemon.
inline constexpr const char* kProjectName = "Project Meridian server";

// Dotted "major.minor.patch" string, e.g. "0.0.0".
std::string version_string();

// A one-line build identity: name, version, milestone, compiler, C++ standard,
// and build date/time. Consumed by `--version` on every daemon and (later) by
// the structured logger's startup banner. Format is stable-ish but intended for
// humans, not machine parsing.
std::string build_info();

}  // namespace meridian::core

#endif  // MERIDIAN_CORE_VERSION_HPP
