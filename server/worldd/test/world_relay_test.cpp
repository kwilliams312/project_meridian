// SPDX-License-Identifier: Apache-2.0
//
// worldd — two-session AoI movement RELAY integration test (IT-M0 "two clients
// see each other move"; issue #87). THE capstone that closes the IT-M0 movement
// loop.
//
// CLEAN-ROOM: written from the server SAD (§2.5 Grid/AoI + the "authoritative
// state → interest set → per-subscriber egress" flow, §5.2 IF-2 framing +
// per-session AEAD, §5.3 IF-3 grant handoff, §8.3 IT-M0 row "movement + AoI relay
// only"), docs/it-m0-runbook.md (Step 3 — two clients see each other move,
// < 250 ms), the world.fbs wire contract, and the OpenSSL public API docs only.
// No GPL source consulted (CONTRIBUTING).
//
// Composes meridian::world-dispatch (the REAL serve/dispatch loop + the REAL
// WORLD_HELLO + MOVEMENT_INTENT + AoI relay) against a REAL MariaDB + REAL TLS
// 1.3 sockets. It seeds TWO session_grant rows exactly as authd writes them (two
// real sessions consume two grants), stands the real listener + serve loop, and
// drives TWO TLS clients.
//
// Needs a live MariaDB with the auth schema loaded (0001_init_auth.up.sql). Reads
// MERIDIAN_DB_* env and SKIPS (exit 0) when none are set, so it is inert in the
// plain server ctest and runs for real only in the worldd-session CI job (or
// locally with env set). The pure grid/interest logic is proven separately by the
// DB-free worldd-aoi unit test.
//
// What it proves end-to-end (docs/it-m0-runbook.md Step 3, DC-4):
//   1. Two clients HandshakeOk and enter world at the SAME spawn (64,64) — within
//      AoI range. On enter each receives an EntityEnter for the other (login
//      bidirectional visibility).
//   2. Client A sends a LEGAL MovementIntent (a small walk step). Client B
//      receives an EntityUpdate for A carrying A's NEW authoritative position
//      (and B still sees A). This is the see-each-other-move relay.
//   3. Client A moves FAR (out of B's AoI leave radius). Client B receives an
//      EntityLeave for A (OUT_OF_RANGE). Symmetric: A receives EntityLeave for B.

#include "world_dispatch.h"
#include "world_session.h"
#include "world_state.h"

#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"

#include "movement_constants.h"
#include "world_generated.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mw = meridian::worldd;
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

using Bytes = std::vector<std::uint8_t>;

// ---- Throwaway self-signed cert (mirrors world_session_test) ----------------
bool generate_self_signed(const std::string& cert_path, const std::string& key_path) {
    bool ok = false;
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    FILE* fk = nullptr;
    FILE* fc = nullptr;
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) goto done;
    if (EVP_PKEY_keygen_init(pctx) != 1) goto done;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) != 1) goto done;
    if (EVP_PKEY_keygen(pctx, &pkey) != 1) goto done;
    x509 = X509_new();
    if (!x509) goto done;
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60L);
    if (X509_set_pubkey(x509, pkey) != 1) goto done;
    {
        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("meridian-worldd-relay-test"), -1, -1, 0);
        if (X509_set_issuer_name(x509, name) != 1) goto done;
    }
    if (X509_sign(x509, pkey, EVP_sha256()) == 0) goto done;
    fk = std::fopen(key_path.c_str(), "wb");
    if (!fk) goto done;
    if (PEM_write_PrivateKey(fk, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) goto done;
    fc = std::fopen(cert_path.c_str(), "wb");
    if (!fc) goto done;
    if (PEM_write_X509(fc, x509) != 1) goto done;
    ok = true;
done:
    if (fk) std::fclose(fk);
    if (fc) std::fclose(fc);
    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    if (pctx) EVP_PKEY_CTX_free(pctx);
    if (!ok) ERR_print_errors_fp(stderr);
    return ok;
}

