// SPDX-License-Identifier: Apache-2.0
//
// meridian client LOGIN core implementation (issue #99). Drives the client side
// of the IF-1 sequence authd owns (server/authd/login_session.cpp) and builds the
// IF-2 WorldHello (world.fbs) from the grant. Clean-room from the auth.fbs /
// world.fbs wire contracts + the server login state machine as the interop
// reference; no GPL source consulted (CONTRIBUTING.md).

#include "login_core.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstring>

#include "auth_generated.h"
#include "version_compat.h"
#include "world_generated.h"

namespace meridian::login {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;

// ---- FlatBuffer encode helpers (client-side messages) ----------------------
Bytes finish(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes enc_client_hello(std::uint32_t build, std::uint16_t proto) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateClientHello(b, build, proto));
    return finish(b);
}

Bytes enc_srp_start(const std::string& account) {
    fb::FlatBufferBuilder b;
    auto acc = b.CreateString(account);
    b.Finish(mn::CreateSrpStart(b, acc));
    return finish(b);
}

Bytes enc_srp_proof(const Bytes& A, const Bytes& M1) {
    fb::FlatBufferBuilder b;
    auto av = b.CreateVector(A);
    auto mv = b.CreateVector(M1);
    b.Finish(mn::CreateSrpProof(b, av, mv));
    return finish(b);
}

Bytes enc_realm_list_request() {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateRealmListRequest(b));
    return finish(b);
}

Bytes enc_realm_select(std::uint32_t realm_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateRealmSelect(b, realm_id));
    return finish(b);
}

// ---- FlatBuffer decode helpers ---------------------------------------------
// Verify then GetRoot; nullptr on a malformed/oversized buffer so the caller can
// treat it as a protocol error (never GetRoot without the Verifier on untrusted
// server bytes — the same discipline authd applies to client bytes).
template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

Bytes vec_to_bytes(const fb::Vector<std::uint8_t>* v) {
    if (v == nullptr) return {};
    return Bytes(v->data(), v->data() + v->size());
}

// Map the auth.fbs AuthErrorCode carried in an Error to a LoginStatus.
LoginStatus status_from_error(mn::AuthErrorCode code) {
    switch (code) {
        case mn::AuthErrorCode::PROTOCOL_MISMATCH:
        case mn::AuthErrorCode::BUILD_REJECTED:
            return LoginStatus::kProtocolMismatch;
        case mn::AuthErrorCode::BAD_CREDENTIALS:
        case mn::AuthErrorCode::ACCOUNT_LOCKED:
        case mn::AuthErrorCode::ACCOUNT_BANNED:
            return LoginStatus::kBadCredentials;
        case mn::AuthErrorCode::REALM_UNAVAILABLE:
        case mn::AuthErrorCode::REALM_FULL:
            return LoginStatus::kRealmUnavailable;
        default:
            return LoginStatus::kProtocolError;
    }
}

// Default realm chooser: first realm whose [build_min, build_max] admits this
// client build (build_max == 0 means "no ceiling", matching authd's check). Falls
// back to the first realm so the server returns the authoritative rejection.
std::uint32_t default_select_realm(const std::vector<RealmInfo>& realms,
                                   const LoginConfig& cfg) {
    if (realms.empty()) return 0;
    for (const RealmInfo& r : realms) {
        const bool above_floor = cfg.client_build >= r.build_min;
        const bool below_ceiling = (r.build_max == 0) || (cfg.client_build <= r.build_max);
        if (above_floor && below_ceiling) return r.id;
    }
    return realms.front().id;
}

