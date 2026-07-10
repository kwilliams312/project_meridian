// SPDX-License-Identifier: Apache-2.0
//
// worldd — BAN + MUTE enforcement END-TO-END integration test (OPS-02c, #419).
//
// CLEAN-ROOM: written from issue #419 + the server SAD (§5.3 grant handoff, §5.2
// IF-2 framing) + world.fbs + meridian/bans/bans.h + the OpenSSL public API. The
// TLS/cert/Client/grant-seed scaffolding mirrors single_session_it (same clean-room
// helpers). No GPL source consulted (CONTRIBUTING.md).
//
// Composes meridian::world-dispatch (the REAL serve loop + WORLD_HELLO / ENTER_WORLD
// / CHAT_MESSAGE handlers) against a REAL MariaDB + REAL TLS 1.3, proving the story's
// worldd acceptance list AT THE WIRE:
//   A. BAN DROP — a session for a BANNED account is refused at WorldHello: the
//      client gets Disconnect{KICKED}, never HandshakeOk.
//   B. MUTE — a spawned character's chat routes normally UNTIL it is muted; a muted
//      character's CHAT_MESSAGE is dropped with a "you are muted" System reply and
//      never delivered; once the mute has EXPIRED, chat routes again.
//
// Reads MERIDIAN_DB_* and SKIPs (exit 0) when unset (inert in the DB-free ctest).

#include "world_dispatch.h"
#include "world_session.h"
#include "world_state.h"

#include "meridian/bans/bans.h"
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
namespace bans = meridian::bans;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
const char* env(const char* k) { return std::getenv(k); }
using Bytes = std::vector<std::uint8_t>;

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
            reinterpret_cast<const unsigned char*>("meridian-worldd-ban-mute-test"),
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

Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_world_hello(std::uint64_t grant_id, std::uint32_t build) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateWorldHello(b, grant_id, build, /*nonce=*/0, /*proof=*/0));
    return bytes_of(b);
}
Bytes enc_enter_world(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldRequest(b, character_id));
    return bytes_of(b);
}
Bytes enc_chat(mn::ChatChannel channel, const std::string& text) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateChatMessageDirect(b, channel, /*target=*/"", text.c_str()));
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
                std::uint32_t realm_id, const Bytes& session_key, std::uint32_t build) {
    db.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, expires_at) "
        "VALUES (?, ?, ?, ?, ?, DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 SECOND))",
        {db::Param{std::to_string(grant_id)}, db::Param{std::to_string(account_id)},
         db::Param{static_cast<std::int64_t>(realm_id)}, blob32(session_key),
         db::Param{static_cast<std::int64_t>(build)}});
}

// Send WorldHello; return the reply opcode (HANDSHAKE_OK or DISCONNECT), or nullopt.
std::optional<mn::Opcode> hello_reply(Client& c, std::uint64_t grant, std::uint32_t build) {
    if (!c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, 1, enc_world_hello(grant, build))))
        return std::nullopt;
    std::optional<Bytes> r = c.recv_frame();
    if (!r) return std::nullopt;
    std::optional<mw::Frame> rf = mw::decode_frame(*r);
    if (!rf) return std::nullopt;
    return rf->opcode;
}

