// SPDX-License-Identifier: Apache-2.0
//
// Engine-free unit test for the client LOGIN core (issue #99). NO Godot, NO
// socket, NO live server: a MOCK ILoginTransport replays the EXACT IF-1 message
// sequence authd produces (server/authd/login_session.cpp), and the test runs a
// REAL server-side SRP-6a ServerSession (meridian-srp) against the core's client
// side — so the SRP interop (M1 verifies server-side, M2 verifies client-side) is
// proven without a database. This is the same shape as the other engine-free core
// tests (movement / telemetry / pack-manifest); a live-authd integration test
// (client/test/authd_login_it — real MariaDB + TLS) proves the wire interop on
// top of this.
//
// What it proves:
//   A. FULL LOGIN — the mock drives ServerHello -> SrpChallenge -> AuthResult(m2)
//      -> RealmList -> SessionGrant; run_login reaches kSuccess with the grant,
//      the 32-byte session_key, the reconnect window, and the selected realm; the
//      server verified our M1 and we verified its M2.
//   B. WRONG PASSWORD — the server-side ServerSession rejects a bad M1; the mock
//      replies AuthResult{error=BAD_CREDENTIALS}; run_login returns kBadCredentials
//      with no grant.
//   C. SERVER-PROOF FORGERY — a server that claims success but sends a bogus M2 is
//      rejected (kServerProofFailed) — mutual auth holds.
//   D. REALM SELECTION — the default chooser picks a build-compatible realm; an
//      explicit chooser is honored.
//   E. WORLD HELLO — build_world_hello produces a decodable WorldHello with the
//      grant id, a 16-byte nonce, and an HMAC-SHA256(session_key, build‖nonce)
//      proof; the IF-2 frame header round-trips.
//   F. SRP CLIENT CORE — client<->server key agreement (K matches) over the real
//      meridian-srp ServerSession, with a random client ephemeral.

#include "login_core.h"
#include "login_transport.h"  // compile-coverage only (not exercised here)
#include "srp_client_core.h"

#include "meridian/srp/srp.h"

#include "auth_generated.h"
#include "world_generated.h"

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;
using login::Bytes;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

srp::Bytes to_srp(const Bytes& b) { return srp::Bytes(b.begin(), b.end()); }
Bytes from_srp(const srp::Bytes& b) { return Bytes(b.begin(), b.end()); }

Bytes finish(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}
Bytes vec_to_bytes(const fb::Vector<std::uint8_t>* v) {
    return v ? Bytes(v->data(), v->data() + v->size()) : Bytes{};
}

// ---- A scripted mock server transport --------------------------------------
// Plays the SERVER side of IF-1 as a state machine over the frames run_login
// sends. It runs a REAL meridian-srp ServerSession so the SRP proofs are genuine.
// `password` is the account's true password (the ServerSession's verifier is
// derived from it); a login attempt with a different password produces an M1 the
// ServerSession rejects — exactly like authd.
class MockAuthdTransport final : public login::ILoginTransport {
public:
    struct Realm {
        std::uint32_t id;
        std::string name;
        std::uint32_t build_min;
        std::uint32_t build_max;
    };

    MockAuthdTransport(std::string account, std::string password,
                       std::vector<Realm> realms, std::uint16_t proto_ver = 1,
                       std::uint64_t grant_id = 0xDEADBEEFCAFEF00Dull,
                       bool forge_m2 = false)
        : account_(std::move(account)),
          verifier_(srp::make_verifier(account_, password, params_)),
          srp_(account_, verifier_.salt, verifier_.verifier, params_),
          realms_(std::move(realms)),
          proto_ver_(proto_ver),
          grant_id_(grant_id),
          forge_m2_(forge_m2) {
        session_key_.assign(32, 0);
        for (int i = 0; i < 32; ++i) session_key_[i] = static_cast<std::uint8_t>(0x40 + i);
    }

