// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free client LOGIN core: the IF-1 login state machine
// + the IF-2 world-handshake kickoff (issue #99).
//
// This is the CLIENT counterpart to the whole server auth path (authd #79 /
// worldd #84) and the login half of IT-M0. authd owns and drives the IF-1 message
// sequence (server/authd/login_session.cpp run_login); this core drives the OTHER
// side of that exact conversation, then initiates IF-2 to worldd with the grant:
//
//   IF-1 (to authd, TLS 1.3, u32-LE-length framing):
//     ClientHello{build,proto}      -> ServerHello / Error
//     SrpStart{account}             -> SrpChallenge{salt,B}
//     SrpProof{A,M1}                -> AuthResult{success,m2,error}
//     RealmListRequest              -> RealmList{realms[]}
//     RealmSelect{realm_id}         -> SessionGrant{grant_id,session_key,rc_ms}
//   IF-2 kickoff (to worldd):
//     build WorldHello{grant_id, client_build, nonce, proof} from the grant
//     (proof = HMAC-SHA256(session_key, client_build ‖ nonce) — world.fbs).
//
// The wire contract is schema/net/auth.fbs (IF-1) + schema/net/world.fbs (IF-2);
// this core encodes the client-side messages and decodes the server replies via
// meridian::proto (the same FlatBuffers codegen authd/worldd use). SRP-6a is the
// engine-free srp_client_core (mirrors meridian-srp's params: RFC 5054 2048-bit +
// SHA-256).
//
// ENGINE-FREE (Client SAD §9.2): NO Godot types. The transport is an injected
// seam (ILoginTransport) — a byte-frame read/write interface — so the state
// machine is unit-tested against a MOCK server that replays the exact IF-1
// sequence, AND wrapped over a real OpenSSL TLS 1.3 client (TlsLoginTransport,
// login_transport.*) for the live-authd integration test and the GDExtension. The
// thin Godot binding is meridian_login.* (#99).

#ifndef MERIDIAN_LOGIN_CORE_H
#define MERIDIAN_LOGIN_CORE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "srp_client_core.h"

namespace meridian::login {

// ---------------------------------------------------------------------------
// Transport seam — frame-level byte I/O over an already-established connection.
// ---------------------------------------------------------------------------
// The login core speaks in FRAMES (a FlatBuffer payload), not bytes: it never
// touches the u32-LE length prefix or the socket. An implementation owns the
// transport (TLS for IF-1) and does the length framing. This keeps the state
// machine engine-free AND socket-free, so a mock can drive it deterministically.
class ILoginTransport {
public:
    virtual ~ILoginTransport() = default;

    // Send one frame: the implementation prepends the u32-LE length prefix (IF-1
    // framing, max 8 KiB) and writes prefix+payload. Returns false on any write
    // error / oversize payload.
    virtual bool send_frame(const Bytes& payload) = 0;

    // Receive exactly one frame: read the u32-LE length, then that many payload
    // bytes; return the payload (prefix stripped). std::nullopt on a clean EOF or
    // any read/framing error (the caller treats it as a transport failure).
    virtual std::optional<Bytes> recv_frame() = 0;

    // Non-blocking-with-timeout receive. Distinguishes "no frame yet" (a read
    // timeout, `would_block` set true) from "peer closed / error" (`would_block`
    // false). This is what the bot's world-session drain loop uses to poll for
    // ASYNCHRONOUSLY relayed frames (EntityEnter/Update/Leave from the AoI relay,
    // #87) WITHOUT blocking forever when nothing is pending — the mover's own
    // MovementState and the OTHER session's entity frames arrive interleaved and
    // out of lockstep, so a strict one-recv-per-send blocking loop would miss them.
    //
    // Default: no timeout support — behaves exactly like recv_frame() (a frame or a
    // close; never `would_block`). Concrete socket transports override it to honour
    // the read timeout set via set_recv_timeout_ms(). The login mock inherits the
    // default (its queue is synchronous, so it never blocks).
    virtual std::optional<Bytes> recv_frame_nb(bool& would_block) {
        would_block = false;
        return recv_frame();
    }

    // Set the receive timeout (milliseconds) applied by recv_frame_nb(); 0 disables
    // it (blocking). Default no-op for transports without a real socket (the mock).
    virtual void set_recv_timeout_ms(unsigned /*ms*/) {}
};

// ---------------------------------------------------------------------------
// Result surface.
// ---------------------------------------------------------------------------

// Where the login stopped + why. Mirrors the server's LoginOutcome so a failure
// on either side maps cleanly. kSuccess means a SessionGrant was received and the
// server's M2 verified.
enum class LoginStatus {
    kSuccess,             // full flow completed; grant received, server M2 verified
    kConnectFailed,       // transport not usable (no ILoginTransport frames flow)
    kProtocolMismatch,    // ServerHello/Error rejected our proto/build
    kBadCredentials,      // AuthResult failure (wrong password / unknown user)
    kServerProofFailed,   // AuthResult success but M2 did NOT verify (abort — the
                          // server could not prove it holds the verifier)
    kRealmUnavailable,    // RealmSelect rejected (no such realm / build range)
    kProtocolError,       // an unexpected / undecodable message from the server
    kTransportClosed,     // peer closed mid-flow
};

// The IF-1 auth error code echoed from the server on a failure (auth.fbs
// AuthErrorCode). 0 == UNKNOWN / none. Surfaced so the UI can localize by code.
struct LoginResult {
    LoginStatus status = LoginStatus::kProtocolError;
    std::string detail;             // human-readable note (logs / fallback UX)
    std::uint16_t server_error_code = 0;  // auth.fbs AuthErrorCode (0 = none)