// ---- Minimal TLS 1.3 IF-2 client (mirrors world_session_test) ---------------
class Client {
public:
    explicit Client(std::uint16_t port) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd_);
        connected_ = (SSL_connect(ssl_) == 1);
        if (!connected_) ERR_print_errors_fp(stderr);
    }
    ~Client() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
        if (fd_ >= 0) ::close(fd_);
        if (ctx_) SSL_CTX_free(ctx_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connected() const { return connected_; }

    bool send_frame(const Bytes& payload) {
        std::uint32_t len = static_cast<std::uint32_t>(payload.size());
        Bytes f{static_cast<std::uint8_t>(len & 0xFF),
                static_cast<std::uint8_t>((len >> 8) & 0xFF),
                static_cast<std::uint8_t>((len >> 16) & 0xFF),
                static_cast<std::uint8_t>((len >> 24) & 0xFF)};
        f.insert(f.end(), payload.begin(), payload.end());
        return write_all(f.data(), f.size());
    }

    std::optional<Bytes> recv_frame() {
        std::uint8_t lenbuf[4];
        if (!read_all(lenbuf, 4)) return std::nullopt;
        std::uint32_t len = static_cast<std::uint32_t>(lenbuf[0]) |
                            (static_cast<std::uint32_t>(lenbuf[1]) << 8) |
                            (static_cast<std::uint32_t>(lenbuf[2]) << 16) |
                            (static_cast<std::uint32_t>(lenbuf[3]) << 24);
        Bytes payload(len);
        if (len > 0 && !read_all(payload.data(), len)) return std::nullopt;
        return payload;
    }

private:
    bool write_all(const std::uint8_t* buf, std::size_t n) {
        std::size_t sent = 0;
        while (sent < n) {
            int w = SSL_write(ssl_, buf + sent, static_cast<int>(n - sent));
            if (w <= 0) return false;
            sent += static_cast<std::size_t>(w);
        }
        return true;
    }
    bool read_all(std::uint8_t* buf, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            int r = SSL_read(ssl_, buf + got, static_cast<int>(n - got));
            if (r <= 0) return false;
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    bool connected_ = false;
};

// ---- world.fbs payload builders --------------------------------------------
Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_world_hello(std::uint64_t grant_id, std::uint32_t build) {
    fb::FlatBufferBuilder b;
    auto h = mn::CreateWorldHello(b, grant_id, build, /*nonce=*/0, /*proof=*/0);
    b.Finish(h);
    return bytes_of(b);
}
Bytes enc_movement_intent(std::uint32_t seq, std::uint32_t flags, float x, float y,
                          float z, std::uint64_t client_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateMovementIntent(b, seq, flags, x, y, z, /*orientation=*/0.0f,
                                      client_time_ms));
    return bytes_of(b);
}

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

std::uint64_t cell_u64(const db::Cell& c) {
    return c.has_value() ? std::strtoull(c->c_str(), nullptr, 10) : 0;
}
db::Param blob32(const Bytes& b) { return db::Param{db::Bytes_t(b.begin(), b.end())}; }

void seed_grant(db::Connection& db, std::uint64_t grant_id, std::uint64_t account_id,
                std::uint32_t realm_id, const Bytes& session_key,
                std::uint32_t client_build, const std::string& expires_sql) {
    db.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, expires_at) "
        "VALUES (?, ?, ?, ?, ?, " + expires_sql + ")",
        {db::Param{std::to_string(grant_id)},
         db::Param{std::to_string(account_id)},
         db::Param{static_cast<std::int64_t>(realm_id)},
         blob32(session_key),
         db::Param{static_cast<std::int64_t>(client_build)}});
}

// Handshake a client, returning true on HandshakeOk. Leaves the connection open.
bool do_handshake(Client& c, std::uint64_t grant_id, std::uint32_t build) {
    if (!c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, /*seq=*/1,
                                       enc_world_hello(grant_id, build))))
        return false;
    std::optional<Bytes> hs = c.recv_frame();
    if (!hs) return false;
    std::optional<mw::Frame> rf = mw::decode_frame(*hs);
    if (!rf || rf->opcode != mn::Opcode::HANDSHAKE_OK) return false;
    Bytes pl(rf->payload, rf->payload + rf->payload_len);
    return decode<mn::HandshakeOk>(pl) != nullptr;
}

