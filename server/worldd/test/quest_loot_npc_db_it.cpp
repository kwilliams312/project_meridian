// SPDX-License-Identifier: Apache-2.0
//
// worldd — quest / loot / gossip / trainer LIVE-SESSION DB integration test (issue
// #388, epic #20). The acceptance bar for #388 is the LIVE dispatch path with
// DURABLE side effects: it drives QST-01 / ITM-02 / NPC-01/02 opcodes over a REAL
// TLS 1.3 session against a REAL MariaDB and asserts the character's DB state
// (inventory rows + character.money) changes exactly as the wire contract promises.
//
// CLEAN-ROOM: written from the server SAD, the world.fbs wire contract, the
// meridian-characters / meridian-items public APIs, and the OpenSSL public API
// only. No GPL source consulted (CONTRIBUTING.md).
//
// DB-GATED: needs a live MariaDB with the auth schema (session_grant / account /
// realm). It CREATE-TABLE-IF-NOT-EXISTS the `character` / item_instance /
// character_inventory tables (mirroring 0001_init_characters.up.sql) so the same
// DB serves as the characters DB. Reads MERIDIAN_DB_* env and SKIPS (exit 0) when
// none are set — inert in the plain server ctest, real only in the worldd-session
// CI job (or locally via scripts/dev/test.sh --db).
//
// What it proves end-to-end over one in-world session:
//   * out-of-phase (char-select) QUEST_ACCEPT / LOOT_REQUEST -> typed rejects;
//   * ENTER_WORLD pushes the initial QuestLog;
//   * a foreign-owned corpse -> NOT_A_LOOTER (wrong-char / ownership gate);
//   * accept a COLLECT quest, loot 5 ore from a corpse -> the ore rows land in the
//     character's DB inventory, QUEST_PROGRESS marks the objective complete;
//   * loot the corpse money -> character.money credited over the wire;
//   * turn the quest in -> reward copper credited (character.money) over the wire;
//   * learn a trainer ability -> the cost is DEBITED from character.money; a
//     re-learn is ALREADY_KNOWN and leaves money untouched.

#include "world_dispatch.h"
#include "world_session.h"

#include "characters.h"
#include "item_template.h"     // Copper Ore template id
#include "loot_roll.h"
#include "loot_session.h"
#include "npc_def.h"           // placeholder NPC / ability ids
#include "quest_def.h"         // placeholder quest ids
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
namespace lo = meridian::loot;
namespace npc = meridian::npc;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

using Bytes = std::vector<std::uint8_t>;

// M1 placeholder content ids (mirror the server-side placeholder stores).
constexpr std::uint32_t kQ2        = mw::kPlaceholderQuestIdBase + 2;  // "Ore for the Smith" (collect 5)
constexpr std::uint32_t kSmithNpc  = mw::kPlaceholderNpcIdBase + 2;    // Q2 giver/turn-in AND the trainer
constexpr std::uint32_t kCopperOre = items::kPlaceholderIdBase + 8;
constexpr std::uint64_t kCorpse    = 0xC0FFEEULL;
constexpr std::uint64_t kForeign   = 0xBADCAFEULL;
constexpr std::int64_t  kStartMoney = 1000;

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
            reinterpret_cast<const unsigned char*>("meridian-qln-db-test"), -1, -1, 0);
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
Bytes enc_quest_accept(std::uint32_t quest_id, std::uint64_t giver) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestAccept(b, quest_id, giver));
    return bytes_of(b);
}
Bytes enc_quest_turn_in(std::uint32_t quest_id, std::uint64_t turn_in, std::int32_t choice) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestTurnIn(b, quest_id, turn_in, choice));
    return bytes_of(b);
}
Bytes enc_loot_request(std::uint64_t corpse) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootRequest(b, corpse));
    return bytes_of(b);
}
Bytes enc_loot_take(std::uint64_t corpse, std::uint32_t slot, bool money) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootTake(b, corpse, slot, money));
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

