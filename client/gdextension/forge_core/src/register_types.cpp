// SPDX-License-Identifier: Apache-2.0
//
// GDExtension entry point for the Forge `forge_core` module (issue #134).
// Skeleton scope: registers the single ForgeCore shim class the editor plugin
// (addons/meridian_forge/) calls into — proving the EditorPlugin↔GDExtension
// bridge (Tools SAD §5 / §8 M0 exit). Later Forge issues add the chunk exporter,
// Recast bake, and the full ITerrainBackend (SAD §5.1–§5.4).
//
// ForgeCore registers at the SCENE initialization level: the Godot editor loads
// SCENE-level classes, so an EditorPlugin can instantiate ForgeCore. This keeps
// forge_core a plain data/logic extension (no editor-only engine classes), which
// is why the same binary is harmless if ever loaded by a non-editor host.

#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "forge_core.h"

using namespace godot;

void initialize_forge_core_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(forge::ForgeCore);
}

void uninitialize_forge_core_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C" {
// GDExtension library entry point. Must match `entry_symbol` in
// forge/project/forge_core.gdextension.
GDExtensionBool GDE_EXPORT forge_core_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_forge_core_module);
	init_obj.register_terminator(uninitialize_forge_core_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
