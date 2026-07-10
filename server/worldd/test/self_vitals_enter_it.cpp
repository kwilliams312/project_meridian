// SPDX-License-Identifier: Apache-2.0
//
// worldd — self-vitals-at-ENTER_WORLD live-session integration test (#439, UI-01
// HUD contract; part of the client HUD epic #24). Follow-up to #431/#438.
//
// CLEAN-ROOM: written from issue #439 + the server SAD (§2.5 authoritative Unit
// owns health/power/level; §5.2 IF-2 framing; §5.3 IF-3 grant handoff), world.fbs
// (VitalsUpdate / EnterWorldResponse), and the OpenSSL public API only. No GPL
// source consulted (CONTRIBUTING.md). The TLS/cert/Client/grant-seed scaffolding
// mirrors single_session_it / world_relay_test (same clean-room helpers).
//
// WHAT THIS PROVES AT THE WIRE: a session that logs in and ENTER_WORLDs receives
// an authoritative VITALS_UPDATE for its OWN character BEFORE it sends any combat
// or takes any damage — so the client HUD player frame can populate health/max,
// power/max + type, and level the instant the player is in-world (no blank own
// bars until the first delta, the #438 gap this closes). The self VITALS_UPDATE is
// pushed by the ENTER_WORLD handler via WorldState::broadcast_vitals, whose
// recipient #1 is always the subject's own client.
//
// Composes meridian::world-dispatch (the REAL serve loop + REAL WORLD_HELLO +
// ENTER_WORLD handlers + AoI relay) against a REAL MariaDB + REAL TLS 1.3.
//
// Needs a live MariaDB with the auth schema loaded (0001_init_auth.up.sql). Reads
// MERIDIAN_DB_* env and SKIPS (exit 0) when none are set — inert in the plain
// server ctest, runs for real in the worldd-session CI job (or locally with env).
// The DB-free vitals LOGIC (broadcast reaches self + observers) is proven by
// worldd-vitals-test; this proves the ENTER_WORLD wiring end-to-end.

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

// ---- Throwaway self-signed cert (mirrors single_session_it) -----------------
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
            reinterpret_cast<const unsigned char*>("meridian-worldd-self-vitals-test"),
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

// ---- Minimal TLS 1.3 IF-2 client (mirrors single_session_it) ----------------
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

// ---- world.fbs payload builders + decode ------------------------------------
Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_world_hello(std::uint64_t grant_id, std::uint32_t build) {
    fb::FlatBufferBuilder b;
    auto h = mn::CreateWorldHello(b, grant_id, build, /*nonce=*/0, /*proof=*/0);
    b.Finish(h);
    return bytes_of(b);
}
Bytes enc_enter_world(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldRequest(b, character_id));
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

// Send ENTER_WORLD_REQUEST and read exactly the ENTER_WORLD_RESPONSE (sent BEFORE
// any relayed frame). Returns true on a typed OK, leaving the self-vitals /
// AoI stream intact for recv_vitals().
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

// A decoded VitalsUpdate captured off the wire.
struct Vitals {
    bool got = false;
    std::uint64_t guid = 0;
    std::uint32_t health = 0, max_health = 0, power = 0, max_power = 0;
    mn::PowerType power_type = mn::PowerType::NONE;
    std::uint16_t level = 0;
};

// Read frames until a VITALS_UPDATE arrives (or the connection ends). Skips any
// other opcode (a co-located observer's EntityEnter, etc.) so the caller asserts
// on the self vitals snapshot specifically. No combat is ever sent, so a
// VITALS_UPDATE here can ONLY be the ENTER_WORLD self snapshot (#439).
Vitals recv_vitals(Client& c) {
    for (int i = 0; i < 16; ++i) {
        std::optional<Bytes> fr = c.recv_frame();
        if (!fr) return {};  // connection ended
        std::optional<mw::Frame> rf = mw::decode_frame(*fr);
        if (!rf) continue;
        if (rf->opcode != mn::Opcode::VITALS_UPDATE) continue;  // e.g. EntityEnter
        Bytes pl(rf->payload, rf->payload + rf->payload_len);
        const auto* u = decode<mn::VitalsUpdate>(pl);
        if (!u) continue;
        return Vitals{true, u->entity_guid(), u->health(), u->max_health(), u->power(),
                      u->max_power(), u->power_type(), u->level()};
    }
    return {};
}

// Standalone `character` table DDL (mirrors 0001_init_characters.up.sql). No-op
// when the real migration already created the table.
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

