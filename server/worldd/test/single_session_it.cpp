// SPDX-License-Identifier: Apache-2.0
//
// worldd — single active session per account END-TO-END integration test (#326).
//
// CLEAN-ROOM: written from issue #326 + the server SAD (§5.3 IF-3 grant handoff,
// §5.2 IF-2 framing) + world.fbs + the OpenSSL public API only. No GPL source
// consulted (CONTRIBUTING.md). The TLS/cert/Client/grant-seed scaffolding mirrors
// world_session_test / world_relay_test (same clean-room helpers).
//
// Composes meridian::world-dispatch (the REAL serve loop + REAL WORLD_HELLO
// handler with the single-session admit) against a REAL MariaDB + REAL TLS 1.3.
// Proves the #326 acceptance AT THE WIRE, not just at the registry unit level:
//   two concurrent logins for ONE account cannot both stay in-world — the 2nd
//   login KICKS the 1st (kick-old), the 1st receives Disconnect{KICKED}, and the
//   server's active-session registry + AoI world both hold exactly ONE session.
//
// Needs a live MariaDB with the auth schema loaded (0001_init_auth.up.sql). Reads
// MERIDIAN_DB_* env and SKIPS (exit 0) when none are set — inert in the plain
// server ctest, runs for real in the worldd-session CI job (or locally with env).
// The DB-free registry logic is proven separately by worldd-active-session-test.

#include "world_dispatch.h"
#include "world_session.h"
#include "world_state.h"

#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"

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
#include <chrono>
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
            reinterpret_cast<const unsigned char*>("meridian-worldd-single-session-test"),
            -1, -1, 0);
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

// ---- world.fbs payload builders + decode -----------------------------------
Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_world_hello(std::uint64_t grant_id, std::uint32_t build) {
    fb::FlatBufferBuilder b;
    auto h = mn::CreateWorldHello(b, grant_id, build, /*nonce=*/0, /*proof=*/0);
    b.Finish(h);
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

// Handshake a client (WorldHello -> HandshakeOk). Leaves the connection open at
// character-select (server-authoritative characters, D-35: NOT yet spawned).
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

Bytes enc_enter_world(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldRequest(b, character_id));
    return bytes_of(b);
}

// Enter the world as an OWNED character (ENTER_WORLD_REQUEST -> RESPONSE(OK)).
// This is where a session actually spawns + takes the account's single in-world
// slot (#326) — the admit/kick moved off the handshake path (D-35). Returns true
// only on a typed OK.
bool do_enter_world(Client& c, std::uint64_t character_id) {
    if (!c.send_frame(mw::encode_frame(mn::Opcode::ENTER_WORLD_REQUEST, /*seq=*/2,
                                       enc_enter_world(character_id))))
        return false;
    std::optional<Bytes> r = c.recv_frame();
    if (!r) return false;
    std::optional<mw::Frame> rf = mw::decode_frame(*r);
    if (!rf || rf->opcode != mn::Opcode::ENTER_WORLD_RESPONSE) return false;
    Bytes pl(rf->payload, rf->payload + rf->payload_len);
    const auto* resp = decode<mn::EnterWorldResponse>(pl);
    return resp != nullptr && resp->status() == mn::EnterWorldStatus::OK;
}

// Standalone `character` table DDL (mirrors 0001_init_characters.up.sql). Lets the
// test seed an owned character in whatever DB char_db points at (here the auth DB,
// which doubles as the characters DB for the test). No-op when the real migration
// already created the table.
constexpr const char* kCharacterDdl =
    "CREATE TABLE IF NOT EXISTS `character` ("
    "  id            BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,"
    "  account_id    BIGINT UNSIGNED   NOT NULL,"
    "  name          VARCHAR(32)       NOT NULL,"
    "  race          TINYINT UNSIGNED  NOT NULL,"
    "  class         TINYINT UNSIGNED  NOT NULL,"
    "  level         SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
    "  xp            INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  money         BIGINT UNSIGNED   NOT NULL DEFAULT 0,"
    "  map_id        INT UNSIGNED      NOT NULL,"
    "  instance_id   INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  pos_x         FLOAT             NOT NULL,"
    "  pos_y         FLOAT             NOT NULL,"
    "  pos_z         FLOAT             NOT NULL,"
    "  pos_o         FLOAT             NOT NULL DEFAULT 0,"
    "  played_time   INT UNSIGNED      NOT NULL DEFAULT 0,"
    "  logout_at     DATETIME          NULL,"
    "  save_epoch    BIGINT            NOT NULL DEFAULT 0,"
    "  created_at    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at    DATETIME          NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_character_name (name),"
    "  KEY idx_character_account (account_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

