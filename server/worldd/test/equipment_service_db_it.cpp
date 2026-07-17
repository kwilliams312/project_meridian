// SPDX-License-Identifier: Apache-2.0
// Durable equip/replace/unequip transaction proof (#802). DB-gated like item_store_it.

#include "equipment_service.h"
#include "item_store.h"
#include "world_dispatch.h"

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
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace meridian;
using namespace meridian::worldd;
namespace itm = meridian::items;
namespace fb = flatbuffers;
namespace mn = meridian::net;

namespace {
int fails = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++fails;
}
const char* env(const char* key) { return std::getenv(key); }
db::Param sid(std::uint64_t v) { return db::Param{std::to_string(v)}; }

template <typename E, typename F> bool throws(F&& f) {
    try { f(); } catch (const E&) { return true; } catch (...) {}
    return false;
}

class Store final : public itm::TemplateStore {
public:
    Store() {
        itm::ItemTemplate sword;
        sword.id = 980001; sword.name = "Test sword";
        sword.item_class = itm::ItemClass::kWeapon;
        sword.slot = itm::ItemSlot::kMainHand; sword.required_level = 1;
        sword.max_stack = 1; sword.equip_type_id = 77;
        rows.emplace(sword.id, sword);
        itm::ItemTemplate potion;
        potion.id = 980002; potion.name = "Test potion";
        potion.item_class = itm::ItemClass::kConsumable;
        potion.slot = itm::ItemSlot::kNone; potion.max_stack = 1;
        rows.emplace(potion.id, potion);
    }
    const itm::ItemTemplate* find(std::uint32_t id) const override {
        auto it = rows.find(id); return it == rows.end() ? nullptr : &it->second;
    }
    std::vector<std::uint32_t> ids() const {
        std::vector<std::uint32_t> out; for (const auto& [id, _] : rows) out.push_back(id);
        return out;
    }
private:
    std::map<std::uint32_t, itm::ItemTemplate> rows;
};

constexpr const char* kCharacter =
    "CREATE TABLE IF NOT EXISTS `character` (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "account_id BIGINT UNSIGNED NOT NULL,name VARCHAR(32) NOT NULL,race TINYINT UNSIGNED NOT NULL,"
    "class TINYINT UNSIGNED NOT NULL,level SMALLINT UNSIGNED NOT NULL DEFAULT 1,xp INT UNSIGNED NOT NULL DEFAULT 0,"
    "money BIGINT UNSIGNED NOT NULL DEFAULT 0,map_id INT UNSIGNED NOT NULL,instance_id INT UNSIGNED NOT NULL DEFAULT 0,"
    "pos_x FLOAT NOT NULL,pos_y FLOAT NOT NULL,pos_z FLOAT NOT NULL,pos_o FLOAT NOT NULL DEFAULT 0,"
    "played_time INT UNSIGNED NOT NULL DEFAULT 0,logout_at DATETIME NULL,save_epoch BIGINT NOT NULL DEFAULT 0,"
    "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "PRIMARY KEY(id),UNIQUE KEY uq_character_name(name),KEY idx_character_account(account_id)) ENGINE=InnoDB";
constexpr const char* kInstance =
    "CREATE TABLE IF NOT EXISTS item_instance (item_guid BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "item_template_id INT UNSIGNED NOT NULL,stack INT UNSIGNED NOT NULL DEFAULT 1,durability INT UNSIGNED NOT NULL DEFAULT 0,"
    "suffix_id INT UNSIGNED NOT NULL DEFAULT 0,creator BIGINT UNSIGNED NULL,created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,PRIMARY KEY(item_guid),"
    "KEY idx_item_instance_template(item_template_id),KEY idx_item_instance_creator(creator),"
    "CONSTRAINT fk_item_instance_creator FOREIGN KEY(creator) REFERENCES `character`(id) ON DELETE SET NULL) ENGINE=InnoDB";