    bool send_frame(const Bytes& payload) override {
        // The CLIENT is sending us a frame; produce the server reply and queue it.
        switch (state_) {
            case St::kHello: {
                const mn::ClientHello* h = decode<mn::ClientHello>(payload);
                if (h == nullptr) return false;
                client_build_ = h->build();
                fb::FlatBufferBuilder b;
                b.Finish(mn::CreateServerHello(b, /*build=*/client_build_, proto_ver_));
                queue_.push_back(finish(b));
                state_ = St::kSrpStart;
                return true;
            }
            case St::kSrpStart: {
                const mn::SrpStart* s = decode<mn::SrpStart>(payload);
                if (s == nullptr || s->account() == nullptr) return false;
                fb::FlatBufferBuilder b;
                auto salt_v = b.CreateVector(verifier_.salt);
                auto b_v = b.CreateVector(srp_.B());
                b.Finish(mn::CreateSrpChallenge(b, salt_v, b_v));
                queue_.push_back(finish(b));
                state_ = St::kSrpProof;
                return true;
            }
            case St::kSrpProof: {
                const mn::SrpProof* p = decode<mn::SrpProof>(payload);
                if (p == nullptr || p->a() == nullptr || p->m1() == nullptr) return false;
                Bytes A = vec_to_bytes(p->a());
                Bytes M1 = vec_to_bytes(p->m1());
                std::optional<srp::Bytes> M2 = srp_.verify(to_srp(A), to_srp(M1));
                fb::FlatBufferBuilder b;
                if (M2.has_value()) {
                    Bytes m2 = from_srp(*M2);
                    if (forge_m2_) {  // C: tamper the server proof
                        for (auto& byte : m2) byte ^= 0xFF;
                    }
                    auto m2_v = b.CreateVector(m2);
                    b.Finish(mn::CreateAuthResult(b, /*success=*/true, m2_v, /*error=*/0));
                    state_ = St::kRealmList;
                } else {
                    auto msg = b.CreateString("authentication failed");
                    auto err = mn::CreateError(b, mn::AuthErrorCode::BAD_CREDENTIALS, msg);
                    b.Finish(mn::CreateAuthResult(b, /*success=*/false, /*m2=*/0, err));
                    state_ = St::kDone;  // wrong pw: authd stops the flow
                }
                queue_.push_back(finish(b));
                return true;
            }
            case St::kRealmList: {
                if (decode<mn::RealmListRequest>(payload) == nullptr) return false;
                fb::FlatBufferBuilder b;
                std::vector<fb::Offset<mn::RealmRow>> rows;
                for (const Realm& r : realms_) {
                    auto name = b.CreateString(r.name);
                    auto addr = b.CreateString("127.0.0.1");
                    rows.push_back(mn::CreateRealmRow(b, r.id, name, addr,
                                                      /*port=*/7200, /*pop=*/0,
                                                      r.build_min, r.build_max, /*flags=*/0));
                }
                auto vec = b.CreateVector(rows);
                b.Finish(mn::CreateRealmList(b, vec));
                queue_.push_back(finish(b));
                state_ = St::kRealmSelect;
                return true;
            }
            case St::kRealmSelect: {
                const mn::RealmSelect* rs = decode<mn::RealmSelect>(payload);
                if (rs == nullptr) return false;
                selected_realm_ = rs->realm_id();
                bool known = false;
                for (const Realm& r : realms_) if (r.id == selected_realm_) known = true;
                fb::FlatBufferBuilder b;
                if (known) {
                    auto key_v = b.CreateVector(session_key_);
                    b.Finish(mn::CreateSessionGrant(b, grant_id_, key_v,
                                                    /*reconnect_window_ms=*/30000));
                } else {
                    auto msg = b.CreateString("no such realm");
                    b.Finish(mn::CreateError(b, mn::AuthErrorCode::REALM_UNAVAILABLE, msg));
                }
                queue_.push_back(finish(b));
                state_ = St::kDone;
                return true;
            }
            case St::kDone:
                return false;  // unexpected extra client frame
        }
        return false;
    }

    std::optional<Bytes> recv_frame() override {
        if (queue_.empty()) return std::nullopt;
        Bytes f = std::move(queue_.front());
        queue_.pop_front();
        return f;
    }

