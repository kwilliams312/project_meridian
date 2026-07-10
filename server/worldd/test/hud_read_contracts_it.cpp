// SPDX-License-Identifier: Apache-2.0
//
// worldd — HUD read-contracts LIVE-SESSION integration test (issue #453, epic #24).
// Proves the two server->client READ contracts the HUD needs, end-to-end over a REAL
// TLS 1.3 session against a REAL MariaDB, using the M1 PLACEHOLDER stores (the same
// vendor NPC + vendor catalog + item templates worldd falls back to when no world
// content DB is installed):
//
//   GAP 1 — inventory snapshot (INVENTORY_SNAPSHOT, ITM-01): the character's backpack
//   contents + money, pushed at ENTER_WORLD and re-pushed after every server-
//   authoritative inventory change (here: a vendor buy / sell / buyback). Before #453
//   the client HUD bags window (#452) had no item data and money was "unknown" until
//   the first transaction.
//
//   GAP 2 — vendor catalog (VENDOR_LIST, ECO-01): the vendor's for-sale list with
//   SERVER-COMPUTED prices, auto-pushed on GOSSIP_HELLO to a vendor NPC (mirroring
//   how TRAINER_LIST auto-pushes to a trainer). Before #453 the buy list was an
//   item-template-id greybox — the server validated buys against a catalog it never
//   sent.
//
// Plus the minor #453 items: money rides the ENTER_WORLD snapshot, and
// VENDOR_BUYBACK_RESULT echoes the buyback_slot the client asked for.
//
// CLEAN-ROOM: written from issue #453, the server SAD, the world.fbs wire contract,
// the meridian-items / meridian-vendor / meridian-npc public APIs, and the OpenSSL
// public API only. No GPL source consulted (CONTRIBUTING.md). The TLS/cert/Client/
// seed scaffolding mirrors quest_loot_npc_db_it (same clean-room helpers).
//
// DB-GATED: needs a live MariaDB with the auth schema (session_grant / account /
// realm). It CREATE-TABLE-IF-NOT-EXISTS the `character` / item_instance /
// character_inventory tables (mirroring 0001_init_characters.up.sql) so the same DB
// serves as the characters DB. Reads MERIDIAN_DB_* env and SKIPS (exit 0) when none
// are set — inert in the plain server ctest, real only in the worldd-session CI job
// (or locally via scripts/dev/test.sh --db).

#include "world_dispatch.h"
#include "world_session.h"

#include "characters.h"
#include "item_template.h"     // kPlaceholderIdBase + placeholder template prices
#include "npc_def.h"           // kNpcVendor placeholder id
#include "vendor_catalog.h"    // kPlaceholderGeneralVendor
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
#include <unordered_map>
#include <vector>

using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mw = meridian::worldd;
namespace chr = meridian::characters;
namespace npc = meridian::npc;
namespace vend = meridian::vendor;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* env(const char* k) { return std::getenv(k); }

using Bytes = std::vector<std::uint8_t>;

