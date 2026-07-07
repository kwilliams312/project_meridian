// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — Forge `forge_core` GDExtension: engine-free VERSION core
// (issue #134, the M0 EditorPlugin-skeleton de-risk).
//
// Forge is the in-Godot terrain/world-building editor plugin (Tools SAD §5). Its
// heavy logic lives in the `forge_core` GDExtension (C++20) behind a thin editor-
// facing layer, so the editor-plugin↔native bridge is the one architecture piece
// this skeleton has to prove (SAD §8 M0 exit: "EditorPlugin skeleton: one dock,
// one gizmoed node, one forge_core call"; PRD R3/R7).
//
// ENGINE-FREE + DEPENDENCY-FREE (mirrors the client-core discipline, Client SAD
// §9.2): plain C++17-compatible, no Godot, no third-party deps, so it unit-tests
// as pure logic (forge_version_test.cpp) in a plain ctest with NO Godot runtime.
// The Godot-facing wrapper (ForgeCore, forge_core.*) is a thin shim over this.

#ifndef FORGE_CORE_FORGE_VERSION_H
#define FORGE_CORE_FORGE_VERSION_H

namespace forge {

// The forge_core build/version string, surfaced to the editor plugin as the
// proof-of-bridge value (the dock renders it via ForgeCore.version()). Bumped
// alongside the client/ENGINE_VERSION pin. Static storage — safe to return as a
// C string and to wrap in a godot::String without allocation concerns.
const char* forge_core_version();

// The bare semantic version ("MAJOR.MINOR.PATCH"), no engine suffix. Useful for
// a machine-comparable check without parsing the human string above.
const char* forge_core_semver();

}  // namespace forge

#endif  // FORGE_CORE_FORGE_VERSION_H