    const Bytes& session_key() const { return session_key_; }
    std::uint32_t selected_realm() const { return selected_realm_; }

private:
    enum class St { kHello, kSrpStart, kSrpProof, kRealmList, kRealmSelect, kDone };
    srp::Parameters params_{};  // {Rfc5054_2048, Sha256}
    std::string account_;
    srp::Verifier verifier_;
    srp::ServerSession srp_;
    std::vector<Realm> realms_;
    std::uint16_t proto_ver_;
    std::uint64_t grant_id_;
    bool forge_m2_;
    Bytes session_key_;
    std::deque<Bytes> queue_;
    St state_ = St::kHello;
    std::uint32_t client_build_ = 0;
    std::uint32_t selected_realm_ = 0;
};

void test_full_login() {
    std::printf("A. FULL LOGIN (correct password)\n");
    login::LoginConfig cfg;
    cfg.client_build = 1000;
    cfg.proto_ver = 1;
    MockAuthdTransport t("alice", "correct horse battery staple",
                         {{7u, "Reference", 0u, 100000u}});
    std::vector<login::RealmInfo> realms;
    login::LoginResult r = login::run_login(t, cfg, "alice",
                                            "correct horse battery staple",
                                            nullptr, &realms);
    check("A: status == kSuccess", r.status == login::LoginStatus::kSuccess);
    check("A: grant_id received", r.grant_id == 0xDEADBEEFCAFEF00Dull);
    check("A: session_key is 32 bytes", r.session_key.size() == 32);
    check("A: session_key matches server key", r.session_key == t.session_key());
    check("A: reconnect window surfaced", r.reconnect_window_ms == 30000);
    check("A: selected realm is the seeded realm", r.selected_realm_id == 7u);
    check("A: realm list surfaced (1 realm)", realms.size() == 1);
    check("A: realm name present", !realms.empty() && realms[0].name == "Reference");
}

void test_wrong_password() {
    std::printf("B. WRONG PASSWORD -> rejected, no grant\n");
    login::LoginConfig cfg;
    cfg.client_build = 1000;
    MockAuthdTransport t("alice", "the real password", {{7u, "Reference", 0u, 0u}});
    login::LoginResult r =
        login::run_login(t, cfg, "alice", "WRONG password", nullptr, nullptr);
    check("B: status == kBadCredentials", r.status == login::LoginStatus::kBadCredentials);
    check("B: BAD_CREDENTIALS error code",
          r.server_error_code == static_cast<std::uint16_t>(mn::AuthErrorCode::BAD_CREDENTIALS));
    check("B: no grant issued", r.grant_id == 0);
    check("B: no session key", r.session_key.empty());
}

void test_forged_server_proof() {
    std::printf("C. FORGED SERVER M2 -> rejected (mutual auth)\n");
    login::LoginConfig cfg;
    cfg.client_build = 1000;
    MockAuthdTransport t("alice", "pw", {{7u, "Reference", 0u, 0u}},
                         /*proto_ver=*/1, /*grant_id=*/1, /*forge_m2=*/true);
    login::LoginResult r = login::run_login(t, cfg, "alice", "pw", nullptr, nullptr);
    check("C: status == kServerProofFailed",
          r.status == login::LoginStatus::kServerProofFailed);
    check("C: no grant on a server we could not authenticate", r.grant_id == 0);
}

void test_realm_selection() {
    std::printf("D. REALM SELECTION (build compat + explicit chooser)\n");
    login::LoginConfig cfg;
    cfg.client_build = 1500;
    // Realm 1 excludes this build (max 999); realm 2 admits it. Default chooser
    // must skip realm 1 and pick realm 2.
    {
        MockAuthdTransport t("alice", "pw",
                             {{1u, "Old", 0u, 999u}, {2u, "Current", 1000u, 2000u}});
        login::LoginResult r = login::run_login(t, cfg, "alice", "pw", nullptr, nullptr);
        check("D: default chooser skips build-incompatible realm",
              r.status == login::LoginStatus::kSuccess && r.selected_realm_id == 2u);
    }
    // Explicit chooser overrides — always pick realm 1's id.
    {
        MockAuthdTransport t("alice", "pw",
                             {{1u, "Old", 0u, 999u}, {2u, "Current", 1000u, 2000u}});
        auto pick_first = [](const std::vector<login::RealmInfo>& rs,
                             const login::LoginConfig&) -> std::uint32_t {
            return rs.front().id;
        };
        login::LoginResult r = login::run_login(t, cfg, "alice", "pw", pick_first, nullptr);
        check("D: explicit chooser is honored (picked realm 1)",
              r.selected_realm_id == 1u);
    }
}

