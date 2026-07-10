// SPDX-License-Identifier: Apache-2.0
//
// worldd character-management integration test (CHR-01 over IF-2; issue #286,
// decision D-35).
//
// CLEAN-ROOM: written from the server SAD (§5.2 IF-2 framing, §5.3 IF-3 session
// handoff, §4.2/§4.4 characters DB + soft-ref rule, decision D-11 roster), the
// world.fbs wire contract, and the meridian-characters public API only. No GPL
// source consulted (CONTRIBUTING).
//
// Composes meridian::world-dispatch (the real serve/dispatch loop + the REAL
// CHAR_LIST/CREATE/DELETE handlers, backed by the meridian-characters CRUD)
// against a REAL MariaDB + a REAL TLS 1.3 socket. It drives the char-management
// ops over an AUTHENTICATED world session exactly as a client would.
//
// DB-GATED: needs a live MariaDB with the auth schema loaded (session_grant /
// account / realm) — it seeds a grant exactly as authd writes it and consumes it
// via the real WORLD_HELLO handler. It CREATE-TABLE-IF-NOT-EXISTS the `character`
// table (standalone — no outgoing FK, §4.4), so the SAME database that holds the
// auth schema also serves as the characters DB for the test (char_db points at
// the same connection params). Reads MERIDIAN_DB_* env and SKIPS (exit 0) when
// none are set, so it is inert in the plain server ctest and runs for real only
// in the worldd-session CI job (or locally via scripts/dev/test.sh --db).
//
// What it proves end-to-end over one authenticated session (the #286 acceptance
// list), plus the account-scoping/ownership guarantee:
//   0. A char request BEFORE the handshake is rejected (Disconnect) — char
//      management is legal only on an authenticated session.
//   1. list on a fresh account is EMPTY.
//   1b. a name already owned by another account is rejected (DUPLICATE_NAME) —
//      tested while the session account is still empty (below the #329 cap).
//   2. create -> OK + a minted id; list then shows exactly that character.
//   3. a 2nd create for the SAME account is rejected (LIMIT_REACHED, #329) — the
//      one-character-per-account cap, enforced server-side.
//   4. an invalid race id is rejected (INVALID_RACE).
//   5. an invalid class id is rejected (INVALID_CLASS).
//   6. deleting ANOTHER account's character is REFUSED (ownership predicate) and
//      the victim row survives — the handler acts on the session's account only.
//   7. deleting your OWN character succeeds (OK); list is empty again.

#include "world_dispatch.h"
#include "world_session.h"

#include "characters.h"
#include "meridian/db/connection.h"
#include "meridian/net/tls_listener.h"
#include "roster.h"

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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mw = meridian::worldd;
namespace chr = meridian::characters;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

using Bytes = std::vector<std::uint8_t>;

// ---- Throwaway self-signed cert (OpenSSL API; mirrors the session test) ------
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
            reinterpret_cast<const unsigned char*>("meridian-worldd-charmgmt-test"), -1, -1, 0);
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
    b.Finish(mn::CreateWorldHello(b, grant_id, build, /*nonce=*/0, /*proof=*/0));
    return bytes_of(b);
}
Bytes enc_char_list_request() {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCharListRequest(b));
    return bytes_of(b);
}
Bytes enc_char_create_request(const std::string& name, std::uint8_t race,
                              std::uint8_t cls) {
    fb::FlatBufferBuilder b;
    auto n = b.CreateString(name);
    b.Finish(mn::CreateCharCreateRequest(b, n, race, cls));
    return bytes_of(b);
}
Bytes enc_char_delete_request(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCharDeleteRequest(b, character_id));
    return bytes_of(b);
}
Bytes enc_enter_world_request(std::uint64_t character_id) {
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

db::Param blob32(const Bytes& b) {
    return db::Param{db::Bytes_t(b.begin(), b.end())};
}

// Seed a session_grant row EXACTLY as authd writes it (decimal-string binding for
// the BIGINT UNSIGNED grant_id/account_id — the meridian-db signed-bind gotcha).
void seed_grant(db::Connection& db, std::uint64_t grant_id, std::uint64_t account_id,
                std::uint32_t realm_id, const Bytes& session_key,
                std::uint32_t client_build) {
    db.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, expires_at) "
        "VALUES (?, ?, ?, ?, ?, DATE_ADD(UTC_TIMESTAMP(), INTERVAL 60 SECOND))",
        {db::Param{std::to_string(grant_id)},
         db::Param{std::to_string(account_id)},
         db::Param{static_cast<std::int64_t>(realm_id)},
         blob32(session_key),
         db::Param{static_cast<std::int64_t>(client_build)}});
}

