// SPDX-License-Identifier: Apache-2.0
//
// worldd — trainer-against-DB-content LIVE-SESSION integration test (issue #392,
// epic #20). The acceptance bar for #392's live half: a real TLS 1.3 session drives
// the NPC-02 trainer opcodes (GOSSIP_HELLO -> TRAINER_LIST, TRAINER_LEARN) against a
// world server whose NPC seam is a DbNpcStore loaded from AUTHORED trainer rows
// (npc_template + npc_trainer + npc_trainer_ability), and asserts the DURABLE side
// effect — the learn debits character.money over the wire — plus every rejection axis
// (wrong class, too-low level, insufficient funds, already-known) leaving money
// untouched. This is the end-to-end proof that a DB-loaded trainer works (before
// #392 a DB-backed NpcDef was NEVER a trainer, so this path only worked against the
// placeholder store; see quest_loot_npc_db_it.cpp for the placeholder-store variant).
//
// The DIFFERENCE from quest_loot_npc_db_it.cpp: there the world server uses the
// PLACEHOLDER npc store; here it uses a DbNpcStore built from seeded npc_trainer /
// npc_trainer_ability rows and installed via install_content_stores() — so the trainer
// the client talks to is authored CONTENT, loaded through the #392 loader.
//
// CLEAN-ROOM: written from the server SAD, the world.fbs wire contract, the
// meridian-characters / meridian-worldd public APIs and the OpenSSL public API only.
// No GPL source consulted (CONTRIBUTING.md).
//
// DB-GATED: needs a live MariaDB with the auth schema (session_grant / account /
// realm). It CREATE-TABLE-IF-NOT-EXISTS the `character` table (mirroring
// 0001_init_characters.up.sql) and DROP+CREATE its own world content tables
// (npc_template / quest_template / npc_trainer / npc_trainer_ability) in the same DB,
// so the one connection serves as both the world DB (npc load) and the characters DB.
// Reads MERIDIAN_DB_* env and SKIPS (exit 0) when none are set — inert in the plain
// server ctest, real only in the worldd-session CI job / scripts/dev/test.sh --db.

#include "world_dispatch.h"
#include "world_session.h"

#include "characters.h"
#include "db_content_store.h"   // DbNpcStore (the #392 DB-backed trainer store)
#include "npc_def.h"
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

// The DB-authored trainer (seeded below). The NPC guid the client interacts with is
// the npc_template.id (the gossip/trainer handlers key on the guid directly).
constexpr std::uint32_t kTrainerNpc = 5001;
// Taught abilities — one per rejection axis + one learnable.
constexpr std::uint32_t kAblAny      = 7001;  // any class, level 3, 100c  -> LEARNABLE
constexpr std::uint32_t kAblVanguard = 7002;  // vanguard-only, level 5, 250c -> WRONG_CLASS (char is Runcaller)
constexpr std::uint32_t kAblHighLvl  = 7003;  // any class, level 10, 50c  -> LEVEL_TOO_LOW (char is 5)
constexpr std::uint32_t kAblPricey   = 7004;  // any class, level 1, 5000c -> INSUFFICIENT_FUNDS (char has 300)
constexpr std::int64_t  kStartMoney  = 300;
constexpr std::int64_t  kAnyCost     = 100;

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
            reinterpret_cast<const unsigned char*>("meridian-trainer-db-test"), -1, -1, 0);
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
std::int64_t cell_i64(const db::Cell& c) {
    return c.has_value() ? std::strtoll(c->c_str(), nullptr, 10) : 0;
}
db::Param sid(std::uint64_t v) { return db::Param{std::to_string(v)}; }
db::Param blob32(const Bytes& b) { return db::Param{db::Bytes_t(b.begin(), b.end())}; }