// Seed one owned character of `char_class` and return its minted character.id.
// map/pos default 0 — worldd re-seeds the spawn at zone centre on ENTER_WORLD.
std::uint64_t seed_character(db::Connection& db, std::uint64_t account_id,
                             const std::string& name, std::uint8_t char_class) {
    db.execute("DELETE FROM `character` WHERE name = ?", {db::Param{name}});
    db.execute(
        "INSERT INTO `character` "
        "(account_id, name, race, class, map_id, pos_x, pos_y, pos_z) "
        "VALUES (?, ?, 1, ?, 0, 0, 0, 0)",
        {db::Param{std::to_string(account_id)}, db::Param{name},
         db::Param{static_cast<std::int64_t>(char_class)}});
    db::Result r = db.execute("SELECT id FROM `character` WHERE name = ?",
                              {db::Param{name}});
    return cell_u64(r.rows.at(0)[0]);
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::srand(static_cast<unsigned>(std::time(nullptr) ^ ::getpid()));
    std::printf("worldd self-vitals-at-ENTER_WORLD live-session test (#439, UI-01)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — self-vitals "
                    "end-to-end check skipped (the DB-free vitals broadcast logic is "
                    "proven by worldd-vitals-test)\n");
        std::printf("\nWORLDD SELF-VITALS TEST SKIPPED (no DB)\n");
        return 0;
    }

    char tmpl[] = "/tmp/meridian-worldd-selfvitals-XXXXXX";
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
        const std::string username = "worldd_sv_" + std::to_string(std::rand());
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute(
            "INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
            {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        db::Result ar = db.execute("SELECT id FROM account WHERE username = ?",
                                   {db::Param{username}});
        std::uint64_t account_id = cell_u64(ar.rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        // Seed ONE owned character as a Runcaller (class 2 -> MANA pool), so the
        // self VITALS_UPDATE carries a non-zero power/max_power AND MANA power_type
        // (a richer assertion than a rage/energy class, which spawn health-only-
        // interesting). The placeholder curve spawns it alive at full health + full
        // mana at level 1 (combat_unit placeholder_player_stats).
        db.execute(kCharacterDdl);
        const std::string char_name = "SvChar_" + std::to_string(std::rand());
        const std::uint64_t char_id = seed_character(db, account_id, char_name, /*class=*/2);
        check("test character seeded (Runcaller/mana)", char_id > 0);

        const std::string realm_name = "SV IT Realm " + std::to_string(std::rand());
        db.execute(
            "INSERT INTO realm (name, address, port, build_min, build_max) "
            "VALUES (?, '127.0.0.1', 7400, 0, 100000)",
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
        const std::uint64_t grant_id = rand_u64();
        const Bytes session_key(32, 0xAB);
        seed_grant(db, grant_id, account_id, realm_id, session_key, client_build,
                   "DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND)");
        check("valid grant seeded", true);

        // --- Stand up the real listener + serve loop. ------------------------
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
        wcfg.char_db = p;   // characters DB (same instance) — ENTER_WORLD ownership load
        wcfg.realm_id = realm_id;
        mw::Dispatcher dispatcher;
        mw::WorldServer world(dispatcher, wcfg);
        world.start();

        // Serve exactly one connection on its own thread.
        std::thread server([&] {
            try {
                net::Session s = listener.accept();
                world.serve_connection(std::move(s));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "  server accept error: %s\n", e.what());
            }
        });

        {
            // ===== The session logs in and enters the world. ================
            Client a(port);
            check("connected (TLS)", a.connected());
            check("HandshakeOk", do_handshake(a, grant_id, client_build));
            check("entered world as its owned character", do_enter_world(a, char_id));

            // ===== The self vitals snapshot arrives BEFORE any combat. =======
            // No MOVEMENT_INTENT and no CAST_REQUEST have been sent, and the
            // character has taken no damage — so the ONLY VITALS_UPDATE the server
            // can send now is the #439 self snapshot pushed by the ENTER_WORLD
            // handler. The HUD player frame populates from it immediately.
            const Vitals v = recv_vitals(a);
            check("1: received a self VITALS_UPDATE at ENTER_WORLD (before any combat)",
                  v.got);
            check("1: the vitals are FOR this character (matching char id guid)",
                  v.got && v.guid == char_id);
            check("2: health is populated (> 0)", v.health > 0);
            check("2: max_health is populated (> 0)", v.max_health > 0);
            check("2: the character spawns at full health (health == max_health)",
                  v.health == v.max_health);
            check("3: max_power is populated for a mana class (> 0)", v.max_power > 0);
            check("3: power is populated at full mana (power == max_power)",
                  v.power == v.max_power);
            check("3: the power_type is MANA (Runcaller)",
                  v.power_type == mn::PowerType::MANA);
            check("4: the level is populated (== 1 at spawn)", v.level == 1);

            // Client closes here (scope exit) -> the serve thread unwinds.
        }

        server.join();
        world.stop();

        db.execute("DELETE FROM session_grant WHERE account_id = ?",
                   {db::Param{static_cast<std::int64_t>(account_id)}});
        db.execute("DELETE FROM `character` WHERE id = ?",
                   {db::Param{std::to_string(char_id)}});
        db.execute("DELETE FROM realm WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(realm_id)}});
        db.execute("DELETE FROM account WHERE id = ?",
                   {db::Param{std::to_string(account_id)}});
    } catch (const db::DbError& e) {
        std::fprintf(stderr, "DB error: %s\n", e.what());
        ++g_fail;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        ++g_fail;
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    std::printf(g_fail == 0 ? "\nALL WORLDD SELF-VITALS E2E TESTS PASSED\n"
                            : "\n%d WORLDD SELF-VITALS E2E TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
