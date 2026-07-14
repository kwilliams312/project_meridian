// SPDX-License-Identifier: Apache-2.0
//
// worldd — KNOWN_ABILITIES enter-world LIVE-SESSION integration test (issue #457,
// epic #24). Proves the server->client SPELLBOOK read contract the action bar (#456)
// needs, end-to-end over a REAL TLS 1.3 session against a REAL MariaDB, using the M1
// PLACEHOLDER stores (the same trainer NPC + ability store worldd falls back to when
// no world content DB is installed):
//
//   GAP — known abilities (KNOWN_ABILITIES, CMB-01): the character's KNOWN ability
//   set + per-ability cast/GCD/resource metadata, pushed at ENTER_WORLD and re-pushed
//   on a TRAINER_LEARN that grows the set. Before #457 the client was NEVER told which
//   abilities the character knows or their metadata, so the action bar ran off a
//   greybox set and over-predicted a GCD for a triggers_gcd:false ability.
//
// Flow: a fresh Runcaller enters world -> KNOWN_ABILITIES with an EMPTY set (M1: a new
// character knows nothing — no durable ability table, #372). It then learns the
// placeholder trainer's any-class heal (kTrainedHeal) -> KNOWN_ABILITIES re-pushed
// with that ONE ability carrying the AbilityStore (#343) metadata: cast_ms 2000,
// triggers_gcd true, MANA resource, cost 25, range 40 m.
//
// CLEAN-ROOM: written from issue #457, the server SAD, the world.fbs wire contract,
// the meridian-characters / meridian-npc / meridian-worldd public APIs and the
// OpenSSL public API only. No GPL source consulted (CONTRIBUTING.md). The TLS/cert/
// Client/seed scaffolding mirrors hud_read_contracts_it (same clean-room helpers).
//
// DB-GATED: needs a live MariaDB with the auth schema (session_grant / account /
// realm). It CREATE-TABLE-IF-NOT-EXISTS the `character` table (mirroring
// 0001_init_characters.up.sql). Reads MERIDIAN_DB_* env and SKIPS (exit 0) when none
// are set — inert in the plain server ctest, real only in the worldd-session CI job
// (or locally via scripts/dev/test.sh --db).

#include "world_dispatch.h"
#include "world_session.h"

#include "characters.h"
#include "npc_def.h"           // kNpcTrainer + kTrainedHeal placeholder ids
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
namespace npc = meridian::npc;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

using Bytes = std::vector<std::uint8_t>;

// M1 placeholder content (mirror the server-side placeholder stores).
constexpr std::uint64_t kTrainerNpc = npc::kNpcTrainer;   // the placeholder trainer NPC guid
constexpr std::uint32_t kHealAbl    = npc::kTrainedHeal;  // any-class heal (level 5, 120c)
// The placeholder Heal's AbilityStore (#343) metadata — see ability_store.cpp.
constexpr std::uint32_t kHealCastMs   = 2000;
constexpr std::uint32_t kHealCost     = 25;
constexpr float         kHealRangeM   = 40.0f;
constexpr std::int64_t  kStartMoney   = 1000;  // enough to afford the 120c heal

// ---- Throwaway self-signed cert (OpenSSL API) -------------------------------
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
            reinterpret_cast<const unsigned char*>("meridian-known-abl-test"), -1, -1, 0);
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

// ---- Minimal TLS 1.3 IF-2 client --------------------------------------------
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

// ---- world.fbs payload builders ---------------------------------------------
Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}
Bytes enc_world_hello(std::uint64_t grant_id, std::uint32_t build) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateWorldHello(b, grant_id, build, 0, 0));
    return bytes_of(b);
}
Bytes enc_enter_world_request(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldRequest(b, character_id));
    return bytes_of(b);
}
Bytes enc_gossip_hello(std::uint64_t npc_guid) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateGossipHello(b, npc_guid));
    return bytes_of(b);
}
Bytes enc_trainer_learn(std::uint64_t npc_guid, std::uint32_t ability_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateTrainerLearn(b, npc_guid, ability_id));
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
db::Param sid(std::uint64_t v) { return db::Param{std::to_string(v)}; }
db::Param blob32(const Bytes& b) { return db::Param{db::Bytes_t(b.begin(), b.end())}; }

struct RxFrame {
    mn::Opcode opcode{};
    Bytes payload;
};
std::optional<RxFrame> recv_decoded(Client& c) {
    std::optional<Bytes> raw = c.recv_frame();
    if (!raw) return std::nullopt;
    std::optional<mw::Frame> rf = mw::decode_frame(*raw);
    if (!rf) return std::nullopt;
    return RxFrame{rf->opcode, Bytes(rf->payload, rf->payload + rf->payload_len)};
}
// Read frames (bounded) until `opcode` arrives, returning its payload. The server
// interleaves this session's own pushes (VITALS_UPDATE, INVENTORY_SNAPSHOT,
// KNOWN_ABILITIES, GOSSIP_MENU/TRAINER_LIST) — a straggler is simply skipped until
// the awaited opcode surfaces.
std::optional<Bytes> read_until(Client& c, mn::Opcode opcode, int budget = 12) {
    for (int i = 0; i < budget; ++i) {
        std::optional<RxFrame> rf = recv_decoded(c);
        if (!rf) return std::nullopt;
        if (rf->opcode == opcode) return rf->payload;
    }
    return std::nullopt;
}