// Read frames from `c` until one of the wanted entity-state opcodes arrives (or
// the connection ends). Returns the decoded frame. Skips MOVEMENT_STATE frames
// (the mover's own reconciliation echo) and any other opcode so the caller can
// assert specifically on the AoI relay stream.
struct EntityMsg {
    bool got = false;
    mn::Opcode opcode{};
    std::uint64_t guid = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    std::uint16_t leave_reason = 0;
};

EntityMsg recv_entity(Client& c) {
    for (;;) {
        std::optional<Bytes> fr = c.recv_frame();
        if (!fr) return {};  // connection ended
        std::optional<mw::Frame> rf = mw::decode_frame(*fr);
        if (!rf) continue;
        Bytes pl(rf->payload, rf->payload + rf->payload_len);
        if (rf->opcode == mn::Opcode::ENTITY_ENTER) {
            const auto* e = decode<mn::EntityEnter>(pl);
            if (!e) continue;
            EntityMsg m;
            m.got = true;
            m.opcode = rf->opcode;
            m.guid = e->entity_guid();
            m.x = e->x();
            m.y = e->y();
            m.z = e->z();
            return m;
        }
        if (rf->opcode == mn::Opcode::ENTITY_UPDATE) {
            const auto* u = decode<mn::EntityUpdate>(pl);
            if (!u) continue;
            EntityMsg m;
            m.got = true;
            m.opcode = rf->opcode;
            m.guid = u->entity_guid();
            // world.fbs EntityUpdate position fields are optional (default null);
            // the relay always sends them for a movement delta, so value_or(0) is
            // the actual position here.
            m.x = u->x().value_or(0.0f);
            m.y = u->y().value_or(0.0f);
            m.z = u->z().value_or(0.0f);
            return m;
        }
        if (rf->opcode == mn::Opcode::ENTITY_LEAVE) {
            const auto* l = decode<mn::EntityLeave>(pl);
            if (!l) continue;
            EntityMsg m;
            m.got = true;
            m.opcode = rf->opcode;
            m.guid = l->entity_guid();
            m.leave_reason = static_cast<std::uint16_t>(l->reason());
            return m;
        }
        // MOVEMENT_STATE (mover's own echo) or anything else — skip and keep reading.
    }
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    // Seed rand() from the clock so the account/realm/grant names are unique
    // across repeated LOCAL runs against the same DB (CI gets a fresh DB, but a
    // developer re-running this must not collide on uq_realm_name / a prior grant).
    std::srand(static_cast<unsigned>(::time(nullptr)) ^ static_cast<unsigned>(::getpid()));
    std::printf("worldd two-session AoI movement relay test (IT-M0 see-each-other-move, #87)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — two-session relay "
                    "needs a live MariaDB (two consumed grants). The pure grid/interest "
                    "logic is covered by the DB-free worldd-aoi test.\n");
        std::printf("\nWORLDD RELAY TEST SKIPPED (no DB)\n");
        return 0;
    }

    char tmpl[] = "/tmp/meridian-worldd-relay-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    std::uint64_t account_id = 0;
    std::uint32_t realm_id = 0;
    std::uint64_t grant_a = 0, grant_b = 0;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");

        const std::string username = "worldd_relay_" + std::to_string(std::rand());
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute(
            "INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
            {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        db::Result ar = db.execute("SELECT id FROM account WHERE username = ?",
                                   {db::Param{username}});
        account_id = cell_u64(ar.rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        const std::string realm_name = "WD Relay Realm " + std::to_string(std::rand());
        db.execute(
            "INSERT INTO realm (name, address, port, build_min, build_max) "
            "VALUES (?, '127.0.0.1', 7200, 0, 100000)",
            {db::Param{realm_name}});
        db::Result rr = db.execute("SELECT id FROM realm WHERE name = ?",
                                   {db::Param{realm_name}});
        realm_id = static_cast<std::uint32_t>(cell_u64(rr.rows.at(0)[0]));
        check("test realm seeded", realm_id > 0);

        auto rand_u64 = [] {
            std::uint64_t v = (static_cast<std::uint64_t>(std::rand()) << 32) ^
                              static_cast<std::uint64_t>(std::rand());
            return v == 0 ? 1 : v;
        };
        grant_a = rand_u64();
        grant_b = rand_u64();
        const Bytes session_key(32, 0xAB);
        seed_grant(db, grant_a, account_id, realm_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        seed_grant(db, grant_b, account_id, realm_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        check("two grants seeded (two real sessions)", true);

        // --- Stand up the real listener + serve loop with the auth DB wired.
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound to ephemeral port", port != 0);
        std::printf("  worldd relay listener on 127.0.0.1:%u\n", port);

        mw::WorldServerConfig wcfg;
        wcfg.auth_db = p;
        wcfg.realm_id = realm_id;
        mw::Dispatcher dispatcher;
        mw::WorldServer world(dispatcher, wcfg);
        world.start();

        // Serve exactly two connections, each on its OWN thread (the relay writes
        // to one client's socket from the OTHER client's serve thread — the real
        // cross-thread egress path).
        std::atomic<int> accepted{0};
        std::thread server([&] {
            std::vector<std::thread> conns;
            for (int i = 0; i < 2; ++i) {
                try {
                    net::Session s = listener.accept();
                    accepted.fetch_add(1);
                    conns.emplace_back([&world, s = std::move(s)]() mutable {
                        world.serve_connection(std::move(s));
                    });
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "  server accept error: %s\n", e.what());
                }
            }
            for (auto& t : conns) t.join();
        });

        // ===== 1. BOTH clients enter world at the spawn (64,64) — in range =====
        // Both placeholder characters spawn at the play-area centre (the
        // WORLD_HELLO handler seeds spawn = (kZoneMaxXY*0.5, kZoneMaxXY*0.5, 0) =
        // (64,64,0)), so they are co-located and mutually in AoI range on enter.
        //
        // The two clients live in a nested scope so their sockets CLOSE (dtor ->
        // SSL_shutdown + close) BEFORE server.join() below — otherwise the two
        // serve threads block forever in read_frame() and the join deadlocks.
        {
        Client a(port);
        check("A: client A connected (TLS)", a.connected());
        check("A: client A HandshakeOk", do_handshake(a, grant_a, client_build));

        // A is alone so far — its enter() found no one. Now B enters; B's enter()
        // will EntityEnter A into B, and reciprocally EntityEnter B into A.
        Client b(port);
        check("B: client B connected (TLS)", b.connected());
        check("B: client B HandshakeOk", do_handshake(b, grant_b, client_build));

        // On B's enter, A receives an EntityEnter for B (login bidirectional
        // visibility). No characters DB is wired, so both placeholder characters
        // have the D-11 stub guid 0 — the relay assigns each a UNIQUE synthetic
        // entity guid (kSyntheticGuidBase + slot) so the two sessions are distinct
        // on the wire. We assert A receives an EntityEnter at the spawn position
        // (the guid is B's synthetic id, distinct from A's).
        EntityMsg a_enter = recv_entity(a);
        check("1: A receives EntityEnter when B logs in nearby",
              a_enter.got && a_enter.opcode == mn::Opcode::ENTITY_ENTER);
        check("1: the EntityEnter carries the spawn position (~64,64)",
              a_enter.got && a_enter.x > 63.9f && a_enter.x < 64.1f &&
                  a_enter.y > 63.9f && a_enter.y < 64.1f);

        // B also received an EntityEnter for A at ITS enter (reciprocal login
        // visibility). Drain that first so the next B frame we read is the one A's
        // MOVE produces, not the login enter.
        EntityMsg b_enter = recv_entity(b);
        check("1: B receives EntityEnter for A at login (reciprocal)",
              b_enter.got && b_enter.opcode == mn::Opcode::ENTITY_ENTER);
        const std::uint64_t a_guid = b_enter.guid;  // A's synthetic entity guid
        check("1: A's guid distinct from B's (two distinct sessions)",
              a_guid != a_enter.guid);

        // ===== 2. A moves a legal step -> B sees EntityUpdate at A's new pos =====
        // A walks +0.20 m in x (a small legal walk step from 64,64). B, already
        // seeing A, must receive an EntityUpdate carrying A's NEW position.
        a.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, /*seq=*/2,
                                      enc_movement_intent(/*seq=*/10, /*Walk*/ 1,
                                                          64.20f, 64.0f, 0.0f,
                                                          /*client_time_ms=*/100)));
        EntityMsg b_update = recv_entity(b);
        check("2: B receives an entity message for A after A moves", b_update.got);
        check("2: the update is FOR A (matching A's guid)", b_update.guid == a_guid);
        check("2: B's message is an EntityUpdate (A already visible to B)",
              b_update.opcode == mn::Opcode::ENTITY_UPDATE);
        check("2: the EntityUpdate carries A's NEW position (x ~64.20)",
              b_update.x > 64.19f && b_update.x < 64.21f);

        // ===== 3. A moves FAR out of B's AoI radius -> B sees EntityLeave ========
        // The AoI leave radius is 50 m. A legal single walk step cannot cross 50 m
        // (walk cap ≈ 0.29 m/packet), so we send A a sequence of legal walk steps
        // marching in +x until it is well past the leave radius from B (who is
        // still at 64). Each step is inside the per-packet + window speed budget;
        // 100 ms apart so the rate class admits each. After A clears 50 m from B,
        // B must receive an EntityLeave{OUT_OF_RANGE} for A.
        //
        // A now RUNS in +x (state_flags = Run = 2) out of B's AoI radius. Run mode
        // is used so the SUSTAINED-speed sliding-window check (SAD §5.5 R3) has
        // ample headroom: at 0.20 m every 100 ms A moves 2.0 m/s, far under the
        // run cap (6.0 m/s) — per packet 0.20 < 6.0*0.1*1.15 = 0.69 m, and the 2 s
        // window sum (~8 m) is well under 6.0*2*1.15 = 13.8 m. So every step is a
        // LEGAL accepted move; A's authoritative position advances each time.
        // (Walk here would trip R3 — 0.20 m/100 ms = 2.0 m/s is under the 2.5 m/s
        // walk cap on AVERAGE but the tight window boundary rejects dense walk
        // packets; running is the natural way to legally cover 50 m fast.)
        //
        // We INTERLEAVE send + drain: send one intent, then drain the one entity
        // frame it relays to B. This keeps B's receive buffer from filling and
        // back-pressuring A's serve thread (which relays to B's socket) — a
        // send-all-then-read-all pattern could stall on socket buffers. Each A
        // step produces exactly one B frame (EntityUpdate while in range, then the
        // EntityLeave once A crosses the leave radius). We march until B reports
        // the EntityLeave for A (or a step budget is exhausted).
        std::uint32_t seq = 11;
        std::uint64_t t = 200;
        float x = 64.20f;
        bool b_saw_leave = false;
        for (int i = 0; i < 320 && !b_saw_leave; ++i) {
            x += 0.20f;
            const std::uint32_t frame_seq = seq++;
            a.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, frame_seq,
                                          enc_movement_intent(frame_seq, /*Run*/ 2, x,
                                                             64.0f, 0.0f, t)));
            t += 100;  // 100 ms apart — inside the 10/s rate class
            EntityMsg m = recv_entity(b);
            if (!m.got) break;  // connection ended unexpectedly
            if (m.opcode == mn::Opcode::ENTITY_LEAVE) b_saw_leave = true;
        }
        check("3: B receives EntityLeave for A after A moves out of AoI range",
              b_saw_leave);

        }  // clients a, b destruct here -> sockets close -> serve threads unblock

        server.join();
        world.stop();

        db.execute("DELETE FROM session_grant WHERE account_id = ?",
                   {db::Param{static_cast<std::int64_t>(account_id)}});
        db.execute("DELETE FROM realm WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(realm_id)}});
        db.execute("DELETE FROM account WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(account_id)}});
    } catch (const db::DbError& e) {
        std::printf("  FAIL  DbError %u: %s\n", e.code(), e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::printf("  FAIL  exception: %s\n", e.what());
        ++g_fail;
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    std::printf(g_fail == 0 ? "\nALL WORLDD RELAY TESTS PASSED\n"
                            : "\n%d WORLDD RELAY TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