// A frame read + decoded to (opcode, payload copy).
struct RxFrame {
    mn::Opcode opcode{};
    Bytes payload;
};
std::optional<RxFrame> recv_decoded(Client& c) {
    // Skip the unsolicited self VITALS_UPDATE the server now pushes at ENTER_WORLD
    // (#439: the HUD player-frame snapshot) so round_trip reaches the awaited reply.
    // These quest/loot/npc round-trips never await a VITALS_UPDATE, so dropping it
    // is safe; the vitals broadcast logic is proven by worldd-vitals-test.
    for (;;) {
        std::optional<Bytes> raw = c.recv_frame();
        if (!raw) return std::nullopt;
        std::optional<mw::Frame> rf = mw::decode_frame(*raw);
        if (!rf) return std::nullopt;
        if (rf->opcode == mn::Opcode::VITALS_UPDATE) continue;

        if (rf->opcode == mn::Opcode::INVENTORY_SNAPSHOT) continue;  // #453 unsolicited bags snapshot
        if (rf->opcode == mn::Opcode::KNOWN_ABILITIES) continue;     // #457 unsolicited spellbook
        return RxFrame{rf->opcode, Bytes(rf->payload, rf->payload + rf->payload_len)};
    }
}

// Send `req` under `opcode`, read one reply, validate its opcode, return payload.
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
std::uint64_t ore_count(db::Connection& db, std::uint64_t char_id) {
    db::Result r = db.execute(
        "SELECT COALESCE(SUM(ii.stack),0) FROM item_instance ii "
        "JOIN character_inventory ci ON ci.item_guid = ii.item_guid "
        "WHERE ci.char_id = ? AND ii.item_template_id = ?",
        {sid(char_id), db::Param{static_cast<std::int64_t>(kCopperOre)}});
    return r.rows.empty() ? 0 : cell_u64(r.rows.at(0)[0]);
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
constexpr const char* kItemInstanceDdl =
    "CREATE TABLE IF NOT EXISTS item_instance ("
    "  item_guid BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  item_template_id INT UNSIGNED NOT NULL,"
    "  stack INT UNSIGNED NOT NULL DEFAULT 1,"
    "  durability INT UNSIGNED NOT NULL DEFAULT 0,"
    "  suffix_id INT UNSIGNED NOT NULL DEFAULT 0,"
    "  creator BIGINT UNSIGNED NULL,"
    "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (item_guid),"
    "  KEY idx_item_instance_template (item_template_id),"
    "  KEY idx_item_instance_creator (creator),"
    "  CONSTRAINT fk_item_instance_creator FOREIGN KEY (creator)"
    "    REFERENCES `character` (id) ON DELETE SET NULL"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";
constexpr const char* kInventoryDdl =
    "CREATE TABLE IF NOT EXISTS character_inventory ("
    "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "  char_id BIGINT UNSIGNED NOT NULL,"
    "  bag TINYINT UNSIGNED NOT NULL,"
    "  slot SMALLINT UNSIGNED NOT NULL,"
    "  item_guid BIGINT UNSIGNED NOT NULL,"
    "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_character_inventory_slot (char_id, bag, slot),"
    "  UNIQUE KEY uq_character_inventory_item (item_guid),"
    "  KEY idx_character_inventory_char (char_id),"
    "  CONSTRAINT fk_character_inventory_char FOREIGN KEY (char_id)"
    "    REFERENCES `character` (id) ON DELETE CASCADE,"
    "  CONSTRAINT fk_character_inventory_item FOREIGN KEY (item_guid)"
    "    REFERENCES item_instance (item_guid) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

// Seed a corpse loot session (5 Copper Ore common stack + 50 copper) owned by
// `owner`, at the bootstrap spawn point (in loot range of an in-world character).
void seed_corpse(mw::WorldServer& world, std::uint64_t corpse, std::uint64_t owner) {
    lo::LootRoll roll;
    roll.stacks.push_back(lo::LootStack{kCopperOre, /*count=*/5, /*required_quest_id=*/0});
    roll.copper = 50;
    const lo::LootPoint pos{-320.0f, -320.0f, 0.0f};  // == ENTER_WORLD spawn (kZoneSpawnXY, #562)
    world.loot_registry().insert(
        lo::LootSession(corpse, pos, std::move(roll), {owner}, /*loot_range=*/5.0f));
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd quest/loot/gossip/trainer LIVE-SESSION DB test (#388)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — quest/loot/npc DB "
                    "checks skipped\n");
        return 0;
    }

    char tmpl[] = "/tmp/meridian-qln-db-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    const int salt = std::rand();
    const std::string name = "Qln_" + std::to_string(salt);
    std::uint32_t realm_id = 0;
    std::uint64_t grant_ok = 0;
    std::uint64_t account_id = 0;
    std::uint64_t char_id = 0;

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);
        db.execute(kItemInstanceDdl);
        db.execute(kInventoryDdl);
        db.execute("DELETE FROM `character` WHERE name = ?", {db::Param{name}});

        // Seed account + realm + grant.
        const std::string username = "worldd_qln_" + std::to_string(salt);
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute("INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
                   {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        account_id = cell_u64(db.execute("SELECT id FROM account WHERE username = ?",
                                         {db::Param{username}}).rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        const std::string realm_name = "Qln Realm " + std::to_string(salt);
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

        // Create the session's character (a NON-Vanguard class so the trainer's
        // Vanguard-only strike is WRONG_CLASS), then set it to level 5 + start money
        // (so the level-5 trainer heal is affordable + learnable).
        chr::CreateRequest cr;
        cr.account_id = account_id;
        cr.name = name;
        cr.race = static_cast<std::uint8_t>(chr::kRaceSylvane);
        cr.char_class = static_cast<std::uint8_t>(chr::kClassRuncaller);
        char_id = chr::create_character(db, cr).character_id;
        check("character created", char_id > 0);
        db.execute("UPDATE `character` SET level = 5, money = ? WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(kStartMoney)}, sid(char_id)});
        check("character starts at the expected money", money_of(db, char_id) == kStartMoney);

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

        // Seed the corpses: one owned by our character, one owned by a stranger.
        seed_corpse(world, kCorpse, char_id);
        seed_corpse(world, kForeign, /*owner=*/424242);

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

            // Handshake -> HandshakeOk (authenticated, at character-select).
            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, seq++,
                                          enc_world_hello(grant_ok, client_build)));
            bool hs = false;
            if (std::optional<RxFrame> rf = recv_decoded(c))
                hs = rf->opcode == mn::Opcode::HANDSHAKE_OK;
            check("handshake ok", hs);

            // --- OUT-OF-PHASE (char-select): typed rejects, no crash -----------
            if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                     enc_quest_accept(kQ2, kSmithNpc),
                                     mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                const auto* m = decode<mn::QuestAcceptResult>(*pl);
                check("char-select QUEST_ACCEPT -> UNKNOWN_QUEST (out-of-phase)",
                      m && m->status() == mn::QuestAcceptStatus::UNKNOWN_QUEST);
            } else {
                check("got a QuestAcceptResult (char-select)", false);
            }
            if (auto pl = round_trip(c, mn::Opcode::LOOT_REQUEST, enc_loot_request(kCorpse),
                                     mn::Opcode::LOOT_RESPONSE, seq++)) {
                const auto* m = decode<mn::LootResponse>(*pl);
                check("char-select LOOT_REQUEST -> NO_SUCH_CORPSE (out-of-phase)",
                      m && m->status() == mn::LootStatus::NO_SUCH_CORPSE);
            } else {
                check("got a LootResponse (char-select)", false);
            }

            // --- ENTER_WORLD -> OK ---------------------------------------------
            if (auto pl = round_trip(c, mn::Opcode::ENTER_WORLD_REQUEST,
                                     enc_enter_world_request(char_id),
                                     mn::Opcode::ENTER_WORLD_RESPONSE, seq++)) {
                const auto* m = decode<mn::EnterWorldResponse>(*pl);
                check("ENTER_WORLD with the owned character -> OK",
                      m && m->status() == mn::EnterWorldStatus::OK);
            } else {
                check("got an EnterWorldResponse", false);
            }

            // --- QUEST_LOG request in-world -> an (empty) snapshot -------------
            {
                fb::FlatBufferBuilder b;
                std::vector<fb::Offset<mn::QuestLogEntry>> none;
                b.Finish(mn::CreateQuestLog(b, b.CreateVector(none)));
                if (auto pl = round_trip(c, mn::Opcode::QUEST_LOG, bytes_of(b),
                                         mn::Opcode::QUEST_LOG, seq++)) {
                    const auto* ql = decode<mn::QuestLog>(*pl);
                    check("QUEST_LOG request in-world returns a snapshot",
                          ql && (ql->quests() == nullptr || ql->quests()->size() == 0));
                } else {
                    check("got a QuestLog snapshot", false);
                }
            }

            // --- Wrong-looter: a corpse owned by another player -> NOT_A_LOOTER -
            if (auto pl = round_trip(c, mn::Opcode::LOOT_REQUEST, enc_loot_request(kForeign),
                                     mn::Opcode::LOOT_RESPONSE, seq++)) {
                const auto* m = decode<mn::LootResponse>(*pl);
                check("loot a foreign-owned corpse -> NOT_A_LOOTER",
                      m && m->status() == mn::LootStatus::NOT_A_LOOTER);
            } else {
                check("got a LootResponse (foreign)", false);
            }

            // --- Accept the collect quest Q2 -> OK -----------------------------
            if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                     enc_quest_accept(kQ2, kSmithNpc),
                                     mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                const auto* m = decode<mn::QuestAcceptResult>(*pl);
                check("accept Q2 (collect) -> OK",
                      m && m->status() == mn::QuestAcceptStatus::OK);
            } else {
                check("got a QuestAcceptResult (Q2)", false);
            }
            if (std::optional<RxFrame> rf = recv_decoded(c))  // the post-accept QuestLog
                check("QuestLog follows the accept", rf->opcode == mn::Opcode::QUEST_LOG);
            else
                check("QuestLog follows the accept", false);

            // --- LOOT_REQUEST on the owned corpse -> OK + lists the ore --------
            if (auto pl = round_trip(c, mn::Opcode::LOOT_REQUEST, enc_loot_request(kCorpse),
                                     mn::Opcode::LOOT_RESPONSE, seq++)) {
                const auto* m = decode<mn::LootResponse>(*pl);
                bool ore = false;
                if (m && m->items())
                    for (const auto* it : *m->items())
                        if (it->item_template_id() == kCopperOre && it->count() == 5) ore = true;
                check("loot the corpse -> OK, lists 5 ore + 50 copper",
                      m && m->status() == mn::LootStatus::OK && m->copper() == 50 && ore);
            } else {
                check("got a LootResponse (owned)", false);
            }

            // --- LOOT_TAKE the ore -> OK; ore lands in DB inventory; progress ---
            if (auto pl = round_trip(c, mn::Opcode::LOOT_TAKE, enc_loot_take(kCorpse, 0, false),
                                     mn::Opcode::LOOT_RESULT, seq++)) {
                const auto* m = decode<mn::LootResult>(*pl);
                check("take the ore -> LOOT_RESULT OK (ore x5)",
                      m && m->status() == mn::LootTakeStatus::OK &&
                          m->item_template_id() == kCopperOre && m->count() == 5);
            } else {
                check("got a LootResult (ore)", false);
            }
            check("the ore is in the character's DB inventory", ore_count(db, char_id) == 5);
            if (std::optional<RxFrame> rf = recv_decoded(c)) {  // QUEST_PROGRESS (collect)
                bool complete = false;
                if (rf->opcode == mn::Opcode::QUEST_PROGRESS) {
                    const auto* qp = decode<mn::QuestProgress>(rf->payload);
                    complete = qp && qp->quest_id() == kQ2 && qp->complete() && qp->have() == 5;
                }
                check("QUEST_PROGRESS marks the collect objective complete", complete);
            } else {
                check("got a QuestProgress", false);
            }

            // --- LOOT_TAKE the money -> OK; character.money credited -----------
            if (auto pl = round_trip(c, mn::Opcode::LOOT_TAKE, enc_loot_take(kCorpse, 0, true),
                                     mn::Opcode::LOOT_RESULT, seq++)) {
                const auto* m = decode<mn::LootResult>(*pl);
                check("take the money -> LOOT_RESULT OK (50 copper)",
                      m && m->status() == mn::LootTakeStatus::OK && m->copper() == 50);
            } else {
                check("got a LootResult (money)", false);
            }
            if (std::optional<RxFrame> rf = recv_decoded(c))  // corpse fully looted -> LOOT_CLOSED
                check("corpse fully looted -> LOOT_CLOSED", rf->opcode == mn::Opcode::LOOT_CLOSED);
            else
                check("got a LootClosed", false);
            check("loot money credited character.money (+50)",
                  money_of(db, char_id) == kStartMoney + 50);

            // --- QUEST_TURN_IN Q2 -> OK + reward copper credited --------------
            if (auto pl = round_trip(c, mn::Opcode::QUEST_TURN_IN,
                                     enc_quest_turn_in(kQ2, kSmithNpc, /*choice=*/0),
                                     mn::Opcode::QUEST_TURN_IN_RESULT, seq++)) {
                const auto* m = decode<mn::QuestTurnInResult>(*pl);
                check("turn in Q2 -> OK, reward_money = 75",
                      m && m->status() == mn::QuestTurnInStatus::OK && m->reward_money() == 75);
            } else {
                check("got a QuestTurnInResult", false);
            }
            if (std::optional<RxFrame> rf = recv_decoded(c))  // the post-turn-in QuestLog
                check("QuestLog follows the turn-in", rf->opcode == mn::Opcode::QUEST_LOG);
            else
                check("QuestLog follows the turn-in", false);
            check("quest reward credited character.money (+75)",
                  money_of(db, char_id) == kStartMoney + 50 + 75);

            // --- GOSSIP on the trainer -> menu + TrainerList (heal LEARNABLE) --
            if (auto pl = round_trip(c, mn::Opcode::GOSSIP_HELLO, enc_gossip_hello(kSmithNpc),
                                     mn::Opcode::GOSSIP_MENU, seq++)) {
                const auto* m = decode<mn::GossipMenu>(*pl);
                bool trainer = false;
                if (m && m->options())
                    for (const auto* o : *m->options())
                        if (o->kind() == mn::GossipOptionKind::TRAINER) trainer = true;
                check("gossip on the trainer shows a TRAINER option", m && trainer);
            } else {
                check("got a GossipMenu (trainer)", false);
            }
            if (std::optional<RxFrame> rf = recv_decoded(c)) {  // pushed TrainerList
                bool heal_learnable = false;
                if (rf->opcode == mn::Opcode::TRAINER_LIST) {
                    const auto* m = decode<mn::TrainerList>(rf->payload);
                    if (m && m->entries())
                        for (const auto* e : *m->entries())
                            if (e->ability_id() == npc::kTrainedHeal &&
                                e->state() == mn::TrainableState::LEARNABLE)
                                heal_learnable = true;
                }
                check("TrainerList shows the level-5 heal as LEARNABLE", heal_learnable);
            } else {
                check("got a TrainerList", false);
            }

            // --- TRAINER_LEARN the heal -> OK; cost DEBITED from money ---------
            if (auto pl = round_trip(c, mn::Opcode::TRAINER_LEARN,
                                     enc_trainer_learn(kSmithNpc, npc::kTrainedHeal),
                                     mn::Opcode::TRAINER_LEARN_RESULT, seq++)) {
                const auto* m = decode<mn::TrainerLearnResult>(*pl);
                check("learn the heal -> OK, cost 120 debited",
                      m && m->status() == mn::TrainerLearnStatus::OK && m->cost() == 120 &&
                          m->new_balance() == kStartMoney + 50 + 75 - 120);
            } else {
                check("got a TrainerLearnResult", false);
            }
            check("trainer cost debited character.money (-120)",
                  money_of(db, char_id) == kStartMoney + 50 + 75 - 120);

            // --- Re-learn -> ALREADY_KNOWN; money UNTOUCHED --------------------
            if (auto pl = round_trip(c, mn::Opcode::TRAINER_LEARN,
                                     enc_trainer_learn(kSmithNpc, npc::kTrainedHeal),
                                     mn::Opcode::TRAINER_LEARN_RESULT, seq++)) {
                const auto* m = decode<mn::TrainerLearnResult>(*pl);
                check("re-learn the heal -> ALREADY_KNOWN, nothing debited",
                      m && m->status() == mn::TrainerLearnStatus::ALREADY_KNOWN);
            } else {
                check("got a TrainerLearnResult (re-learn)", false);
            }
            check("re-learn left character.money untouched",
                  money_of(db, char_id) == kStartMoney + 50 + 75 - 120);
        }  // client closes

        server.join();
        world.stop();

        // Cleanup: the character's items (JOIN so the FK cascade clears placements),
        // then the character, grant, realm, account.
        db.execute("DELETE ii FROM item_instance ii "
                   "JOIN character_inventory ci ON ci.item_guid = ii.item_guid "
                   "WHERE ci.char_id = ?", {sid(char_id)});
        db.execute("DELETE FROM `character` WHERE id = ?", {sid(char_id)});
        db.execute("DELETE FROM session_grant WHERE account_id = ?", {sid(account_id)});
        db.execute("DELETE FROM realm WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(realm_id)}});
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

    std::printf(g_fail == 0 ? "\nALL QUEST/LOOT/NPC DB TESTS PASSED\n"
                            : "\n%d QUEST/LOOT/NPC DB TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