constexpr const char* kInventory =
    "CREATE TABLE IF NOT EXISTS character_inventory (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "char_id BIGINT UNSIGNED NOT NULL,bag TINYINT UNSIGNED NOT NULL,slot SMALLINT UNSIGNED NOT NULL,item_guid BIGINT UNSIGNED NOT NULL,"
    "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "PRIMARY KEY(id),UNIQUE KEY uq_character_inventory_slot(char_id,bag,slot),UNIQUE KEY uq_character_inventory_item(item_guid),"
    "KEY idx_character_inventory_char(char_id),CONSTRAINT fk_character_inventory_char FOREIGN KEY(char_id) REFERENCES `character`(id) ON DELETE CASCADE,"
    "CONSTRAINT fk_character_inventory_item FOREIGN KEY(item_guid) REFERENCES item_instance(item_guid) ON DELETE CASCADE) ENGINE=InnoDB";

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
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("meridian-equipment-test"),
            -1, -1, 0);
        if (X509_set_issuer_name(x509, name) != 1) goto done;
    }
    if (X509_sign(x509, pkey, EVP_sha256()) == 0) goto done;
    fk = std::fopen(key_path.c_str(), "wb");
    if (!fk || PEM_write_PrivateKey(fk, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1)
        goto done;
    fc = std::fopen(cert_path.c_str(), "wb");
    if (!fc || PEM_write_X509(fc, x509) != 1) goto done;
    ok = true;
done:
    if (fk) std::fclose(fk);
    if (fc) std::fclose(fc);
    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    if (pctx) EVP_PKEY_CTX_free(pctx);
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
        connected_ = SSL_connect(ssl_) == 1;
    }
    ~Client() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
        if (fd_ >= 0) ::close(fd_);
        if (ctx_) SSL_CTX_free(ctx_);
    }
    bool connected() const { return connected_; }
    bool send_frame(const Bytes& body) {
        const std::uint32_t n = static_cast<std::uint32_t>(body.size());
        Bytes framed{static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8),
                     static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 24)};
        framed.insert(framed.end(), body.begin(), body.end());
        return write_all(framed.data(), framed.size());
    }
    std::optional<Bytes> recv_frame() {
        std::uint8_t len[4];
        if (!read_all(len, sizeof(len))) return std::nullopt;
        const std::uint32_t n = static_cast<std::uint32_t>(len[0]) |
            (static_cast<std::uint32_t>(len[1]) << 8) |
            (static_cast<std::uint32_t>(len[2]) << 16) |
            (static_cast<std::uint32_t>(len[3]) << 24);
        Bytes out(n);
        if (n != 0 && !read_all(out.data(), out.size())) return std::nullopt;
        return out;
    }
private:
    bool write_all(const std::uint8_t* p, std::size_t n) {
        for (std::size_t sent = 0; sent < n;) {
            const int wrote = SSL_write(ssl_, p + sent, static_cast<int>(n - sent));
            if (wrote <= 0) return false;
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }
    bool read_all(std::uint8_t* p, std::size_t n) {
        for (std::size_t got = 0; got < n;) {
            const int read = SSL_read(ssl_, p + got, static_cast<int>(n - got));
            if (read <= 0) return false;
            got += static_cast<std::size_t>(read);
        }
        return true;
    }
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    bool connected_ = false;
};

Bytes equipment_request(mn::EquipmentChangeAction action, std::uint16_t slot) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEquipmentChangeRequest(b, action, slot));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

struct EquipmentReply {
    mn::EquipmentChangeStatus status = mn::EquipmentChangeStatus::INTERNAL;
    bool result_before_snapshot = false;
    std::size_t equipped_count = 0;
};

EquipmentReply receive_equipment_reply(Client& client) {
    EquipmentReply out;
    const auto first = client.recv_frame();
    const auto second = client.recv_frame();
    const auto result_frame = first ? decode_frame(*first) : std::nullopt;
    const auto snapshot_frame = second ? decode_frame(*second) : std::nullopt;
    out.result_before_snapshot = result_frame && snapshot_frame &&
        result_frame->opcode == mn::Opcode::EQUIPMENT_CHANGE_RESULT &&
        snapshot_frame->opcode == mn::Opcode::INVENTORY_SNAPSHOT;
    if (result_frame) {
        fb::Verifier v(result_frame->payload, result_frame->payload_len);
        if (v.VerifyBuffer<mn::EquipmentChangeResult>(nullptr))
            out.status = fb::GetRoot<mn::EquipmentChangeResult>(result_frame->payload)->status();
    }
    if (snapshot_frame) {
        fb::Verifier v(snapshot_frame->payload, snapshot_frame->payload_len);
        if (v.VerifyBuffer<mn::InventorySnapshot>(nullptr)) {
            const auto* snapshot = fb::GetRoot<mn::InventorySnapshot>(snapshot_frame->payload);
            out.equipped_count = snapshot->equipment() ? snapshot->equipment()->size() : 0;
        }
    }
    return out;
}
}  // namespace