// M1 placeholder content (mirror the server-side placeholder stores).
constexpr std::uint64_t kVendorNpc   = npc::kNpcVendor;                 // the vendor NPC guid
constexpr std::uint32_t kVendorId    = vend::kPlaceholderGeneralVendor; // its catalog id (990001)
constexpr std::uint32_t kShortsword  = items::kPlaceholderIdBase + 1;   // buy 100 (template price)
constexpr std::uint32_t kOre         = items::kPlaceholderIdBase + 8;   // buy 6 (catalog override)
constexpr std::uint32_t kPotion      = items::kPlaceholderIdBase + 7;   // buy 5, sell 1, stackable
constexpr std::int64_t  kStartMoney  = 1000;
constexpr std::uint32_t kBuyQty      = 3;

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
            reinterpret_cast<const unsigned char*>("meridian-hud-rc-test"), -1, -1, 0);
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
Bytes enc_vendor_buy(std::uint32_t vendor_id, std::uint32_t template_id, std::uint32_t qty) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuyRequest(b, vendor_id, template_id, qty));
    return bytes_of(b);
}
Bytes enc_vendor_sell(std::uint32_t vendor_id, std::uint16_t backpack_slot, std::uint32_t qty) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorSellRequest(b, vendor_id, backpack_slot, qty));
    return bytes_of(b);
}
Bytes enc_vendor_buyback(std::uint16_t buyback_slot) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuybackRequest(b, buyback_slot));
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
// GOSSIP_MENU ahead of VENDOR_LIST, a reply ahead of a trailing snapshot) — a
// straggler is simply skipped until the awaited opcode surfaces.
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
    "  PRIMARY KEY (id),"
    "  UNIQUE KEY uq_inventory_slot (char_id, bag, slot),"
    "  UNIQUE KEY uq_inventory_item (item_guid),"
    "  KEY idx_inventory_char (char_id),"
    "  CONSTRAINT fk_inventory_item FOREIGN KEY (item_guid)"
    "    REFERENCES item_instance (item_guid) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd HUD read-contracts LIVE-SESSION DB test (#453)\n");

    db::ConnectParams p;
    bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    p.database = env("MERIDIAN_DB_NAME") ? env("MERIDIAN_DB_NAME") : "";

    if (!configured || p.user.empty()) {
        std::printf("SKIP: no MERIDIAN_DB_* connection configured — HUD read-contracts "
                    "checks skipped\n");
        return 0;
    }

    char tmpl[] = "/tmp/meridian-hud-rc-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    const std::uint32_t client_build = 1000;
    const int salt = std::rand();
    const std::string name = "Hud_" + std::to_string(salt);

    try {
        db::Connection db(p);
        std::printf("connected to MariaDB\n");
        db.execute(kCharacterDdl);
        db.execute(kItemInstanceDdl);
        db.execute(kInventoryDdl);
        db.execute("DELETE FROM `character` WHERE name = ?", {db::Param{name}});

        const std::string username = "worldd_hud_" + std::to_string(salt);
        db.execute("DELETE FROM account WHERE username = ?", {db::Param{username}});
        db.execute("INSERT INTO account (username, srp_salt, srp_verifier) VALUES (?, ?, ?)",
                   {db::Param{username}, blob32(Bytes(32, 0x11)), blob32(Bytes(32, 0x22))});
        std::uint64_t account_id = cell_u64(
            db.execute("SELECT id FROM account WHERE username = ?",
                       {db::Param{username}}).rows.at(0)[0]);
        check("test account seeded", account_id > 0);

        const std::string realm_name = "Hud Realm " + std::to_string(salt);
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

        chr::CreateRequest cr;
        cr.account_id = account_id;
        cr.name = name;
        cr.race = static_cast<std::uint8_t>(chr::Race::kSylvane);
        cr.char_class = static_cast<std::uint8_t>(chr::Class::kRuncaller);
        std::uint64_t char_id = chr::create_character(db, cr).character_id;
        check("character created", char_id > 0);
        db.execute("UPDATE `character` SET money = ? WHERE id = ?",
                   {db::Param{static_cast<std::int64_t>(kStartMoney)}, sid(char_id)});

        // Stand up the real listener + serve loop with auth + char DB (placeholder
        // content stores — no install_content_stores, so the vendor NPC + catalog +
        // item templates are the M1 placeholders).
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

            // --- ENTER_WORLD -> OK, then the enter-world INVENTORY_SNAPSHOT --------
            c.send_frame(mw::encode_frame(mn::Opcode::ENTER_WORLD_REQUEST, seq++,
                                          enc_enter_world_request(char_id)));
            if (auto pl = read_until(c, mn::Opcode::ENTER_WORLD_RESPONSE)) {
                const auto* m = decode<mn::EnterWorldResponse>(*pl);
                check("ENTER_WORLD -> OK", m && m->status() == mn::EnterWorldStatus::OK);
            } else {
                check("got an EnterWorldResponse", false);
            }
            if (auto pl = read_until(c, mn::Opcode::INVENTORY_SNAPSHOT)) {
                const auto* m = decode<mn::InventorySnapshot>(*pl);
                check("ENTER_WORLD pushes INVENTORY_SNAPSHOT", m != nullptr);
                if (m) {
                    check("snapshot carries the character's money", m->money() == kStartMoney);
                    check("snapshot reports the backpack capacity", m->backpack_slots() > 0);
                    check("a fresh character's backpack is empty",
                          m->items() == nullptr || m->items()->size() == 0);
                }
            } else {
                check("got an enter-world INVENTORY_SNAPSHOT", false);
            }

            // --- GAP 2: GOSSIP_HELLO to the vendor NPC -> VENDOR_LIST -------------
            c.send_frame(mw::encode_frame(mn::Opcode::GOSSIP_HELLO, seq++,
                                          enc_gossip_hello(kVendorNpc)));
            if (auto pl = read_until(c, mn::Opcode::VENDOR_LIST)) {
                const auto* m = decode<mn::VendorList>(*pl);
                check("vendor gossip auto-pushes VENDOR_LIST", m != nullptr);
                if (m) {
                    check("VENDOR_LIST carries the catalog vendor id", m->vendor_id() == kVendorId);
                    std::unordered_map<std::uint32_t, std::int64_t> price;
                    if (m->items())
                        for (const auto* it : *m->items()) price[it->item_template_id()] = it->price();
                    check("catalog is non-empty", !price.empty());
                    // Server-computed prices: a template-priced item + a catalog override.
                    check("shortsword price == template buy_price (100)",
                          price.count(kShortsword) && price[kShortsword] == 100);
                    check("ore price == catalog override (6, no template buy_price)",
                          price.count(kOre) && price[kOre] == 6);
                    check("potion is listed at its template buy_price (5)",
                          price.count(kPotion) && price[kPotion] == 5);
                }
            } else {
                check("got a VENDOR_LIST", false);
            }

            // --- GAP 1: VENDOR_BUY -> result + updated INVENTORY_SNAPSHOT ---------
            const std::int64_t after_buy = kStartMoney - 5 * static_cast<std::int64_t>(kBuyQty);
            c.send_frame(mw::encode_frame(mn::Opcode::VENDOR_BUY_REQUEST, seq++,
                                          enc_vendor_buy(kVendorId, kPotion, kBuyQty)));
            if (auto pl = read_until(c, mn::Opcode::VENDOR_BUY_RESULT)) {
                const auto* m = decode<mn::VendorBuyResult>(*pl);
                check("VENDOR_BUY -> OK", m && m->status() == mn::VendorBuyStatus::OK);
                check("buy debits the server-computed price", m && m->balance() == after_buy);
            } else {
                check("got a VendorBuyResult", false);
            }
            std::uint16_t potion_slot = 0;
            if (auto pl = read_until(c, mn::Opcode::INVENTORY_SNAPSHOT)) {
                const auto* m = decode<mn::InventorySnapshot>(*pl);
                check("VENDOR_BUY re-pushes INVENTORY_SNAPSHOT", m != nullptr);
                if (m) {
                    check("snapshot money reflects the purchase", m->money() == after_buy);
                    bool found = false;
                    if (m->items())
                        for (const auto* it : *m->items())
                            if (it->item_template_id() == kPotion && it->count() == kBuyQty) {
                                found = true;
                                potion_slot = it->slot();
                            }
                    check("the bought stack appears in the bags snapshot", found);
                }
            } else {
                check("got a post-buy INVENTORY_SNAPSHOT", false);
            }

            // --- VENDOR_SELL the stack back -> result (buyback_slot) + snapshot ---
            std::uint16_t sold_buyback_slot = 0xFFFF;
            c.send_frame(mw::encode_frame(mn::Opcode::VENDOR_SELL_REQUEST, seq++,
                                          enc_vendor_sell(kVendorId, potion_slot, kBuyQty)));
            if (auto pl = read_until(c, mn::Opcode::VENDOR_SELL_RESULT)) {
                const auto* m = decode<mn::VendorSellResult>(*pl);
                check("VENDOR_SELL -> OK", m && m->status() == mn::VendorSellStatus::OK);
                if (m) sold_buyback_slot = m->buyback_slot();
            } else {
                check("got a VendorSellResult", false);
            }
            if (auto pl = read_until(c, mn::Opcode::INVENTORY_SNAPSHOT)) {
                const auto* m = decode<mn::InventorySnapshot>(*pl);
                check("VENDOR_SELL re-pushes INVENTORY_SNAPSHOT", m != nullptr);
                if (m) {
                    bool has_potion = false;
                    if (m->items())
                        for (const auto* it : *m->items())
                            if (it->item_template_id() == kPotion) has_potion = true;
                    check("the sold stack left the bags snapshot", !has_potion);
                }
            } else {
                check("got a post-sell INVENTORY_SNAPSHOT", false);
            }

            // --- VENDOR_BUYBACK -> result echoes buyback_slot + snapshot ----------
            c.send_frame(mw::encode_frame(mn::Opcode::VENDOR_BUYBACK_REQUEST, seq++,
                                          enc_vendor_buyback(sold_buyback_slot)));
            if (auto pl = read_until(c, mn::Opcode::VENDOR_BUYBACK_RESULT)) {
                const auto* m = decode<mn::VendorBuybackResult>(*pl);
                check("VENDOR_BUYBACK -> OK", m && m->status() == mn::VendorBuybackStatus::OK);
                check("VENDOR_BUYBACK_RESULT echoes the requested buyback_slot",
                      m && m->buyback_slot() == sold_buyback_slot);
            } else {
                check("got a VendorBuybackResult", false);
            }
            if (auto pl = read_until(c, mn::Opcode::INVENTORY_SNAPSHOT)) {
                const auto* m = decode<mn::InventorySnapshot>(*pl);
                check("VENDOR_BUYBACK re-pushes INVENTORY_SNAPSHOT", m != nullptr);
                if (m) {
                    bool back = false;
                    if (m->items())
                        for (const auto* it : *m->items())
                            if (it->item_template_id() == kPotion && it->count() == kBuyQty)
                                back = true;
                    check("the repurchased stack is back in the bags snapshot", back);
                }
            } else {
                check("got a post-buyback INVENTORY_SNAPSHOT", false);
            }
        }

        server.join();  // the client block closed above -> serve_connection returned
        world.stop();

        // Cleanup: the character's items (JOIN so the FK cascade clears placements),
        // then the character, grant, realm, account.
        db.execute("DELETE ii FROM item_instance ii "
                   "JOIN character_inventory ci ON ci.item_guid = ii.item_guid "
                   "WHERE ci.char_id = ?", {sid(char_id)});
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

    std::printf(g_fail == 0 ? "\nALL HUD READ-CONTRACT CHECKS PASSED\n"
                            : "\n%d HUD READ-CONTRACT CHECK(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
