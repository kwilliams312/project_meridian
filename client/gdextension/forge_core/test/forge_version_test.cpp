// SPDX-License-Identifier: Apache-2.0
//
// Engine-free unit test for the forge_core version core (issue #134). No Godot.

#include "forge_version.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

int main() {
    const char* ver = forge::forge_core_version();
    const char* semver = forge::forge_core_semver();

    check(ver != nullptr && std::strlen(ver) > 0, "version string non-empty");
    check(semver != nullptr && std::strlen(semver) > 0, "semver non-empty");

    // The human string mentions forge_core and the pinned engine.
    check(std::strstr(ver, "forge_core") != nullptr, "version mentions forge_core");
    check(std::strstr(ver, "4.7") != nullptr, "version mentions the 4.7 engine pin");

    // The human string embeds the bare semver.
    check(std::strstr(ver, semver) != nullptr, "version embeds the semver");

    // Stable across calls (static storage, no per-call allocation).
    check(forge::forge_core_version() == ver, "version pointer stable across calls");

    if (g_failures == 0) {
        std::printf("forge_version: all checks passed\n");
        return 0;
    }
    std::printf("forge_version: %d check(s) FAILED\n", g_failures);
    return 1;
}
