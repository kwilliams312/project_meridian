// Project Meridian — MeridianLogin GDExtension binding (issue #99).
// Thin Godot wrapper over the tested engine-free IF-1/IF-2 client login core.
// All protocol + crypto policy lives in the core (login_core.* / srp_client_core.*
// / login_transport.*); this file marshals Godot types in/out and caches the grant
// for the IF-2 world handshake. See meridian_login.h.

#include "meridian_login.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <string>

#include "login_core.h"
#include "login_transport.h"

using namespace godot;

namespace meridian {

namespace {

std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

// Copy a std::vector<uint8_t> into a Godot PackedByteArray.
PackedByteArray to_pba(const std::vector<std::uint8_t> &b) {
	PackedByteArray a;
	a.resize(static_cast<int64_t>(b.size()));
	if (!b.empty()) {
		std::memcpy(a.ptrw(), b.data(), b.size());
	}
	return a;
}

// Map login::LoginStatus -> the bound GDScript ordinal (identical ordering).
int status_ordinal(login::LoginStatus s) {
	return static_cast<int>(s);
}

const char *status_cstr(login::LoginStatus s) {
	switch (s) {
		case login::LoginStatus::kSuccess: return "success";
		case login::LoginStatus::kConnectFailed: return "connect-failed";
		case login::LoginStatus::kProtocolMismatch: return "protocol-mismatch";
		case login::LoginStatus::kBadCredentials: return "bad-credentials";
		case login::LoginStatus::kServerProofFailed: return "server-proof-failed";
		case login::LoginStatus::kRealmUnavailable: return "realm-unavailable";
		case login::LoginStatus::kProtocolError: return "protocol-error";
		case login::LoginStatus::kTransportClosed: return "transport-closed";
	}
	return "unknown";
}

} // namespace

MeridianLogin::MeridianLogin() = default;
MeridianLogin::~MeridianLogin() = default;

void MeridianLogin::set_client_build(int build) {
	cfg_.client_build = static_cast<std::uint32_t>(build);
}
int MeridianLogin::get_client_build() const {
	return static_cast<int>(cfg_.client_build);
}
void MeridianLogin::set_proto_ver(int proto) {
	cfg_.proto_ver = static_cast<std::uint16_t>(proto);
}
int MeridianLogin::get_proto_ver() const {
	return static_cast<int>(cfg_.proto_ver);
}

Dictionary MeridianLogin::result_to_dict(const login::LoginResult &r,
		const std::vector<login::RealmInfo> &realms, const String &tls_version) {
	Dictionary d;
	d["ok"] = r.ok();
	d["status"] = status_ordinal(r.status);
	d["detail"] = String::utf8(r.detail.c_str());
	d["error_code"] = static_cast<int>(r.server_error_code);
	// grant_id is a full-range u64; Godot ints are signed 64-bit. Reinterpret the
	// bits (never truncate) — the scene treats it as an opaque token, and it is
	// re-serialized as the same u64 by build_world_hello.
	d["grant_id"] = static_cast<int64_t>(r.grant_id);
	d["session_key"] = to_pba(r.session_key);
	d["reconnect_window_ms"] = static_cast<int>(r.reconnect_window_ms);
	d["realm_id"] = static_cast<int>(r.selected_realm_id);
	d["tls_version"] = tls_version;

	Array realm_arr;
	for (const login::RealmInfo &ri : realms) {
		Dictionary rd;
		rd["id"] = static_cast<int>(ri.id);
		rd["name"] = String::utf8(ri.name.c_str());
		rd["address"] = String::utf8(ri.address.c_str());
		rd["port"] = static_cast<int>(ri.port);
		rd["population"] = static_cast<int>(ri.population);
		rd["build_min"] = static_cast<int>(ri.build_min);
		rd["build_max"] = static_cast<int>(ri.build_max);
		rd["flags"] = static_cast<int>(ri.flags);
		realm_arr.push_back(rd);
	}
	d["realms"] = realm_arr;

	// Cache the grant on success so the scene can build the WorldHello without
	// re-parsing this Dictionary; a failure clears any prior grant.
	if (r.ok()) {
		has_grant_ = true;
		grant_ = r;
	} else {
		has_grant_ = false;
	}
	return d;
}

Dictionary MeridianLogin::login(const String &host, int port, const String &account,
		const String &password, int realm_id) {
	const std::string host_s = to_std(host);
	const auto port_u = static_cast<std::uint16_t>(port);

	// Establish the TLS 1.3 transport. A connect/handshake failure is a clean
	// kConnectFailed result the scene branches on (never an exception across the
	// GDExtension boundary).
	login::TlsLoginTransport transport(host_s, port_u);
	String tls_version = String::utf8(transport.tls_version().c_str());
	if (!transport.ok()) {
		login::LoginResult r;
		r.status = login::LoginStatus::kConnectFailed;
		r.detail = transport.error();
		UtilityFunctions::push_warning(
				String("MeridianLogin: connect failed — ") + String::utf8(r.detail.c_str()));
		return result_to_dict(r, {}, tls_version);
	}

	// If the caller pinned a realm id, honor it via a chooser closure captured
	// through a thread-local (function-pointer signature has no capture slot). A 0
	// realm_id means "let the core auto-select the first build-compatible realm".
	std::vector<login::RealmInfo> realms;
	login::LoginResult r;
	if (realm_id > 0) {
		// A tiny static shim: stash the pinned id where the C-style chooser reads it.
		static thread_local std::uint32_t pinned_realm = 0;
		pinned_realm = static_cast<std::uint32_t>(realm_id);
		auto pinned_chooser = [](const std::vector<login::RealmInfo> &,
									  const login::LoginConfig &) -> std::uint32_t {
			return pinned_realm;
		};
		r = login::run_login(transport, cfg_, to_std(account), to_std(password),
				pinned_chooser, &realms);
	} else {
		r = login::run_login(transport, cfg_, to_std(account), to_std(password),
				nullptr, &realms);
	}

	if (!r.ok()) {
		UtilityFunctions::push_warning(
				String("MeridianLogin: login ") + String(status_cstr(r.status)) + " — " +
				String::utf8(r.detail.c_str()));
	}
	return result_to_dict(r, realms, tls_version);
}

PackedByteArray MeridianLogin::build_world_hello_frame() {
	if (!has_grant_) {
		UtilityFunctions::push_warning(
				"MeridianLogin: build_world_hello_frame() with no grant — login first");
		return PackedByteArray();
	}
	std::vector<std::uint8_t> nonce;
	std::vector<std::uint8_t> hello =
			login::build_world_hello(grant_, cfg_.client_build, &nonce);
	world_hello_nonce_ = nonce;
	std::vector<std::uint8_t> frame =
			login::encode_world_frame(login::kOpcodeWorldHello, /*seq=*/0, hello);
	return to_pba(frame);
}

bool MeridianLogin::has_grant() const {
	return has_grant_;
}
int64_t MeridianLogin::get_grant_id() const {
	return static_cast<int64_t>(grant_.grant_id);
}
PackedByteArray MeridianLogin::get_session_key() const {
	return to_pba(grant_.session_key);
}
int MeridianLogin::get_realm_id() const {
	return static_cast<int>(grant_.selected_realm_id);
}
PackedByteArray MeridianLogin::get_world_hello_nonce() const {
	return to_pba(world_hello_nonce_);
}

String MeridianLogin::status_name(int status) {
	return String(status_cstr(static_cast<login::LoginStatus>(status)));
}

void MeridianLogin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_client_build", "build"),
			&MeridianLogin::set_client_build);
	ClassDB::bind_method(D_METHOD("get_client_build"), &MeridianLogin::get_client_build);
	ClassDB::bind_method(D_METHOD("set_proto_ver", "proto"), &MeridianLogin::set_proto_ver);
	ClassDB::bind_method(D_METHOD("get_proto_ver"), &MeridianLogin::get_proto_ver);

	ClassDB::bind_method(
			D_METHOD("login", "host", "port", "account", "password", "realm_id"),
			&MeridianLogin::login);

	ClassDB::bind_method(D_METHOD("build_world_hello_frame"),
			&MeridianLogin::build_world_hello_frame);

	ClassDB::bind_method(D_METHOD("has_grant"), &MeridianLogin::has_grant);
	ClassDB::bind_method(D_METHOD("get_grant_id"), &MeridianLogin::get_grant_id);
	ClassDB::bind_method(D_METHOD("get_session_key"), &MeridianLogin::get_session_key);
	ClassDB::bind_method(D_METHOD("get_realm_id"), &MeridianLogin::get_realm_id);
	ClassDB::bind_method(D_METHOD("get_world_hello_nonce"),
			&MeridianLogin::get_world_hello_nonce);

	ClassDB::bind_static_method("MeridianLogin", D_METHOD("status_name", "status"),
			&MeridianLogin::status_name);

	BIND_ENUM_CONSTANT(STATUS_SUCCESS);
	BIND_ENUM_CONSTANT(STATUS_CONNECT_FAILED);
	BIND_ENUM_CONSTANT(STATUS_PROTOCOL_MISMATCH);
	BIND_ENUM_CONSTANT(STATUS_BAD_CREDENTIALS);
	BIND_ENUM_CONSTANT(STATUS_SERVER_PROOF_FAILED);
	BIND_ENUM_CONSTANT(STATUS_REALM_UNAVAILABLE);
	BIND_ENUM_CONSTANT(STATUS_PROTOCOL_ERROR);
	BIND_ENUM_CONSTANT(STATUS_TRANSPORT_CLOSED);
}

} // namespace meridian