// Standalone `character` table DDL — mirrors server/db/characters/migrations/
// 0001_init_characters.up.sql. The character table has NO outgoing FK (account_id
// is a soft ref, §4.4), so CREATE ... IF NOT EXISTS is a no-op when the real
// migration already loaded it and stands alone otherwise (matches characters_test).
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

// ---- one request/response round-trip over an established session ------------
// Send `req` under `opcode`, read one reply frame, and return its decoded payload
// bytes (validated to `resp_opcode`). Empty optional on any transport/opcode
// mismatch. `seq` is the per-connection sequence (echoed by the server).
std::optional<Bytes> round_trip(Client& c, mn::Opcode opcode, const Bytes& req,
                                mn::Opcode resp_opcode, std::uint64_t seq) {
    if (!c.send_frame(mw::encode_frame(opcode, seq, req))) return std::nullopt;
    std::optional<Bytes> reply = c.recv_frame();
    if (!reply) return std::nullopt;
    std::optional<mw::Frame> rf = mw::decode_frame(*reply);
    if (!rf || rf->opcode != resp_opcode) return std::nullopt;
    return Bytes(rf->payload, rf->payload + rf->payload_len);
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd character-management test (CHR-01 over IF-2, #286 / D-35)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — char-management "
                    "checks skipped (set MERIDIAN_DB_SOCKET or MERIDIAN_DB_HOST + "
                    "MERIDIAN_DB_USER)\n");
        return 0;
    }

    // Self-signed cert into a temp dir.
    char tmpl[] = "/tmp/meridian-worldd-charmgmt-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    // Distinct names so a re-run reliably clears prior rows. std::rand() is
    // unseeded (like characters_test / world_session_test), so `salt` is stable
    // across runs — the pre-cleanup below deletes by NAME (uq_character_name is
    // GLOBAL) to clear any leftover rows from a prior aborted run. account_other
    // is a synthetic owner token for the victim character (soft-ref, §4.4 — no FK).
    const int salt = std::rand();
    const std::uint64_t account_other = 4'300'000'000ULL + static_cast<unsigned>(salt);
    const std::string name_a = "Cm_" + std::to_string(salt) + "_a";
    const std::string name_other = "Cm_" + std::to_string(salt) + "_o";
    std::uint32_t realm_id = 0;
    std::uint64_t grant_ok = 0;
    std::uint64_t grant_account = 0;  // the session's account (real account.id)
    std::uint64_t other_char_id = 0;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);

        // Clear rows this test owns by NAME (the session's account.id is a fresh
        // AUTO_INCREMENT each run, so name is the stable key) — uq_character_name
        // is global, so a leftover name_a from an aborted run would otherwise make
        // the create in step 2 fail as a duplicate.
        auto cleanup_chars = [&] {
            db.execute("DELETE FROM `character` WHERE name IN (?, ?)",
                       {db::Param{name_a}, db::Param{name_other}});
        };
        cleanup_chars();  // clear any stray rows from a prior aborted run

        // --- Seed an account + realm (session_grant FKs both).
        const std::string username = "worldd_charmgmt_" + std::to_string(salt);
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute(
            "INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
            {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        db::Result ar = db.execute("SELECT id FROM account WHERE username = ?",
                                   {db::Param{username}});
        // Bind the grant's account_id to the REAL account row so the FK holds.
        grant_account = cell_u64(ar.rows.at(0)[0]);
        check("test account seeded", grant_account > 0);

        const std::string realm_name = "CM Realm " + std::to_string(salt);
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
        grant_ok = rand_u64();
        seed_grant(db, grant_ok, grant_account, realm_id, Bytes(32, 0xAB), client_build);
        check("valid grant seeded", true);

        // A character owned by ANOTHER account, created directly through the CRUD.
        // The main session (account = grant_account) must NOT be able to delete it.
        chr::CreateRequest ocr;
        ocr.account_id = account_other;
        ocr.name = name_other;
        ocr.race = static_cast<std::uint8_t>(chr::Race::kArdent);
        ocr.char_class = static_cast<std::uint8_t>(chr::Class::kVanguard);
        other_char_id = chr::create_character(db, ocr).character_id;
        check("other account's character seeded", other_char_id > 0);

        // --- Stand up the real TLS listener + the worldd serve loop, with BOTH
        //     the auth DB (grants) and the characters DB wired to the same params
        //     (the `character` table lives in this same DB for the test).
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;  // ephemeral
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound to ephemeral port", port != 0);

        mw::WorldServerConfig wcfg;
        wcfg.auth_db = p;   // grant validation
        wcfg.char_db = p;   // characters DB (same instance; `character` table)
        wcfg.realm_id = realm_id;
        mw::Dispatcher dispatcher;  // the REAL char-management handlers
        mw::WorldServer world(dispatcher, wcfg);
        world.start();

        // Serve two connections: the pre-auth reject, then the main flow.
        std::thread server([&] {
            for (int i = 0; i < 2; ++i) {
                try {
                    net::Session s = listener.accept();
                    world.serve_connection(std::move(s));
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "  server thread error: %s\n", e.what());
                }
            }
        });

        // ===== 0. PRE-AUTH: a char request before the handshake -> Disconnect ==
        {
            Client c(port);
            check("0: pre-auth client connected (TLS)", c.connected());
            c.send_frame(mw::encode_frame(mn::Opcode::CHAR_LIST_REQUEST, /*seq=*/1,
                                          enc_char_list_request()));
            std::optional<Bytes> reply = c.recv_frame();
            bool is_disc = false;
            if (reply) {
                std::optional<mw::Frame> rf = mw::decode_frame(*reply);
                is_disc = rf && rf->opcode == mn::Opcode::DISCONNECT;
            }
            check("0: pre-auth char request got a Disconnect", is_disc);
        }

        // ===== 1..7. MAIN FLOW on one authenticated session ====================
        // Scoped so the client CLOSES before server.join() below — otherwise
        // serve_connection would block forever reading from a still-open peer.
        {
        Client c(port);
        check("main client connected (TLS)", c.connected());
        std::uint64_t seq = 1;

        // Handshake: WorldHello -> HandshakeOk (consumes grant_ok, enters world).
        c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, seq++,
                                      enc_world_hello(grant_ok, client_build)));
        bool handshake_ok = false;
        if (std::optional<Bytes> hs = c.recv_frame()) {
            std::optional<mw::Frame> rf = mw::decode_frame(*hs);
            handshake_ok = rf && rf->opcode == mn::Opcode::HANDSHAKE_OK &&
                           decode<mn::HandshakeOk>(
                               Bytes(rf->payload, rf->payload + rf->payload_len)) != nullptr;
        }
        check("handshake ok on the char-management connection", handshake_ok);

        // 1. list on a fresh account -> empty.
        if (std::optional<Bytes> pl = round_trip(c, mn::Opcode::CHAR_LIST_REQUEST,
                                                 enc_char_list_request(),
                                                 mn::Opcode::CHAR_LIST_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharListResponse>(*pl);
            check("1: list on fresh account is empty",
                  m != nullptr && (m->characters() == nullptr ||
                                   m->characters()->size() == 0));
        } else {
            check("1: got a CharListResponse", false);
        }

        // 1a. ENTER_WORLD on a fresh account (zero characters) -> NO_CHARACTER,
        // no spawn. Server-authoritative: you cannot enter until you own a
        // character (D-35 / #341). The id used is another account's character —
        // still NO_CHARACTER because THIS account owns none.
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::ENTER_WORLD_REQUEST,
                enc_enter_world_request(other_char_id),
                mn::Opcode::ENTER_WORLD_RESPONSE, seq++)) {
            const auto* m = decode<mn::EnterWorldResponse>(*pl);
            check("1a: ENTER_WORLD with no character -> NO_CHARACTER",
                  m != nullptr && m->status() == mn::EnterWorldStatus::NO_CHARACTER);
        } else {
            check("1a: got an EnterWorldResponse", false);
        }

        // 1b. duplicate name -> DUPLICATE_NAME. name_other is owned by
        // account_other (seeded in setup); uq_character_name is GLOBAL, so the
        // session account cannot reuse it. Tested here while the session account
        // is still empty (0 characters) so it is below the #329 cap and the
        // refusal is unambiguously the name collision, not the per-account cap.
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::CHAR_CREATE_REQUEST,
                enc_char_create_request(name_other,
                                        static_cast<std::uint8_t>(chr::Race::kArdent),
                                        static_cast<std::uint8_t>(chr::Class::kVanguard)),
                mn::Opcode::CHAR_CREATE_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharCreateResponse>(*pl);
            check("1b: duplicate name rejected (DUPLICATE_NAME)",
                  m != nullptr && m->status() == mn::CharCreateStatus::DUPLICATE_NAME);
        } else {
            check("1b: got a CharCreateResponse", false);
        }

        // 2. create -> OK + minted id; then list shows exactly that character.
        std::uint64_t minted = 0;
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::CHAR_CREATE_REQUEST,
                enc_char_create_request(name_a,
                                        static_cast<std::uint8_t>(chr::Race::kSylvane),
                                        static_cast<std::uint8_t>(chr::Class::kRuncaller)),
                mn::Opcode::CHAR_CREATE_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharCreateResponse>(*pl);
            check("2: create returns OK",
                  m != nullptr && m->status() == mn::CharCreateStatus::OK);
            if (m) minted = m->character_id();
            check("2: create returns a minted id", minted > 0);
        } else {
            check("2: got a CharCreateResponse", false);
        }

        if (std::optional<Bytes> pl = round_trip(c, mn::Opcode::CHAR_LIST_REQUEST,
                                                 enc_char_list_request(),
                                                 mn::Opcode::CHAR_LIST_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharListResponse>(*pl);
            bool one = m != nullptr && m->characters() != nullptr &&
                       m->characters()->size() == 1;
            check("2: list now shows exactly one character", one);
            if (one) {
                const auto* e = m->characters()->Get(0);
                check("2: listed id matches the minted id", e->character_id() == minted);
                check("2: listed name matches",
                      e->name() != nullptr && e->name()->str() == name_a);
                check("2: listed race matches",
                      e->race() == static_cast<std::uint8_t>(chr::Race::kSylvane));
                check("2: listed class matches",
                      e->char_class() == static_cast<std::uint8_t>(chr::Class::kRuncaller));
                check("2: new character starts at level 1", e->level() == 1);
            }
        } else {
            check("2: got a CharListResponse after create", false);
        }

        // 2a. ENTER_WORLD with ANOTHER account's character id -> NOT_FOUND, no
        // spawn. Ownership is the WHERE predicate; this account now owns its own
        // character, so the miss is NOT_FOUND (not NO_CHARACTER). The session
        // stays alive at character-select.
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::ENTER_WORLD_REQUEST,
                enc_enter_world_request(other_char_id),
                mn::Opcode::ENTER_WORLD_RESPONSE, seq++)) {
            const auto* m = decode<mn::EnterWorldResponse>(*pl);
            check("2a: ENTER_WORLD with a foreign character -> NOT_FOUND",
                  m != nullptr && m->status() == mn::EnterWorldStatus::NOT_FOUND);
        } else {
            check("2a: got an EnterWorldResponse (foreign)", false);
        }

        // 2b. ENTER_WORLD with a nonexistent id -> NOT_FOUND, no spawn.
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::ENTER_WORLD_REQUEST,
                enc_enter_world_request(0xDEADBEEFULL),
                mn::Opcode::ENTER_WORLD_RESPONSE, seq++)) {
            const auto* m = decode<mn::EnterWorldResponse>(*pl);
            check("2b: ENTER_WORLD with a nonexistent id -> NOT_FOUND",
                  m != nullptr && m->status() == mn::EnterWorldStatus::NOT_FOUND);
        } else {
            check("2b: got an EnterWorldResponse (nonexistent)", false);
        }

        // 2c. ENTER_WORLD with the account's OWN character -> OK (spawned as that
        // real character — server-authoritative entry succeeds only for an owned id).
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::ENTER_WORLD_REQUEST,
                enc_enter_world_request(minted),
                mn::Opcode::ENTER_WORLD_RESPONSE, seq++)) {
            const auto* m = decode<mn::EnterWorldResponse>(*pl);
            check("2c: ENTER_WORLD with the owned character -> OK",
                  m != nullptr && m->status() == mn::EnterWorldStatus::OK);
        } else {
            check("2c: got an EnterWorldResponse (owned)", false);
        }

        // 3. second create for the SAME account -> LIMIT_REACHED (#329). The
        // session account already owns name_a (step 2); the one-character-per-
        // account cap refuses a second create. A fresh, unique name is used so
        // the refusal is unambiguously the cap and not a name collision.
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::CHAR_CREATE_REQUEST,
                enc_char_create_request("Cm_" + std::to_string(salt) + "_2nd",
                                        static_cast<std::uint8_t>(chr::Race::kArdent),
                                        static_cast<std::uint8_t>(chr::Class::kVanguard)),
                mn::Opcode::CHAR_CREATE_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharCreateResponse>(*pl);
            check("3: second character for the account rejected (LIMIT_REACHED)",
                  m != nullptr && m->status() == mn::CharCreateStatus::LIMIT_REACHED);
        } else {
            check("3: got a CharCreateResponse", false);
        }

        // 4. invalid race (0 is reserved-invalid) -> INVALID_RACE.
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::CHAR_CREATE_REQUEST,
                enc_char_create_request("Cm_" + std::to_string(salt) + "_r", /*race=*/0,
                                        static_cast<std::uint8_t>(chr::Class::kVanguard)),
                mn::Opcode::CHAR_CREATE_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharCreateResponse>(*pl);
            check("4: invalid race rejected (INVALID_RACE)",
                  m != nullptr && m->status() == mn::CharCreateStatus::INVALID_RACE);
        } else {
            check("4: got a CharCreateResponse", false);
        }

        // 5. invalid class (out of roster range) -> INVALID_CLASS.
        if (std::optional<Bytes> pl = round_trip(
                c, mn::Opcode::CHAR_CREATE_REQUEST,
                enc_char_create_request("Cm_" + std::to_string(salt) + "_c",
                                        static_cast<std::uint8_t>(chr::Race::kArdent),
                                        static_cast<std::uint8_t>(chr::kClassCount + 1)),
                mn::Opcode::CHAR_CREATE_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharCreateResponse>(*pl);
            check("5: invalid class rejected (INVALID_CLASS)",
                  m != nullptr && m->status() == mn::CharCreateStatus::INVALID_CLASS);
        } else {
            check("5: got a CharCreateResponse", false);
        }

        // 6. delete ANOTHER account's character -> REFUSED; victim row survives.
        if (std::optional<Bytes> pl = round_trip(c, mn::Opcode::CHAR_DELETE_REQUEST,
                                                 enc_char_delete_request(other_char_id),
                                                 mn::Opcode::CHAR_DELETE_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharDeleteResponse>(*pl);
            check("6: deleting another account's character is REFUSED",
                  m != nullptr && m->status() == mn::CharDeleteStatus::REFUSED);
        } else {
            check("6: got a CharDeleteResponse", false);
        }
        {
            std::vector<chr::CharacterSummary> victim =
                chr::list_characters(db, account_other);
            check("6: victim character survives the refused delete",
                  victim.size() == 1 && victim[0].id == other_char_id);
        }

        // 7. delete your OWN character -> OK; list is empty again.
        if (std::optional<Bytes> pl = round_trip(c, mn::Opcode::CHAR_DELETE_REQUEST,
                                                 enc_char_delete_request(minted),
                                                 mn::Opcode::CHAR_DELETE_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharDeleteResponse>(*pl);
            check("7: deleting your own character succeeds (OK)",
                  m != nullptr && m->status() == mn::CharDeleteStatus::OK);
        } else {
            check("7: got a CharDeleteResponse", false);
        }
        if (std::optional<Bytes> pl = round_trip(c, mn::Opcode::CHAR_LIST_REQUEST,
                                                 enc_char_list_request(),
                                                 mn::Opcode::CHAR_LIST_RESPONSE, seq++)) {
            const auto* m = decode<mn::CharListResponse>(*pl);
            check("7: list is empty after own delete",
                  m != nullptr && (m->characters() == nullptr ||
                                   m->characters()->size() == 0));
        } else {
            check("7: got a CharListResponse after delete", false);
        }
        }  // main client closes here (before server.join())

        server.join();
        world.stop();

        // --- Cleanup: characters, grants, realm, account.
        cleanup_chars();
        db.execute("DELETE FROM session_grant WHERE account_id = ?",
                   {db::Param{static_cast<std::int64_t>(grant_account)}});
        db.execute("DELETE FROM realm WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(realm_id)}});
        db.execute("DELETE FROM account WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(grant_account)}});
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

    std::printf(g_fail == 0 ? "\nALL CHAR-MANAGEMENT TESTS PASSED\n"
                            : "\n%d CHAR-MANAGEMENT TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
