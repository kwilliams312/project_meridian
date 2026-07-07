// GDExtension entry point for the Meridian client module.
// Bootstrap scope (#158): registered the placeholder MeridianClient class.
// #102 adds the `sim` kinematic movement controller (MeridianMovementController).
// Later issues add the remaining net / stream / datastore classes (Client SAD §2).

#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "meridian_client.h"
#include "meridian_movement_controller.h"

using namespace godot;

void initialize_meridian_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(meridian::MeridianClient);
	GDREGISTER_CLASS(meridian::MeridianMovementController);
}

void uninitialize_meridian_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C" {
// GDExtension library entry point. The symbol name here must match the
// `entry_symbol` in project/meridian.gdextension.
GDExtensionBool GDE_EXPORT meridian_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_meridian_module);
	init_obj.register_terminator(uninitialize_meridian_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