bool do_handshake(Client& c, std::uint64_t grant, std::uint32_t build) {
    return hello_reply(c, grant, build) == mn::Opcode::HANDSHAKE_OK;
}
bool do_enter_world(Client& c, std::uint64_t character_id) {
    if (!c.send_frame(mw::encode_frame(mn::Opcode::ENTER_WORLD_REQUEST, 2,
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

// A delivered chat line captured off the wire.
struct Chat {
    std::string sender_name;
    std::string text;
};

// Send a SAY and read frames until a CHAT_DELIVER arrives (skipping any ENTITY_*
// relay frames), returning it. A muted send yields the "System" notice; an
// unmuted one yields the sender's own spatial echo.
std::optional<Chat> say_and_recv(Client& c, std::uint32_t seq, const std::string& text) {
    if (!c.send_frame(mw::encode_frame(mn::Opcode::CHAT_MESSAGE, seq,
                                       enc_chat(mn::ChatChannel::SAY, text))))
        return std::nullopt;
    for (int i = 0; i < 16; ++i) {
        std::optional<Bytes> r = c.recv_frame();
        if (!r) return std::nullopt;
        std::optional<mw::Frame> rf = mw::decode_frame(*r);
        if (!rf || rf->opcode != mn::Opcode::CHAT_DELIVER) continue;
        Bytes pl(rf->payload, rf->payload + rf->payload_len);
        const auto* d = decode<mn::ChatDeliver>(pl);
        if (d == nullptr) continue;
        return Chat{d->sender_name() ? d->sender_name()->str() : std::string(),
                    d->text() ? d->text()->str() : std::string()};
    }
    return std::nullopt;
}

constexpr const char* kCharacterDdl =
    "CREATE TABLE IF NOT EXISTS `character` ("
    "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  account_id BIGINT UNSIGNED NOT NULL,"
    "  name VARCHAR(32) NOT NULL,"
    "  race TINYINT UNSIGNED NOT NULL,"
    "  class TINYINT UNSIGNED NOT NULL,"
    "  appearance JSON NULL,"  // 0003 (§5.2): opaque visual record
    "  level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
    "  xp INT UNSIGNED NOT NULL DEFAULT 0,"
    "  money BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    "  map_id INT UNSIGNED NOT NULL,"
    "  instance_id INT UNSIGNED NOT NULL DEFAULT 0,"
    "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
    "  pos_o FLOAT NOT NULL DEFAULT 0, played_time INT UNSIGNED NOT NULL DEFAULT 0,"
    "  logout_at DATETIME NULL, save_epoch BIGINT NOT NULL DEFAULT 0,"
    "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id), UNIQUE KEY uq_character_name (name),"
    "  KEY idx_character_account (account_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

// Standalone character_mute DDL (mirrors 0002_character_mute.up.sql, minus the FK
// so it stands in whatever DB char_db points at — here the auth DB).
constexpr const char* kMuteDdl =
    "CREATE TABLE IF NOT EXISTS `character_mute` ("
    "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  char_id BIGINT UNSIGNED NOT NULL,"
    "  expires_at DATETIME NULL,"
    "  reason VARCHAR(255) NOT NULL,"
    "  muted_by BIGINT UNSIGNED NULL,"
    "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id), KEY idx_character_mute_char (char_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

std::uint64_t seed_account(db::Connection& db, const std::string& username) {
    db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
    db.execute("INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
               {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
    db::Result r = db.execute("SELECT id FROM account WHERE username = ?",
                              {db::Param{username}});
    return cell_u64(r.rows.at(0)[0]);
}

std::uint64_t seed_character(db::Connection& db, std::uint64_t account_id,
                             const std::string& name) {
    db.execute("DELETE FROM `character` WHERE name = ?", {db::Param{name}});
    db.execute(
        "INSERT INTO `character` (account_id, name, race, class, map_id, pos_x, pos_y, pos_z) "
        "VALUES (?, ?, 1, 1, 0, 0, 0, 0)",
        {db::Param{std::to_string(account_id)}, db::Param{name}});
    db::Result r = db.execute("SELECT id FROM `character` WHERE name = ?", {db::Param{name}});
    return cell_u64(r.rows.at(0)[0]);
}

std::uint64_t rand_u64() {
    std::uint64_t v = (static_cast<std::uint64_t>(std::rand()) << 32) ^
                      static_cast<std::uint64_t>(std::rand());
    return v == 0 ? 1 : v;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::srand(static_cast<unsigned>(std::time(nullptr) ^ ::getpid()));
    std::printf("worldd BAN + MUTE enforcement END-TO-END test (OPS-02c #419)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* configured — worldd ban/mute E2E skipped "
                    "(the bans-lib logic is proven by the bans test)\n");
        return 0;
    }

    char tmpl[] = "/tmp/meridian-worldd-bm-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t build = 1000;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);
        db.execute(kMuteDdl);

        // --- Seed a realm (grants FK it). ------------------------------------
        const std::string realm_name = "BM IT Realm " + std::to_string(std::rand());
        db.execute(
            "INSERT INTO realm (name, address, port, build_min, build_max) "
            "VALUES (?, '127.0.0.1', 7400, 0, 100000)",
            {db::Param{realm_name}});
        std::uint32_t realm_id = static_cast<std::uint32_t>(cell_u64(
            db.execute("SELECT id FROM realm WHERE name = ?", {db::Param{realm_name}})
                .rows.at(0)[0]));
        check("test realm seeded", realm_id > 0);

        // Banned account (Part A) + a clean account+character (Part B).
        const std::string banned_user = "bm_banned_" + std::to_string(std::rand());
        const std::uint64_t banned_id = seed_account(db, banned_user);
        bans::ban_account(db, banned_id, "test ban", /*issued_by=*/0, std::nullopt);
        check("banned account seeded + banned", banned_id > 0 &&
              bans::account_ban(db, banned_id).has_value());

        const std::string clean_user = "bm_clean_" + std::to_string(std::rand());
        const std::uint64_t clean_id = seed_account(db, clean_user);
        const std::string char_name = "BmChar_" + std::to_string(std::rand());
        const std::uint64_t char_id = seed_character(db, clean_id, char_name);
        check("clean account + character seeded", clean_id > 0 && char_id > 0);

        const Bytes key(32, 0xCD);
        const std::uint64_t grant_banned = rand_u64();
        const std::uint64_t grant_clean = rand_u64();
        seed_grant(db, grant_banned, banned_id, realm_id, key, build);
        seed_grant(db, grant_clean, clean_id, realm_id, key, build);

        // --- Stand up the real serve loop. -----------------------------------
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;
        net::TlsListener listener(lc);
        std::uint16_t sport = listener.local_port();
        check("listener bound", sport != 0);

        mw::WorldServerConfig wcfg;
        wcfg.auth_db = p;
        wcfg.char_db = p;
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
                    std::fprintf(stderr, "  server accept note: %s\n", e.what());
                }
            }
            for (auto& t : conns) t.join();
        });

        // ===== A. BANNED account is DROPPED at WorldHello ====================
        {
            Client a(sport);
            check("A: connected (TLS)", a.connected());
            std::optional<mn::Opcode> op = hello_reply(a, grant_banned, build);
            check("A: WorldHello for a banned account is refused (Disconnect, not HandshakeOk)",
                  op == mn::Opcode::DISCONNECT);
        }

        // ===== B. MUTE enforcement (unmuted -> muted -> expired) ============
        {
            Client b(sport);
            check("B: connected (TLS)", b.connected());
            check("B: HandshakeOk (clean account)", do_handshake(b, grant_clean, build));
            check("B: entered world", do_enter_world(b, char_id));

            // B.1 — NOT muted: a SAY echoes back as the character (normal delivery).
            {
                std::optional<Chat> got = say_and_recv(b, 10, "hello world");
                check("B.1 unmuted: a CHAT_DELIVER came back", got.has_value());
                check("B.1 unmuted: it is the character's own echo (not a mute notice)",
                      got && got->sender_name == char_name && got->text == "hello world");
            }

            // B.2 — MUTE the character (permanent), then a SAY is DROPPED with the
            // "you are muted" System notice (queried fresh, so it applies at once).
            bans::mute_character(db, char_id, "flooding", /*issued_by=*/0, std::nullopt);
            check("B.2 setup: the character reads as muted",
                  bans::character_mute(db, char_id).has_value());
            {
                std::optional<Chat> got = say_and_recv(b, 11, "muted message");
                check("B.2 muted: a reply came back", got.has_value());
                check("B.2 muted: it is the System 'you are muted' notice",
                      got && got->sender_name == "System" &&
                          got->text.find("muted") != std::string::npos);
                check("B.2 muted: the original text was NOT delivered",
                      got && got->text != "muted message");
            }

            // B.3 — the mute EXPIRES: replace it with an already-elapsed row; chat
            // routes normally again (expiry respected).
            db.execute("DELETE FROM `character_mute` WHERE char_id = ?",
                       {db::Param{static_cast<std::int64_t>(char_id)}});
            db.execute(
                "INSERT INTO `character_mute` (char_id, expires_at, reason) "
                "VALUES (?, UTC_TIMESTAMP() - INTERVAL 1 MINUTE, 'lapsed')",
                {db::Param{static_cast<std::int64_t>(char_id)}});
            check("B.3 setup: the mute is now expired (not active)",
                  !bans::character_mute(db, char_id).has_value());
            {
                std::optional<Chat> got = say_and_recv(b, 12, "after expiry");
                check("B.3 after expiry: chat routes normally again",
                      got && got->sender_name == char_name && got->text == "after expiry");
            }
        }

        server.join();
        world.stop();

        // Cleanup.
        db.execute("DELETE FROM `character_mute` WHERE char_id = ?",
                   {db::Param{static_cast<std::int64_t>(char_id)}});
        db.execute("DELETE FROM `character` WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(char_id)}});
        db.execute("DELETE FROM session_grant WHERE account_id IN (?, ?)",
                   {db::Param{static_cast<std::int64_t>(banned_id)},
                    db::Param{static_cast<std::int64_t>(clean_id)}});
        db.execute("DELETE FROM account WHERE id IN (?, ?)",
                   {db::Param{static_cast<std::int64_t>(banned_id)},
                    db::Param{static_cast<std::int64_t>(clean_id)}});  // account_ban cascades
        db.execute("DELETE FROM realm WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(realm_id)}});
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

    std::printf(g_fail == 0 ? "\nALL WORLDD BAN/MUTE E2E TESTS PASSED\n"
                            : "\n%d WORLDD BAN/MUTE E2E TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
