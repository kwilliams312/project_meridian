// SPDX-License-Identifier: Apache-2.0
//
// forge_core VERSION core implementation (issue #134). See forge_version.h.

#include "forge_version.h"

namespace forge {

// Skeleton version. Bumped alongside the client/ENGINE_VERSION pin (Godot 4.7).
static const char* kForgeCoreSemver = "0.0.1";
static const char* kForgeCoreVersion = "forge_core 0.0.1 (godot 4.7-stable)";

const char* forge_core_version() { return kForgeCoreVersion; }

const char* forge_core_semver() { return kForgeCoreSemver; }

}  // namespace forge