    // Populated on kSuccess (the deliverable the boot/login scene consumes):
    std::uint64_t grant_id = 0;         // SessionGrant.grant_id (IF-3 token)
    Bytes session_key;                  // SessionGrant.session_key (32 B; IF-2 HKDF root)
    std::uint32_t reconnect_window_ms = 0;  // ISSUE #66 server-owned window
    std::uint32_t selected_realm_id = 0;    // the realm we were granted for

    bool ok() const { return status == LoginStatus::kSuccess; }
};

// One realm advertised in the RealmList (auth.fbs RealmRow). Surfaced so the
// login scene can present a picker; the default flow auto-selects (see
// LoginConfig.realm_selector).
struct RealmInfo {
    std::uint32_t id = 0;
    std::string name;
    std::string address;
    std::uint16_t port = 0;
    std::uint16_t population = 0;
    std::uint32_t build_min = 0;
    std::uint32_t build_max = 0;
    std::uint32_t flags = 0;
};

// Client identity + policy for one login (mirrors the fields authd checks).
struct LoginConfig {
    std::uint32_t client_build = 1;   // ClientHello.build (realm build-range gate)
    std::uint16_t proto_ver = 1;      // ClientHello.proto_ver (must match authd)
    SrpParams srp_params{};           // {Rfc5054_2048, Sha256} — authd's default
};

// ---------------------------------------------------------------------------
// The login flow.
// ---------------------------------------------------------------------------

// Drive the full IF-1 login over `transport`, authenticating `account`/`password`
// and selecting a realm. `select_realm` chooses which realm to request a grant
// for, given the RealmList the server returned; it returns the chosen realm id.
// A realm id not present in the list is sent anyway (the server rejects it →
// kRealmUnavailable), which is what a caller wants if it insists on a specific
// realm. If `select_realm` is null, the FIRST realm this client's build is in
// range for is chosen; if none qualifies, the first realm is used (letting the
// server return the authoritative rejection).
//
// On success returns LoginResult{kSuccess, grant_id, session_key, ...}. On any
// protocol/auth/realm failure returns the mapped status + the server's error code
// (never throws for those — they are normal outcomes). `realms_out`, when non-null,
// is filled with the RealmList the server returned (for a UI picker), regardless
// of the final status once the list is reached.
LoginResult run_login(
    ILoginTransport& transport, const LoginConfig& cfg,
    const std::string& account, const std::string& password,
    std::uint32_t (*select_realm)(const std::vector<RealmInfo>&, const LoginConfig&) = nullptr,
    std::vector<RealmInfo>* realms_out = nullptr);

// ---------------------------------------------------------------------------
// IF-2 world-handshake kickoff.
// ---------------------------------------------------------------------------

// Build the WorldHello FlatBuffer payload (world.fbs) the client sends as the
// FIRST IF-2 frame to worldd, from a successful LoginResult. Generates a random
// 16-byte nonce and computes proof = HMAC-SHA256(session_key, client_build ‖
// nonce) per world.fbs. `out_nonce`, when non-null, receives the nonce used (the
// client keeps it to verify the server_proof in HandshakeOk once the server wires
// it — worldd #84 currently leaves server_proof empty at M0).
//
// NOTE (honest, matches worldd #84): at M0 worldd validates the grant and IGNORES
// the WorldHello.proof, and the IF-2 payload is NOT AEAD-wrapped on the wire yet
// (the seal() seam runs but writes plaintext — server/worldd/world_state.cpp). So
// the proof here is well-formed and future-proof but not yet checked server-side.
Bytes build_world_hello(const LoginResult& grant, std::uint32_t client_build,
                        Bytes* out_nonce = nullptr);

// Wrap an IF-2 payload in the world-frame header (u16 opcode LE ‖ u64 seq LE ‖
// payload), matching server/worldd/world_dispatch.cpp encode_frame. The WorldHello
// uses opcode 0x0001 (WORLD_HELLO) and seq 0 as the first frame.
Bytes encode_world_frame(std::uint16_t opcode, std::uint64_t seq,
                         const Bytes& payload);

// The WORLD_HELLO opcode value (world.fbs Opcode enum). Exposed so the wrapper /
// tests do not hard-code a magic number.
inline constexpr std::uint16_t kOpcodeWorldHello = 0x0001;

}  // namespace meridian::login

#endif  // MERIDIAN_LOGIN_CORE_H