int main() {
    Dispatcher dispatcher;
    check("equipment request has a live dispatcher handler",
          dispatcher.has_handler(net::Opcode::EQUIPMENT_CHANGE_REQUEST));

    db::ConnectParams p; bool configured = false;
    if (const char* s = env("MERIDIAN_DB_SOCKET")) { p.unix_socket = s; configured = true; }
    if (const char* h = env("MERIDIAN_DB_HOST")) { p.host = h; configured = true; }
    if (const char* v = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(v));
    if (const char* v = env("MERIDIAN_DB_USER")) p.user = v;
    if (const char* v = env("MERIDIAN_DB_PASS")) p.password = v;
    if (const char* v = env("MERIDIAN_DB_NAME")) p.database = v;
    if (!configured || p.user.empty()) { std::puts("SKIP: no MERIDIAN_DB_* connection"); return 0; }

    db::Connection db(p); db.execute(kCharacter); db.execute(kInstance); db.execute(kInventory);
    const auto salt = static_cast<unsigned>(std::rand());
    const auto account = 8'020'000'000ULL + salt;
    const std::string name = "Eq_" + std::to_string(salt);
    db.execute("DELETE FROM `character` WHERE account_id=?", {sid(account)});
    auto made = db.execute("INSERT INTO `character` (account_id,name,race,class,map_id,pos_x,pos_y,pos_z) VALUES (?,?,1,1,0,0,0,0)",
                           {sid(account), db::Param{name}});
    const auto character = made.last_insert_id;
    Store store;
    auto first = itm::mint_instance(db, 980001, 1, character);
    auto second = itm::mint_instance(db, 980001, 1, character);
    auto potion = itm::mint_instance(db, 980002, 1, character);
    itm::place_item(db, character, 0, itm::backpack_placement_slot(0), first.item_guid);
    itm::place_item(db, character, 0, itm::backpack_placement_slot(1), second.item_guid);
    itm::place_item(db, character, 0, itm::backpack_placement_slot(2), potion.item_guid);

    EquipTypeCatalog types; types.add(EquipTypeDef{77, "test:sword", "Sword", EquipCategory::kWeapon, "main"});
    ClassRecord allowed; allowed.roster_id = 1; allowed.usable_weapon_types.insert(77);
    ClassRecord denied; denied.roster_id = 2;

    // Drive the real verifier + Dispatcher handler over TLS, not only the service.
    // The request has no character id: ctx.char_id is the sole ownership binding.
    install_content_stores(&store, nullptr, nullptr, nullptr);
    ClassCatalog classes;
    classes.touch(1) = allowed;
    classes.touch(2) = denied;
    char temp[] = "/tmp/meridian-equipment-XXXXXX";
    char* dir = mkdtemp(temp);
    check("wire test temp directory created", dir != nullptr);
    if (dir != nullptr) {
        const std::string cert = std::string(dir) + "/cert.pem";
        const std::string key = std::string(dir) + "/key.pem";
        check("wire test certificate created", generate_self_signed(cert, key));
        mn::ListenConfig cfg;
        cfg.cert_path = cert;
        cfg.key_path = key;
        cfg.bind_addr = "127.0.0.1";
        cfg.port = 0;
        mn::TlsListener listener(cfg);
        bool dispatch_ok = true;
        std::thread server([&] {
            try {
                mn::Session session = listener.accept();
                ConnCtx ctx;
                ctx.authenticated = true;
                ctx.account_id = account;
                ctx.phase = SessionPhase::kInWorld;
                ctx.char_db = &db;
                ctx.char_id = character;
                ctx.char_level = 1;
                ctx.class_catalog = &classes;
                ctx.equip_type_catalog = &types;
                for (int request_index = 0; request_index < 3; ++request_index) {
                    ctx.char_class = request_index == 1 ? 2 : 1;
                    const Bytes frame = session.read_frame();
                    std::uint16_t opcode = 0;
                    std::uint64_t seq = 0;
                    if (dispatcher.dispatch(session, frame, ctx, opcode, seq) !=
                        DispatchOutcome::kHandled) {
                        dispatch_ok = false;
                    }
                }
            } catch (...) {
                dispatch_ok = false;
            }
        });
        {
            Client client(listener.local_port());
            check("wire client connected over TLS", client.connected());

            client.send_frame(encode_frame(
                mn::Opcode::EQUIPMENT_CHANGE_REQUEST, 101,
                equipment_request(mn::EquipmentChangeAction::EQUIP, 0)));
            const EquipmentReply equipped = receive_equipment_reply(client);
            check("wire equip returns OK", equipped.status == mn::EquipmentChangeStatus::OK);
            check("wire result precedes authoritative snapshot",
                  equipped.result_before_snapshot);
            check("wire equip snapshot contains paperdoll item", equipped.equipped_count == 1);

            client.send_frame(encode_frame(
                mn::Opcode::EQUIPMENT_CHANGE_REQUEST, 102,
                equipment_request(mn::EquipmentChangeAction::EQUIP, 1)));
            const EquipmentReply rejected = receive_equipment_reply(client);
            check("wire wrong-class maps to NOT_PROFICIENT",
                  rejected.status == mn::EquipmentChangeStatus::NOT_PROFICIENT);
            check("wire reject still reconciles authoritative snapshot",
                  rejected.result_before_snapshot && rejected.equipped_count == 1);

            client.send_frame(encode_frame(
                mn::Opcode::EQUIPMENT_CHANGE_REQUEST, 103,
                equipment_request(mn::EquipmentChangeAction::UNEQUIP,
                                  static_cast<std::uint16_t>(itm::EquipSlot::kMainHand))));
            const EquipmentReply unequipped = receive_equipment_reply(client);
            check("wire unequip returns OK", unequipped.status == mn::EquipmentChangeStatus::OK);
            check("wire unequip snapshot clears paperdoll",
                  unequipped.result_before_snapshot && unequipped.equipped_count == 0);
        }
        server.join();
        check("all equipment requests dispatched through live handler", dispatch_ok);
    }

    equip_owned_item(db, character, 0, 1, allowed, types, store);
    {
      auto inv = itm::load_inventory(db, character, store);
      check("equip persists across reload", inv.equipped_at(itm::EquipSlot::kMainHand)->item_guid == first.item_guid);
    }
    equip_owned_item(db, character, 1, 1, allowed, types, store);
    {
      auto inv = itm::load_inventory(db, character, store);
      check("replacement equips incoming", inv.equipped_at(itm::EquipSlot::kMainHand)->item_guid == second.item_guid);
      check("replacement returns displaced to source slot", inv.backpack_at(1)->item_guid == first.item_guid);
    }
    check("wrong class rejects", throws<EquipGateRejected>([&] { equip_owned_item(db, character, 1, 1, denied, types, store); }));
    {
      auto inv = itm::load_inventory(db, character, store);
      check("reject leaves placements unchanged", inv.equipped_at(itm::EquipSlot::kMainHand)->item_guid == second.item_guid);
    }
    check("non-equippable rejects", throws<itm::NotEquippable>([&] { equip_owned_item(db, character, 2, 1, allowed, types, store); }));
    unequip_owned_item(db, character, itm::EquipSlot::kMainHand, store);
    {
      auto inv = itm::load_inventory(db, character, store);
      check("unequip persists", inv.equipped_at(itm::EquipSlot::kMainHand) == nullptr);
      check("unequipped item returns to first free slot", inv.backpack_at(0)->item_guid == second.item_guid);
    }

    db.execute("DELETE FROM `character` WHERE id=?", {sid(character)});
    std::printf("%s (%d failure(s))\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}
