// GDExtension entry point for the Meridian client module.
// Bootstrap scope (#158): registered the placeholder MeridianClient class.
// #102 adds the `sim` kinematic movement controller (MeridianMovementController).
// #168 adds the client telemetry log channel (MeridianTelemetry, D-29).
// #107 adds the boot-scene IF-5 pack mount + manifest verify (MeridianPackMount).
// #99 adds the IF-1/IF-2 client login flow (MeridianLogin): login → realm → world
// handshake — the client half of IT-M0 auth, over TLS 1.3 + SRP-6a.
// #104 adds the REMOTE-entity interpolation + clock-sync estimator
// (MeridianRemoteInterpolator): smooth rendering of OTHER players' movement.
// #97 adds the dedicated NET THREAD (MeridianNetThread): owns the IF-2 world
// session on its own std::thread, hands decoded messages to the main thread over a
// lock-free SPSC ring drained at the pre-sim sync point, with a priority send queue.
// Later issues add the remaining net / stream / datastore classes (Client SAD §2).

#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "meridian_client.h"
#include "meridian_login.h"
#include "meridian_movement_controller.h"
#include "meridian_net_thread.h"
#include "meridian_pack_mount.h"
#include "meridian_remote_interpolator.h"
#include "meridian_telemetry.h"

using namespace godot;

void initialize_meridian_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(meridian::MeridianClient);
	GDREGISTER_CLASS(meridian::MeridianMovementController);
	GDREGISTER_CLASS(meridian::MeridianTelemetry);
	GDREGISTER_CLASS(meridian::MeridianPackMount);
	GDREGISTER_CLASS(meridian::MeridianLogin);
	GDREGISTER_CLASS(meridian::MeridianRemoteInterpolator);
	GDREGISTER_CLASS(meridian::MeridianNetThread);
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
