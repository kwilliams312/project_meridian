// Project Meridian — MeridianLogin GDExtension class (issue #99).
//
// The thin Godot binding over the engine-free IF-1/IF-2 client login core
// (login_core.* + srp_client_core.* + login_transport.*). This is the client
// counterpart to the whole server auth path (authd #79 / worldd #84): the boot /
// login scene (Client SAD §2.3 flow `Boot → login → realm select → enter world`)
// hands this class a host + credentials, it drives the full IF-1 login over TLS
// 1.3 (ClientHello → SRP-6a → RealmList → SessionGrant), then exposes the grant +
// session_key + selected realm so the scene can kick off the IF-2 world handshake.
//
// Mirrors the engine-free-core + thin-wrapper pattern of MeridianPackMount (#107)
// and MeridianTelemetry (#168): the wrapper's ONLY jobs are (a) marshal Godot
// String/int args into the core's plain types, (b) run the synchronous login core
// (off Godot's main thread by the caller — Client SAD §6.1), and (c) marshal the
// core's LoginResult / RealmList into GDScript Dictionaries. ALL protocol + crypto
// policy — the IF-1 state machine, SRP-6a, the M2 mutual-auth check, realm-build
// gating, the WorldHello proof — lives in the tested engine-free core.

#ifndef MERIDIAN_LOGIN_H
#define MERIDIAN_LOGIN_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "login_core.h"

namespace meridian {

class MeridianLogin : public godot::RefCounted {
	GDCLASS(MeridianLogin, godot::RefCounted)

public:
	// GDScript-facing status ordinals (mirror login::LoginStatus exactly) so the
	// login scene can branch on `result.status == MeridianLogin.STATUS_SUCCESS`
	// or `== STATUS_BAD_CREDENTIALS` without a magic number.
	enum LoginStatusCode {
		STATUS_SUCCESS = 0,
		STATUS_CONNECT_FAILED = 1,
		STATUS_PROTOCOL_MISMATCH = 2,
		STATUS_BAD_CREDENTIALS = 3,
		STATUS_SERVER_PROOF_FAILED = 4,
		STATUS_REALM_UNAVAILABLE = 5,
		STATUS_PROTOCOL_ERROR = 6,
		STATUS_TRANSPORT_CLOSED = 7,
	};

protected:
	static void _bind_methods();

public:
	MeridianLogin();
	~MeridianLogin();

	// ── Client identity (set once at boot, before login) ─────────────────────
	void set_client_build(int build);   // ClientHello.build (realm build-range gate)
	int get_client_build() const;
	void set_proto_ver(int proto);       // ClientHello.proto_ver (must match authd)
	int get_proto_ver() const;

	// ── The login entry point (the login-scene call) ─────────────────────────
	// Connect to authd at host:port over TLS 1.3, run the full IF-1 login for
	// `account`/`password`, and select a realm. `realm_id` chooses the realm to
	// request a grant for; pass 0 to auto-select the first build-compatible realm.
	//
	// SYNCHRONOUS + BLOCKING: the login scene runs this on a worker thread (Godot
	// Thread / WorkerThreadPool) and marshals the result back to the UI — never on
	// the main thread (Client SAD §6.1). Returns a Dictionary:
	//   {
	//     "ok":                  bool (true only when status == STATUS_SUCCESS),
	//     "status":              int  (LoginStatusCode),
	//     "detail":              String (human-readable note / fallback UX),
	//     "error_code":          int  (auth.fbs AuthErrorCode; 0 = none),
	//     "grant_id":            int  (SessionGrant.grant_id; IF-3 token),
	//     "session_key":         PackedByteArray (32 B; IF-2 HKDF root),
	//     "reconnect_window_ms": int  (ISSUE #66 server-owned window),
	//     "realm_id":            int  (the realm we were granted for),
	//     "realms":              Array of realm Dictionaries (the RealmList; for a
	//                            picker — populated whenever the list was reached),
	//     "tls_version":         String ("TLSv1.3" on a good connect, else ""),
	//   }
	// On a successful login the grant + session_key + realm are ALSO cached on this
	// instance (get_grant_id / get_session_key / get_realm_id / has_grant) so the
	// scene can build the WorldHello without re-parsing the Dictionary.
	godot::Dictionary login(const godot::String &host, int port,
			const godot::String &account, const godot::String &password,
			int realm_id);

	// ── IF-2 world-handshake kickoff ─────────────────────────────────────────
	// Build the WorldHello IF-2 frame (u16 opcode ‖ u64 seq ‖ WorldHello FB) the
	// client sends as the FIRST frame to worldd, from the cached grant of the last
	// successful login(). Returns an empty PackedByteArray if there is no grant.
	// The nonce used is cached (get_world_hello_nonce) so the scene can later
	// verify the server_proof in HandshakeOk (once worldd wires it — #84 leaves it
	// empty at M0).
	godot::PackedByteArray build_world_hello_frame();

	// ── Cached result of the last successful login() ─────────────────────────
	bool has_grant() const;                       // last login() reached kSuccess
	int64_t get_grant_id() const;                 // SessionGrant.grant_id
	godot::PackedByteArray get_session_key() const;  // 32 B IF-2 HKDF root
	int get_realm_id() const;                     // the granted realm
	godot::PackedByteArray get_world_hello_nonce() const;  // last WorldHello nonce

	// Human-readable status name (logs / diagnostics; e.g. "bad-credentials").
	static godot::String status_name(int status);

private:
	// Assemble the return Dictionary from a core result + cache identity on success.
	godot::Dictionary result_to_dict(const login::LoginResult &r,
			const std::vector<login::RealmInfo> &realms,
			const godot::String &tls_version);

	login::LoginConfig cfg_;

	// Cached grant from the last successful login().
	bool has_grant_ = false;
	login::LoginResult grant_;   // status/grant_id/session_key/realm on success
	std::vector<std::uint8_t> world_hello_nonce_;
};

} // namespace meridian

VARIANT_ENUM_CAST(meridian::MeridianLogin::LoginStatusCode);

#endif // MERIDIAN_LOGIN_H