void test_world_hello() {
    std::printf("E. IF-2 WORLD HELLO from the grant\n");
    login::LoginConfig cfg;
    cfg.client_build = 1234;
    MockAuthdTransport t("alice", "pw", {{7u, "Reference", 0u, 0u}});
    login::LoginResult r = login::run_login(t, cfg, "alice", "pw", nullptr, nullptr);
    check("E: precondition — login succeeded", r.status == login::LoginStatus::kSuccess);

    Bytes nonce;
    Bytes wh = login::build_world_hello(r, cfg.client_build, &nonce);
    const mn::WorldHello* h = decode<mn::WorldHello>(wh);
    check("E: WorldHello decodes", h != nullptr);
    if (h != nullptr) {
        check("E: grant_id carried", h->grant_id() == r.grant_id);
        check("E: client_build carried", h->client_build() == cfg.client_build);
        check("E: nonce is 16 bytes", h->nonce() && h->nonce()->size() == 16);
        check("E: proof is HMAC-SHA256 (32 bytes)", h->proof() && h->proof()->size() == 32);
    }
    check("E: out_nonce returned (16 bytes)", nonce.size() == 16);

    // IF-2 frame header round-trips: u16 opcode LE ‖ u64 seq LE ‖ payload.
    Bytes frame = login::encode_world_frame(login::kOpcodeWorldHello, 0, wh);
    check("E: frame >= header + payload", frame.size() == 10 + wh.size());
    std::uint16_t op = static_cast<std::uint16_t>(frame[0]) |
                       (static_cast<std::uint16_t>(frame[1]) << 8);
    check("E: frame opcode == WORLD_HELLO(0x0001)", op == 0x0001);
}

void test_srp_client_core() {
    std::printf("F. SRP CLIENT CORE key agreement (random ephemeral)\n");
    srp::Parameters params{};  // {Rfc5054_2048, Sha256}
    const std::string user = "bob";
    const std::string pw = "hunter2";
    srp::Verifier v = srp::make_verifier(user, pw, params);
    srp::ServerSession server(user, v.salt, v.verifier, params);

    login::SrpParams cparams;  // defaults == {Rfc5054_2048, Sha256}
    login::SrpClientSession client(user, pw, cparams);

    Bytes M1 = client.compute_proof(from_srp(v.salt), from_srp(server.B()));
    std::optional<srp::Bytes> M2 = server.verify(to_srp(client.public_a()), to_srp(M1));
    check("F: server verifies client M1", M2.has_value());
    if (M2.has_value()) {
        check("F: client verifies server M2", client.verify_server(from_srp(*M2)));
    }
    check("F: session key K matches (client == server)",
          from_srp(server.session_key()) == client.session_key());
    check("F: session key is 32 bytes (SHA-256)", client.session_key().size() == 32);

    // A wrong password on the client side must NOT agree with the stored verifier.
    login::SrpClientSession bad(user, "wrong", cparams);
    srp::ServerSession server2(user, v.salt, v.verifier, params);
    Bytes badM1 = bad.compute_proof(from_srp(v.salt), from_srp(server2.B()));
    check("F: wrong password fails server verify",
          !server2.verify(to_srp(bad.public_a()), to_srp(badM1)).has_value());
}

}  // namespace

int main() {
    std::printf("client login core test (IF-1/IF-2, #99)\n\n");
    test_full_login();
    test_wrong_password();
    test_forged_server_proof();
    test_realm_selection();
    test_world_hello();
    test_srp_client_core();
    std::printf(g_fail == 0 ? "\nALL CLIENT LOGIN CORE TESTS PASSED\n"
                            : "\n%d CLIENT LOGIN CORE TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