void seed_grant(db::Connection& db, std::uint64_t grant_id, std::uint64_t account_id,
                std::uint32_t realm_id, const Bytes& session_key, std::uint32_t build) {
    db.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, expires_at) "
        "VALUES (?, ?, ?, ?, ?, DATE_ADD(UTC_TIMESTAMP(), INTERVAL 60 SECOND))",
        {sid(grant_id), sid(account_id),
         db::Param{static_cast<std::int64_t>(realm_id)}, blob32(session_key),
         db::Param{static_cast<std::int64_t>(build)}});
}

constexpr const char* kCharacterDdl =
    "CREATE TABLE IF NOT EXISTS `character` ("
    "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  account_id BIGINT UNSIGNED NOT NULL,"
    "  name VARCHAR(32) NOT NULL,"
    "  race TINYINT UNSIGNED NOT NULL,"
    "  class TINYINT UNSIGNED NOT NULL,"
    "  level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
    "  xp INT UNSIGNED NOT NULL DEFAULT 0,"
    "  money BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    "  map_id INT UNSIGNED NOT NULL,"
    "  instance_id INT UNSIGNED NOT NULL DEFAULT 0,"
    "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL,"
    "  pos_o FLOAT NOT NULL DEFAULT 0,"
    "  played_time INT UNSIGNED NOT NULL DEFAULT 0,"
    "  logout_at DATETIME NULL,"
    "  save_epoch BIGINT NOT NULL DEFAULT 0,"
    "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_character_name (name),"
    "  KEY idx_character_account (account_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