// Seed one owned character via raw SQL (avoids a characters-lib link dep) and
// return the minted character.id. map/pos default to 0 — worldd re-seeds the
// spawn at zone centre on ENTER_WORLD.
std::uint64_t seed_character(db::Connection& db, std::uint64_t account_id,
                             const std::string& name) {
    db.execute("DELETE FROM `character` WHERE name = ?", {db::Param{name}});
    db.execute(
        "INSERT INTO `character` "
        "(account_id, name, race, class, map_id, pos_x, pos_y, pos_z) "
        "VALUES (?, ?, 1, 1, 0, 0, 0, 0)",
        {db::Param{std::to_string(account_id)}, db::Param{name}});
    db::Result r = db.execute("SELECT id FROM `character` WHERE name = ?",
                              {db::Param{name}});
    return cell_u64(r.rows.at(0)[0]);
}

// Poll `pred` up to `budget_ms` (10 ms steps) — condition-based waiting, no
// arbitrary sleeps. Used to synchronize on the server-side admit/kick completing
// (they run on the serve threads, slightly after the client sees HandshakeOk).
template <typename Pred>
bool wait_until(Pred pred, int budget_ms) {
    for (int waited = 0; waited < budget_ms; waited += 10) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    // Seed the RNG so account/realm/grant names are unique across runs against a
    // persistent DB (std::rand() is otherwise deterministic and would collide with
    // a prior run's uq_realm_name / grant rows).
    std::srand(static_cast<unsigned>(std::time(nullptr) ^ ::getpid()));
    std::printf("worldd single active session per account END-TO-END test (#326)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — single-session "
                    "end-to-end checks skipped (the DB-free registry logic is proven "
                    "by worldd-active-session-test)\n");
        return 0;
    }

    char tmpl[] = "/tmp/meridian-worldd-ss-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");

        // --- Seed an account + realm (session_grant FKs both). ---------------
        const std::string username = "worldd_ss_" + std::to_string(std::rand());
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute(
            "INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
            {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        db::Result ar = db.execute("SELECT id FROM account WHERE username = ?",
                                   {db::Param{username}});
        std::uint64_t account_id = cell_u64(ar.rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        // Seed ONE owned character (one-per-account, #329). Both logins enter the
        // world as THIS character — server-authoritative entry requires a real,
        // owned character (D-35); there is no placeholder fabrication anymore.
        db.execute(kCharacterDdl);
        const std::string char_name = "SsChar_" + std::to_string(std::rand());
        const std::uint64_t char_id = seed_character(db, account_id, char_name);
        check("test character seeded", char_id > 0);

        const std::string realm_name = "SS IT Realm " + std::to_string(std::rand());
        db.execute(
            "INSERT INTO realm (name, address, port, build_min, build_max) "
            "VALUES (?, '127.0.0.1', 7300, 0, 100000)",
            {db::Param{realm_name}});
        db::Result rr = db.execute("SELECT id FROM realm WHERE name = ?",
                                   {db::Param{realm_name}});
        std::uint32_t realm_id = static_cast<std::uint32_t>(cell_u64(rr.rows.at(0)[0]));
        check("test realm seeded", realm_id > 0);

        auto rand_u64 = [] {
            std::uint64_t v = (static_cast<std::uint64_t>(std::rand()) << 32) ^
                              static_cast<std::uint64_t>(std::rand());
            return v == 0 ? 1 : v;
        };
        // TWO valid grants for the SAME account — exactly the "authd issued a
        // second grant while a session is live" case #326 must resolve.
        const std::uint64_t grant_a = rand_u64();
        const std::uint64_t grant_b = rand_u64();
        const Bytes session_key(32, 0xAB);
        seed_grant(db, grant_a, account_id, realm_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        seed_grant(db, grant_b, account_id, realm_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        check("two valid grants for one account seeded", true);

        // --- Stand up the real listener + serve loop, thread-per-connection so
        //     TWO sessions can be live at once (the kick writes to A's socket
        //     from B's serve thread). --------------------------------------
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound to ephemeral port", port != 0);

        mw::WorldServerConfig wcfg;
        wcfg.auth_db = p;
        wcfg.char_db = p;          // characters DB (same instance) — ENTER_WORLD ownership load
        wcfg.realm_id = realm_id;
        mw::Dispatcher dispatcher;
        mw::WorldServer world(dispatcher, wcfg);
        world.start();

        std::thread server([&] {
            std::vector<std::thread> conns;
            for (int i = 0; i < 2; ++i) {
                try {
                    net::Session s = listener.accept();
                    conns.emplace_back([&world, s = std::move(s)]() mutable {
                        world.serve_connection(std::move(s));
                    });
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "  server accept error: %s\n", e.what());
                }
            }
            for (auto& t : conns) t.join();
        });

        {
            // ===== A logs in first and enters the world. ====================
            Client a(port);
            check("A: connected (TLS)", a.connected());
            check("A: HandshakeOk on first login", do_handshake(a, grant_a, client_build));
            check("A: entered world as its owned character", do_enter_world(a, char_id));
            // Wait for A's server-side admit to complete (it runs just after the
            // ENTER_WORLD OK write). A is now the account's single live session.
            check("A: account registered as the single live session",
                  wait_until([&] { return world.active_sessions().is_active(account_id); }, 2000));
            check("A: exactly one active account",
                  world.active_sessions().active_count() == 1);
            check("A: exactly one session in the AoI world",
                  wait_until([&] { return world.world_state().session_count() == 1; }, 2000));

            // ===== B logs in for the SAME account -> kicks A (kick-old). =====
            Client b(port);
            check("B: connected (TLS)", b.connected());
            check("B: HandshakeOk on second login",
                  do_handshake(b, grant_b, client_build));
            // B enters as the SAME owned character -> takes the account's single
            // in-world slot and KICKS A (kick-old, #326 at the ENTER_WORLD seam).
            check("B: entered world (admitted, kicks A)", do_enter_world(b, char_id));

            // A must now receive a Disconnect{KICKED} (the KickFn wrote it to A's
            // socket from B's serve thread). B's AoI enter first relays A an
            // EntityEnter for B (both spawn co-located at 64,64), so read frames
            // until the Disconnect arrives — the kick is guaranteed once B's admit
            // runs (right after B's HandshakeOk). recv_frame blocks between frames.
            bool a_kicked = false;
            bool a_got_frame = false;
            for (int i = 0; i < 16; ++i) {
                std::optional<Bytes> kf = a.recv_frame();
                if (!kf) break;  // connection ended without a Disconnect
                a_got_frame = true;
                std::optional<mw::Frame> rf = mw::decode_frame(*kf);
                if (!rf) continue;
                if (rf->opcode != mn::Opcode::DISCONNECT) continue;  // e.g. EntityEnter
                Bytes pl(rf->payload, rf->payload + rf->payload_len);
                const auto* d = decode<mn::Disconnect>(pl);
                a_kicked = d != nullptr && d->reason() == mn::DisconnectReason::KICKED;
                break;
            }
            check("A: received a server frame after B's login", a_got_frame);
            check("A: was kicked with Disconnect{KICKED}", a_kicked);

            // The account STILL has exactly one live session — B, not A. Two
            // concurrent logins never both stay in-world (#326 acceptance).
            check("account still has exactly ONE active session (B took over)",
                  world.active_sessions().active_count() == 1 &&
                      world.active_sessions().is_active(account_id));
            check("exactly one session remains in the AoI world (A was evicted)",
                  wait_until([&] { return world.world_state().session_count() == 1; }, 2000));

            // Clients close here (scope exit) -> both serve threads unwind.
        }

        server.join();
        world.stop();

        // After both sessions tear down, the account frees its registry slot
        // (compare-and-remove: B's clean release; A's kicked-old release no-oped).
        check("account slot freed after both sessions leave",
              wait_until([&] { return world.active_sessions().active_count() == 0; }, 2000));

        db.execute("DELETE FROM `character` WHERE id = ?",
                   {db::Param{std::to_string(char_id)}});
        db.execute("DELETE FROM account WHERE id = ?",
                   {db::Param{std::to_string(account_id)}});
    } catch (const db::DbError& e) {
        std::fprintf(stderr, "DB error: %s\n", e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        ++g_fail;
    }

    std::printf(g_fail == 0 ? "\nALL WORLDD SINGLE-SESSION E2E TESTS PASSED\n"
                            : "\n%d WORLDD SINGLE-SESSION E2E TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