// little-endian scalar writers for the IF-2 frame header (matches worldd's
// world_dispatch.cpp put_u16/put_u64).
void put_u16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void put_u64(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The login flow.
// ---------------------------------------------------------------------------

LoginResult run_login(
    ILoginTransport& transport, const LoginConfig& cfg,
    const std::string& account, const std::string& password,
    std::uint32_t (*select_realm)(const std::vector<RealmInfo>&, const LoginConfig&),
    std::vector<RealmInfo>* realms_out) {
    LoginResult result;

    auto transport_closed = [&](const char* where) {
        result.status = LoginStatus::kTransportClosed;
        result.detail = std::string("peer closed / read failed at ") + where;
        return result;
    };
    auto protocol_error = [&](const char* what) {
        result.status = LoginStatus::kProtocolError;
        result.detail = what;
        return result;
    };

    // ---- 1. ClientHello -> ServerHello | Error ------------------------------
    if (!transport.send_frame(enc_client_hello(cfg.client_build, cfg.proto_ver))) {
        result.status = LoginStatus::kConnectFailed;
        result.detail = "failed to send ClientHello";
        return result;
    }
    std::optional<Bytes> frame = transport.recv_frame();
    if (!frame) return transport_closed("ServerHello");
    if (const mn::ServerHello* sh = decode<mn::ServerHello>(*frame)) {
        // Schema/protocol version check (#98): compare our proto_ver against the
        // server-advertised one through the ONE shared compat rule (version_compat.*).
        // IF-1 carries no content hash (that is IF-2 HandshakeOk), so only proto_ver is
        // compared here; a blocking verdict is the "client out of date" case, surfaced
        // as kProtocolMismatch → the login scene's "client out of date — please update".
        const compat::CompatResult cr = compat::check_compat(
            compat::SchemaVersion{cfg.proto_ver, {}},
            compat::SchemaVersion{sh->proto_ver(), {}});
        if (cr.blocking) {
            result.status = LoginStatus::kProtocolMismatch;
            result.detail = cr.detail;
            return result;
        }
    } else if (const mn::Error* err = decode<mn::Error>(*frame)) {
        result.status = status_from_error(err->code());
        result.server_error_code = static_cast<std::uint16_t>(err->code());
        result.detail = err->message() ? err->message()->str() : "server rejected ClientHello";
        return result;
    } else {
        return protocol_error("undecodable ServerHello");
    }

    // ---- 2. SrpStart{account} -> SrpChallenge{salt,B} -----------------------
    if (!transport.send_frame(enc_srp_start(account)))
        return transport_closed("SrpStart send");
    frame = transport.recv_frame();
    if (!frame) return transport_closed("SrpChallenge");
    const mn::SrpChallenge* ch = decode<mn::SrpChallenge>(*frame);
    if (ch == nullptr || ch->salt() == nullptr || ch->b() == nullptr) {
        // authd sends an Error only on a malformed SrpStart (which we never send);
        // any non-challenge here is a protocol error.
        if (const mn::Error* err = decode<mn::Error>(*frame)) {
            result.status = status_from_error(err->code());
            result.server_error_code = static_cast<std::uint16_t>(err->code());
            result.detail = err->message() ? err->message()->str() : "SrpStart rejected";
            return result;
        }
        return protocol_error("undecodable SrpChallenge");
    }
    Bytes salt = vec_to_bytes(ch->salt());
    Bytes B = vec_to_bytes(ch->b());

    // ---- 3. SrpProof{A,M1} -> AuthResult{success,m2,error} ------------------
    // Compute the client proof against the challenge (real SRP-6a, random a).
    SrpClientSession srp(account, password, cfg.srp_params);
    Bytes M1;
    try {
        M1 = srp.compute_proof(salt, B);
    } catch (const std::exception& e) {
        // A zero B (or another safety-check failure) is a server that cannot be
        // trusted — abort rather than proceed.
        result.status = LoginStatus::kProtocolError;
        result.detail = std::string("SRP proof computation failed: ") + e.what();
        return result;
    }
    if (!transport.send_frame(enc_srp_proof(srp.public_a(), M1)))
        return transport_closed("SrpProof send");
    frame = transport.recv_frame();
    if (!frame) return transport_closed("AuthResult");
    const mn::AuthResult* ar = decode<mn::AuthResult>(*frame);
    if (ar == nullptr) return protocol_error("undecodable AuthResult");
    if (!ar->success()) {
        result.status = LoginStatus::kBadCredentials;
        if (ar->error() != nullptr) {
            result.server_error_code = static_cast<std::uint16_t>(ar->error()->code());
            result.detail = ar->error()->message()
                                ? ar->error()->message()->str()
                                : "authentication failed";
        } else {
            result.detail = "authentication failed";
        }
        return result;
    }
    // The server claims success — but we MUST verify its M2 proof before trusting
    // it (mutual authentication: the server proves it holds the verifier).
    Bytes m2 = vec_to_bytes(ar->m2());
    if (!srp.verify_server(m2)) {
        result.status = LoginStatus::kServerProofFailed;
        result.detail = "server M2 proof did not verify (server not authenticated)";
        return result;
    }

    // ---- 4. RealmListRequest -> RealmList -----------------------------------
    if (!transport.send_frame(enc_realm_list_request()))
        return transport_closed("RealmListRequest send");
    frame = transport.recv_frame();
    if (!frame) return transport_closed("RealmList");
    const mn::RealmList* rl = decode<mn::RealmList>(*frame);
    if (rl == nullptr) return protocol_error("undecodable RealmList");

    std::vector<RealmInfo> realms;
    if (rl->realms() != nullptr) {
        realms.reserve(rl->realms()->size());
        for (const mn::RealmRow* row : *rl->realms()) {
            RealmInfo ri;
            ri.id = row->id();
            ri.name = row->name() ? row->name()->str() : "";
            ri.address = row->address() ? row->address()->str() : "";
            ri.port = row->port();
            ri.population = row->population();
            ri.build_min = row->build_min();
            ri.build_max = row->build_max();
            ri.flags = row->flags();
            realms.push_back(std::move(ri));
        }
    }
    if (realms_out != nullptr) *realms_out = realms;

    if (realms.empty()) {
        result.status = LoginStatus::kRealmUnavailable;
        result.detail = "server returned an empty realm list";
        return result;
    }

    // ---- 5. RealmSelect{realm_id} -> SessionGrant | Error -------------------
    const std::uint32_t realm_id =
        (select_realm ? select_realm : default_select_realm)(realms, cfg);
    result.selected_realm_id = realm_id;

    if (!transport.send_frame(enc_realm_select(realm_id)))
        return transport_closed("RealmSelect send");
    frame = transport.recv_frame();
    if (!frame) return transport_closed("SessionGrant");
    if (const mn::SessionGrant* g = decode<mn::SessionGrant>(*frame)) {
        result.grant_id = g->grant_id();
        result.session_key = vec_to_bytes(g->session_key());
        result.reconnect_window_ms = g->reconnect_window_ms();
        result.status = LoginStatus::kSuccess;
        result.detail = "grant issued";
        return result;
    }
    if (const mn::Error* err = decode<mn::Error>(*frame)) {
        result.status = status_from_error(err->code());
        result.server_error_code = static_cast<std::uint16_t>(err->code());
        result.detail = err->message() ? err->message()->str() : "realm select rejected";
        return result;
    }
    return protocol_error("undecodable SessionGrant");
}

// ---------------------------------------------------------------------------
// IF-2 world-handshake kickoff.
// ---------------------------------------------------------------------------

Bytes build_world_hello(const LoginResult& grant, std::uint32_t client_build,
                        Bytes* out_nonce) {
    // 16-byte client nonce (world.fbs "client nonce (16 B); binds this handshake").
    Bytes nonce(16);
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        // Deterministic fallback so a handshake is still well-formed if the RNG
        // is unavailable; the proof still binds build ‖ nonce.
        for (std::size_t i = 0; i < nonce.size(); ++i) {
            nonce[i] = static_cast<std::uint8_t>(i * 37 + 11);
        }
    }
    if (out_nonce != nullptr) *out_nonce = nonce;

    // proof = HMAC-SHA256(session_key, client_build_LE(4) ‖ nonce) (world.fbs).
    Bytes msg;
    msg.reserve(4 + nonce.size());
    for (int i = 0; i < 4; ++i) {
        msg.push_back(static_cast<std::uint8_t>((client_build >> (8 * i)) & 0xFF));
    }
    msg.insert(msg.end(), nonce.begin(), nonce.end());

    Bytes proof(EVP_MAX_MD_SIZE);
    unsigned proof_len = 0;
    HMAC(EVP_sha256(), grant.session_key.data(),
         static_cast<int>(grant.session_key.size()), msg.data(), msg.size(),
         proof.data(), &proof_len);
    proof.resize(proof_len);

    fb::FlatBufferBuilder b;
    auto nonce_v = b.CreateVector(nonce);
    auto proof_v = b.CreateVector(proof);
    auto wh = mn::CreateWorldHello(b, grant.grant_id, client_build, nonce_v, proof_v);
    b.Finish(wh);
    return finish(b);
}

Bytes encode_world_frame(std::uint16_t opcode, std::uint64_t seq,
                         const Bytes& payload) {
    Bytes out;
    out.reserve(sizeof(std::uint16_t) + sizeof(std::uint64_t) + payload.size());
    put_u16(out, opcode);
    put_u64(out, seq);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace meridian::login