// Find a KnownAbility row for `ability_id` in a KnownAbilities table (or nullptr).
const mn::KnownAbility* find_ability(const mn::KnownAbilities* m, std::uint32_t ability_id) {
    if (!m || !m->abilities()) return nullptr;
    for (const auto* a : *m->abilities())
        if (a->ability_id() == ability_id) return a;
    return nullptr;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd KNOWN_ABILITIES enter-world LIVE-SESSION test (#457)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — known-abilities "
                    "checks skipped\n");
        return 0;
    }

    char tmpl[] = "/tmp/meridian-known-abl-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    const int salt = std::rand();
    const std::string name = "Abl_" + std::to_string(salt);

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);
        db.execute("DELETE FROM `character` WHERE name = ?", {db::Param{name}});

        const std::string username = "worldd_abl_" + std::to_string(salt);
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute("INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
                   {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        std::uint64_t account_id = cell_u64(
            db.execute("SELECT id FROM account WHERE username = ?",
                       {db::Param{username}}).rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        const std::string realm_name = "Abl Realm " + std::to_string(salt);
        db.execute("INSERT INTO realm (name, address, port, build_min, build_max) "
                   "VALUES (?, '127.0.0.1', 7200, 0, 100000)", {db::Param{realm_name}});
        std::uint32_t realm_id = static_cast<std::uint32_t>(cell_u64(
            db.execute("SELECT id FROM realm WHERE name = ?",
                       {db::Param{realm_name}}).rows.at(0)[0]));
        check("test realm seeded", realm_id > 0);

        auto rand_u64 = [] {
            std::uint64_t v = (static_cast<std::uint64_t>(std::rand()) << 32) ^
                              static_cast<std::uint64_t>(std::rand());
            return v == 0 ? 1 : v;
        };
        std::uint64_t grant_ok = rand_u64();
        seed_grant(db, grant_ok, account_id, realm_id, Bytes(32, 0xAB), client_build);

        // A Runcaller at level 5 with 1000 copper: eligible for the placeholder
        // trainer's any-class heal (kTrainedHeal — level 5, 120c).
        chr::CreateRequest cr;
        cr.account_id = account_id;
        cr.name = name;
        cr.race = static_cast<std::uint8_t>(chr::kRaceSylvane);
        cr.char_class = static_cast<std::uint8_t>(chr::kClassRuncaller);
        std::uint64_t char_id = chr::create_character(db, cr).character_id;
        check("character created", char_id > 0);
        db.execute("UPDATE `character` SET level = 5, money = ? WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(kStartMoney)}, sid(char_id)});

        // Stand up the real listener + serve loop with auth + char DB (placeholder
        // content stores — the placeholder trainer NPC + ability store).
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;
        net::TlsListener listener(lc);
        std::uint16_t sport = listener.local_port();
        check("listener bound to ephemeral port", sport != 0);

        mw::WorldServerConfig wcfg;
        wcfg.auth_db = p;
        wcfg.char_db = p;
        wcfg.realm_id = realm_id;
        mw::Dispatcher dispatcher;
        mw::WorldServer world(dispatcher, wcfg);
        world.start();

        std::thread server([&] {
            try {
                net::Session s = listener.accept();
                world.serve_connection(std::move(s));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "  server thread error: %s\n", e.what());
            }
        });

        {
            Client c(sport);
            check("client connected (TLS)", c.connected());
            std::uint64_t seq = 1;

            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, seq++,
                                          enc_world_hello(grant_ok, client_build)));
            check("handshake ok", read_until(c, mn::Opcode::HANDSHAKE_OK).has_value());

            // --- ENTER_WORLD -> OK, then the enter-world KNOWN_ABILITIES (EMPTY) ----
            c.send_frame(mw::encode_frame(mn::Opcode::ENTER_WORLD_REQUEST, seq++,
                                          enc_enter_world_request(char_id)));
            if (auto pl = read_until(c, mn::Opcode::ENTER_WORLD_RESPONSE)) {
                const auto* m = decode<mn::EnterWorldResponse>(*pl);
                check("ENTER_WORLD -> OK", m && m->status() == mn::EnterWorldStatus::OK);
            } else {
                check("got an EnterWorldResponse", false);
            }
            if (auto pl = read_until(c, mn::Opcode::KNOWN_ABILITIES)) {
                const auto* m = decode<mn::KnownAbilities>(*pl);
                check("ENTER_WORLD pushes KNOWN_ABILITIES", m != nullptr);
                // A fresh character knows nothing at M1 (no durable ability table).
                check("a fresh character's known set is empty",
                      m && (m->abilities() == nullptr || m->abilities()->size() == 0));
            } else {
                check("got an enter-world KNOWN_ABILITIES", false);
            }

            // --- GOSSIP_HELLO to the trainer NPC (drains the pushed TRAINER_LIST) ---
            c.send_frame(mw::encode_frame(mn::Opcode::GOSSIP_HELLO, seq++,
                                          enc_gossip_hello(kTrainerNpc)));
            check("trainer gossip auto-pushes TRAINER_LIST",
                  read_until(c, mn::Opcode::TRAINER_LIST).has_value());

            // --- TRAINER_LEARN the any-class heal -> OK, then a re-pushed spellbook -
            c.send_frame(mw::encode_frame(mn::Opcode::TRAINER_LEARN, seq++,
                                          enc_trainer_learn(kTrainerNpc, kHealAbl)));
            if (auto pl = read_until(c, mn::Opcode::TRAINER_LEARN_RESULT)) {
                const auto* m = decode<mn::TrainerLearnResult>(*pl);
                check("TRAINER_LEARN heal -> OK",
                      m && m->status() == mn::TrainerLearnStatus::OK);
            } else {
                check("got a TrainerLearnResult", false);
            }
            if (auto pl = read_until(c, mn::Opcode::KNOWN_ABILITIES)) {
                const auto* m = decode<mn::KnownAbilities>(*pl);
                check("learning re-pushes KNOWN_ABILITIES", m != nullptr);
                check("the known set now holds exactly the one learned ability",
                      m && m->abilities() != nullptr && m->abilities()->size() == 1);
                const mn::KnownAbility* heal = find_ability(m, kHealAbl);
                check("the learned heal appears in the known set", heal != nullptr);
                if (heal) {
                    check("known heal cast_ms == AbilityStore metadata (2000)",
                          heal->cast_ms() == kHealCastMs);
                    check("known heal triggers_gcd == true", heal->triggers_gcd() == true);
                    check("known heal resource_type == MANA",
                          heal->resource_type() == mn::AbilityResource::MANA);
                    check("known heal resource_cost == AbilityStore metadata (25)",
                          heal->resource_cost() == kHealCost);
                    check("known heal range_m == AbilityStore metadata (40)",
                          heal->range_m() == kHealRangeM);
                }
            } else {
                check("got a post-learn KNOWN_ABILITIES", false);
            }
        }

        server.join();  // the client block closed above -> serve_connection returned
        world.stop();

        // Cleanup: the character, grant, realm, account.
        db.execute("DELETE FROM `character` WHERE id = ?", {sid(char_id)});
        db.execute("DELETE FROM session_grant WHERE account_id = ?", {sid(account_id)});
        db.execute("DELETE FROM realm WHERE id = ?", {sid(realm_id)});
        db.execute("DELETE FROM account WHERE id = ?", {sid(account_id)});
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

    std::printf(g_fail == 0 ? "\nALL KNOWN-ABILITIES CHECKS PASSED\n"
                            : "\n%d KNOWN-ABILITIES CHECK(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