struct RxFrame {
    mn::Opcode opcode{};
    Bytes payload;
};
std::optional<RxFrame> recv_decoded(Client& c) {
    // Skip the unsolicited self VITALS_UPDATE the server now pushes at ENTER_WORLD
    // (#439: the HUD player-frame snapshot) so round_trip reaches the awaited reply.
    // These trainer/gossip round-trips never await a VITALS_UPDATE, so dropping it
    // is safe; the vitals broadcast logic is proven by worldd-vitals-test.
    for (;;) {
        std::optional<Bytes> raw = c.recv_frame();
        if (!raw) return std::nullopt;
        std::optional<mw::Frame> rf = mw::decode_frame(*raw);
        if (!rf) return std::nullopt;
        if (rf->opcode == mn::Opcode::VITALS_UPDATE) continue;

        if (rf->opcode == mn::Opcode::INVENTORY_SNAPSHOT) continue;  // #453 unsolicited bags snapshot
        return RxFrame{rf->opcode, Bytes(rf->payload, rf->payload + rf->payload_len)};
    }
}
std::optional<Bytes> round_trip(Client& c, mn::Opcode opcode, const Bytes& req,
                                mn::Opcode resp_opcode, std::uint64_t seq) {
    if (!c.send_frame(mw::encode_frame(opcode, seq, req))) return std::nullopt;
    std::optional<RxFrame> rf = recv_decoded(c);
    if (!rf || rf->opcode != resp_opcode) return std::nullopt;
    return rf->payload;
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

std::int64_t money_of(db::Connection& db, std::uint64_t char_id) {
    db::Result r = db.execute("SELECT money FROM `character` WHERE id = ?", {sid(char_id)});
    return r.rows.empty() ? -1 : cell_i64(r.rows.at(0)[0]);
}

// The world content tables the DbNpcStore reads. Minimal (no FKs — the store only
// SELECTs), DROP+CREATE for a clean rerun. Columns mirror schema/sql/world/10_npc.sql.
void create_world_tables(db::Connection& c) {
    const char* drops[] = {
        "DROP TABLE IF EXISTS npc_trainer_ability",
        "DROP TABLE IF EXISTS npc_trainer",
        "DROP TABLE IF EXISTS quest_template",
        "DROP TABLE IF EXISTS npc_template",
    };
    for (const char* d : drops) c.execute(d);
    c.execute(
        "CREATE TABLE npc_template ("
        "  id INT UNSIGNED NOT NULL, name VARCHAR(80) NOT NULL, vendor_ref_id INT UNSIGNED NULL,"
        "  PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE quest_template ("
        "  id INT UNSIGNED NOT NULL, giver_npc_id INT UNSIGNED NOT NULL,"
        "  turn_in_npc_id INT UNSIGNED NULL, PRIMARY KEY (id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE npc_trainer (npc_id INT UNSIGNED NOT NULL, PRIMARY KEY (npc_id))"
        " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    c.execute(
        "CREATE TABLE npc_trainer_ability ("
        "  npc_id INT UNSIGNED NOT NULL, ability_id INT UNSIGNED NOT NULL,"
        "  cost_copper BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "  required_class ENUM('vanguard','runcaller','warden','mender') NULL,"
        "  required_level SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
        "  PRIMARY KEY (npc_id, ability_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

void drop_world_tables(db::Connection& c) {
    const char* drops[] = {
        "DROP TABLE IF EXISTS npc_trainer_ability", "DROP TABLE IF EXISTS npc_trainer",
        "DROP TABLE IF EXISTS quest_template",      "DROP TABLE IF EXISTS npc_template",
    };
    for (const char* d : drops) c.execute(d);
}

// Seed the authored trainer: one npc_template row + npc_trainer marker + four taught
// abilities (one per rejection axis + one learnable).
void seed_trainer(db::Connection& c) {
    c.execute("INSERT INTO npc_template (id, name, vendor_ref_id) VALUES (?, ?, NULL)",
              {sid(kTrainerNpc), db::Param{std::string("Ridgewatch Trainer")}});
    c.execute("INSERT INTO npc_trainer (npc_id) VALUES (?)", {sid(kTrainerNpc)});
    auto abl = [&](std::uint32_t ability, std::int64_t cost, db::Param cls, std::int64_t level) {
        c.execute(
            "INSERT INTO npc_trainer_ability (npc_id, ability_id, cost_copper, required_class, "
            "required_level) VALUES (?, ?, ?, ?, ?)",
            {sid(kTrainerNpc), sid(ability), db::Param{cost}, cls, db::Param{level}});
    };
    abl(kAblAny, kAnyCost, db::Param{std::monostate{}}, 3);   // any class
    abl(kAblVanguard, 250, db::Param{std::string("vanguard")}, 5);
    abl(kAblHighLvl, 50, db::Param{std::monostate{}}, 10);
    abl(kAblPricey, 5000, db::Param{std::monostate{}}, 1);
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

// Locate a TrainerList entry's state for `ability_id` (or a sentinel if absent).
std::optional<mn::TrainableState> state_of(const mn::TrainerList* m, std::uint32_t ability_id) {
    if (!m || !m->entries()) return std::nullopt;
    for (const auto* e : *m->entries())
        if (e->ability_id() == ability_id) return e->state();
    return std::nullopt;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd trainer-vs-DB-content LIVE-SESSION test (#392)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — trainer/DB checks skipped\n");
        return 0;
    }

    char tmpl[] = "/tmp/meridian-trainer-db-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    const int salt = std::rand();
    const std::string name = "Trn_" + std::to_string(salt);
    std::uint32_t realm_id = 0;
    std::uint64_t grant_ok = 0;
    std::uint64_t account_id = 0;
    std::uint64_t char_id = 0;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);

        // Seed the authored trainer into world content tables, then build the DB store.
        create_world_tables(db);
        seed_trainer(db);
        mw::DbNpcStore npc_store(db);
        check("DbNpcStore loaded the authored trainer",
              npc_store.find(kTrainerNpc) != nullptr &&
                  npc_store.find(kTrainerNpc)->is_trainer &&
                  npc_store.find(kTrainerNpc)->trainer_abilities.size() == 4);
        // Install the DB-backed NPC seam (item/vendor/quest stay on their placeholders —
        // the trainer path reads only the NPC store + the character DB).
        mw::install_content_stores(nullptr, nullptr, nullptr, &npc_store);

        // Seed account + realm + grant.
        db.execute("DELETE FROM `character` WHERE name = ?", {db::Param{name}});
        const std::string username = "worldd_trn_" + std::to_string(salt);
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute("INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
                   {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        account_id = cell_u64(db.execute("SELECT id FROM account WHERE username = ?",
                                         {db::Param{username}}).rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        const std::string realm_name = "Trn Realm " + std::to_string(salt);
        db.execute("INSERT INTO realm (name, address, port, build_min, build_max) "
                   "VALUES (?, '127.0.0.1', 7200, 0, 100000)", {db::Param{realm_name}});
        realm_id = static_cast<std::uint32_t>(
            cell_u64(db.execute("SELECT id FROM realm WHERE name = ?",
                                {db::Param{realm_name}}).rows.at(0)[0]));
        check("test realm seeded", realm_id > 0);

        auto rand_u64 = [] {
            std::uint64_t v = (static_cast<std::uint64_t>(std::rand()) << 32) ^
                              static_cast<std::uint64_t>(std::rand());
            return v == 0 ? 1 : v;
        };
        grant_ok = rand_u64();
        seed_grant(db, grant_ok, account_id, realm_id, Bytes(32, 0xAB), client_build);

        // A Runcaller (class 2) at level 5 with 300 copper: eligible for the any-class
        // ability (level 3, 100c) but WRONG_CLASS for the Vanguard-only one, TOO-LOW for
        // the level-10 one, and can't afford the 5000c one.
        chr::CreateRequest cr;
        cr.account_id = account_id;
        cr.name = name;
        cr.race = static_cast<std::uint8_t>(chr::Race::kSylvane);
        cr.char_class = static_cast<std::uint8_t>(chr::Class::kRuncaller);
        char_id = chr::create_character(db, cr).character_id;
        check("character created", char_id > 0);
        db.execute("UPDATE `character` SET level = 5, money = ? WHERE id = ?",
                   {db::Param{kStartMoney}, sid(char_id)});
        check("character starts at 300 copper", money_of(db, char_id) == kStartMoney);

        // Stand up the real listener + serve loop with auth + char DB.
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
            bool hs = false;
            if (std::optional<RxFrame> rf = recv_decoded(c))
                hs = rf->opcode == mn::Opcode::HANDSHAKE_OK;
            check("handshake ok", hs);

            if (auto pl = round_trip(c, mn::Opcode::ENTER_WORLD_REQUEST,
                                     enc_enter_world_request(char_id),
                                     mn::Opcode::ENTER_WORLD_RESPONSE, seq++)) {
                const auto* m = decode<mn::EnterWorldResponse>(*pl);
                check("ENTER_WORLD -> OK", m && m->status() == mn::EnterWorldStatus::OK);
            } else {
                check("got an EnterWorldResponse", false);
            }

            // --- GOSSIP on the DB-loaded trainer -> TRAINER option + a pushed list ---
            if (auto pl = round_trip(c, mn::Opcode::GOSSIP_HELLO, enc_gossip_hello(kTrainerNpc),
                                     mn::Opcode::GOSSIP_MENU, seq++)) {
                const auto* m = decode<mn::GossipMenu>(*pl);
                bool trainer = false;
                if (m && m->options())
                    for (const auto* o : *m->options())
                        if (o->kind() == mn::GossipOptionKind::TRAINER) trainer = true;
                check("gossip on the DB trainer shows a TRAINER option", m && trainer);
            } else {
                check("got a GossipMenu (trainer)", false);
            }
            if (std::optional<RxFrame> rf = recv_decoded(c)) {  // the pushed TrainerList
                const mn::TrainerList* m =
                    rf->opcode == mn::Opcode::TRAINER_LIST ? decode<mn::TrainerList>(rf->payload)
                                                           : nullptr;
                auto st = [&](std::uint32_t abl) { return state_of(m, abl); };
                check("TrainerList: any-class ability LEARNABLE",
                      st(kAblAny) == mn::TrainableState::LEARNABLE);
                check("TrainerList: Vanguard-only ability WRONG_CLASS for the Runcaller",
                      st(kAblVanguard) == mn::TrainableState::WRONG_CLASS);
                check("TrainerList: level-10 ability LEVEL_TOO_LOW for the level-5 char",
                      st(kAblHighLvl) == mn::TrainableState::LEVEL_TOO_LOW);
                check("TrainerList: 5000c ability CANT_AFFORD at 300 copper",
                      st(kAblPricey) == mn::TrainableState::CANT_AFFORD);
            } else {
                check("got a TrainerList", false);
            }

            // --- Rejections leave money untouched --------------------------------
            auto learn = [&](std::uint32_t abl) -> const mn::TrainerLearnResult* {
                auto pl = round_trip(c, mn::Opcode::TRAINER_LEARN, enc_trainer_learn(kTrainerNpc, abl),
                                     mn::Opcode::TRAINER_LEARN_RESULT, seq++);
                static Bytes hold;
                if (!pl) return nullptr;
                hold = *pl;
                return decode<mn::TrainerLearnResult>(hold);
            };

            {
                const auto* m = learn(kAblVanguard);
                check("learn Vanguard ability -> WRONG_CLASS",
                      m && m->status() == mn::TrainerLearnStatus::WRONG_CLASS);
                check("wrong-class: money unchanged at 300", money_of(db, char_id) == kStartMoney);
            }
            {
                const auto* m = learn(kAblHighLvl);
                check("learn level-10 ability -> LEVEL_TOO_LOW",
                      m && m->status() == mn::TrainerLearnStatus::LEVEL_TOO_LOW);
                check("too-low-level: money unchanged at 300", money_of(db, char_id) == kStartMoney);
            }
            {
                const auto* m = learn(kAblPricey);
                check("learn 5000c ability -> INSUFFICIENT_FUNDS",
                      m && m->status() == mn::TrainerLearnStatus::INSUFFICIENT_FUNDS);
                check("insufficient-funds: money unchanged at 300",
                      money_of(db, char_id) == kStartMoney);
            }

            // --- Valid learn debits the cost over the wire -----------------------
            {
                const auto* m = learn(kAblAny);
                check("learn any-class ability -> OK, cost 100 debited",
                      m && m->status() == mn::TrainerLearnStatus::OK && m->cost() == kAnyCost &&
                          m->new_balance() == kStartMoney - kAnyCost);
                check("trainer cost debited character.money (-100)",
                      money_of(db, char_id) == kStartMoney - kAnyCost);
            }
            {
                const auto* m = learn(kAblAny);
                check("re-learn -> ALREADY_KNOWN, nothing debited",
                      m && m->status() == mn::TrainerLearnStatus::ALREADY_KNOWN);
                check("re-learn left character.money untouched",
                      money_of(db, char_id) == kStartMoney - kAnyCost);
            }
        }  // client closes

        server.join();
        world.stop();

        // Restore the placeholder seam so a later in-process store doesn't dangle.
        mw::install_content_stores(nullptr, nullptr, nullptr, nullptr);

        // Cleanup.
        db.execute("DELETE FROM `character` WHERE id = ?", {sid(char_id)});
        db.execute("DELETE FROM session_grant WHERE account_id = ?", {sid(account_id)});
        db.execute("DELETE FROM realm WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(realm_id)}});
        db.execute("DELETE FROM account WHERE id = ?", {sid(account_id)});
        drop_world_tables(db);
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

    std::printf(g_fail == 0 ? "\nALL TRAINER/DB WIRE TESTS PASSED\n"
                            : "\n%d TRAINER/DB WIRE TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
