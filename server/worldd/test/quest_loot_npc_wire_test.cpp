// SPDX-License-Identifier: Apache-2.0
//
// worldd — quest / loot / gossip / trainer WIRE round-trip integration test (issue
// #388, epic #20). Proves the QST-01 / ITM-02 / NPC-01/02 opcode families are LIVE
// on the session dispatch path (not just the pure libs): a connected in-world
// session accepts a quest, opens a loot window on a corpse, and talks to a gossip /
// trainer NPC over the REAL TLS listener + world serve/dispatch loop.
//
// CLEAN-ROOM: written from the server SAD, the world.fbs wire contract, the
// quest_log / loot / npc module headers, and the OpenSSL public API only. No GPL
// source consulted (CONTRIBUTING.md).
//
// Self-contained (no DB): a test-installed WORLD_HELLO handler promotes the
// connection IN-WORLD (spawns + emplaces a quest log + captures a synthetic class/
// level) without a grant DB, and the test seeds the shared loot registry with a
// corpse. So it always runs in the plain server `ctest` (like worldd-combat-wire).
// The DB-asserting flows (loot -> inventory, turn-in reward money, trainer copper
// debit) live in the DB-backed worldd-quest-loot-npc test.

#include "world_dispatch.h"

#include "loot_roll.h"
#include "loot_session.h"
#include "npc_def.h"       // placeholder NPC ids (kNpcQuestGiver / kNpcTrainer)
#include "quest_def.h"     // placeholder quest ids
#include "item_template.h"  // Copper Ore template id

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
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mw = meridian::worldd;
namespace lo = meridian::loot;
namespace npc = meridian::npc;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

using Bytes = std::vector<std::uint8_t>;

// M1 placeholder ids the test drives (mirror the server-side placeholder stores).
constexpr std::uint32_t kQuestGiverNpc = mw::kPlaceholderNpcIdBase + 1;  // gives Q1 (kill)
constexpr std::uint32_t kSmithNpc      = mw::kPlaceholderNpcIdBase + 2;  // gives Q2 (collect, choice reward)
constexpr std::uint32_t kTrainerNpc    = npc::kNpcTrainer;               // trainer + gives Q1
constexpr std::uint32_t kQ1            = mw::kPlaceholderQuestIdBase + 1; // "Culling the Kobolds"
constexpr std::uint32_t kQ2            = mw::kPlaceholderQuestIdBase + 2; // "Ore for the Smith" (choice reward)
constexpr std::uint32_t kCopperOre     = items::kPlaceholderIdBase + 8;
// Q1 reward preview: 50 XP, 120 copper, 2x Minor Health Potion (always-granted).
constexpr std::uint32_t kMinorHealthPotion = items::kPlaceholderIdBase + 7;
// Q2 choice preview: Worn Shortsword OR Cracked Buckler (pick one).
constexpr std::uint32_t kWornShortsword    = items::kPlaceholderIdBase + 1;
constexpr std::uint32_t kCrackedBuckler    = items::kPlaceholderIdBase + 2;
constexpr std::uint64_t kCorpse        = 0xC0FFEEULL;      // the seeded (owned) corpse
constexpr std::uint64_t kCorpseForeign = 0xBADCAFEULL;     // a corpse owned by someone else
constexpr std::uint64_t kSelfGuid      = 4242ULL;          // this session's synthetic char guid

// ---- Throwaway self-signed cert (OpenSSL API; mirrors the other wire tests) ---
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
            reinterpret_cast<const unsigned char*>("meridian-qln-wire-test"), -1, -1, 0);
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

Bytes bytes_of(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes enc_world_hello() {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateWorldHello(b, /*grant_id=*/1, /*build=*/1, 0, 0));
    return bytes_of(b);
}
Bytes enc_quest_accept(std::uint32_t quest_id, std::uint64_t giver) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestAccept(b, quest_id, giver));
    return bytes_of(b);
}
Bytes enc_quest_log_req() {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::QuestLogEntry>> none;
    b.Finish(mn::CreateQuestLog(b, b.CreateVector(none)));
    return bytes_of(b);
}
Bytes enc_loot_request(std::uint64_t corpse) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootRequest(b, corpse));
    return bytes_of(b);
}
Bytes enc_loot_release(std::uint64_t corpse) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootRelease(b, corpse));
    return bytes_of(b);
}
Bytes enc_gossip_hello(std::uint64_t npc) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateGossipHello(b, npc));
    return bytes_of(b);
}

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

// Send `req` under `opcode`, read one reply, validate its opcode, return payload.
std::optional<Bytes> round_trip(Client& c, mn::Opcode opcode, const Bytes& req,
                                mn::Opcode resp_opcode, std::uint64_t seq) {
    if (!c.send_frame(mw::encode_frame(opcode, seq, req))) return std::nullopt;
    std::optional<Bytes> reply = c.recv_frame();
    if (!reply) return std::nullopt;
    std::optional<mw::Frame> rf = mw::decode_frame(*reply);
    if (!rf || rf->opcode != resp_opcode) return std::nullopt;
    return Bytes(rf->payload, rf->payload + rf->payload_len);
}

// Seed a corpse loot session (5 Copper Ore + 50 copper) owned by `owner`, at the
// bootstrap spawn point (so an in-world session standing there is in range).
void seed_corpse(mw::WorldServer& world, std::uint64_t corpse, std::uint64_t owner) {
    lo::LootRoll roll;
    roll.stacks.push_back(lo::LootStack{kCopperOre, /*count=*/5, /*required_quest_id=*/0});
    roll.copper = 50;
    const lo::LootPoint pos{64.0f, 64.0f, 0.0f};  // == ENTER_WORLD spawn (kZoneMaxXY*0.5)
    world.loot_registry().insert(
        lo::LootSession(corpse, pos, std::move(roll), {owner}, /*loot_range=*/5.0f));
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd quest/loot/gossip/trainer WIRE test (#388)\n");

    // --- Wiring proof (pure): a default Dispatcher routes every new opcode. -----
    {
        mw::Dispatcher d;
        check("QUEST_ACCEPT has a handler", d.has_handler(mn::Opcode::QUEST_ACCEPT));
        check("QUEST_TURN_IN has a handler", d.has_handler(mn::Opcode::QUEST_TURN_IN));
        check("QUEST_LOG has a handler", d.has_handler(mn::Opcode::QUEST_LOG));
        check("LOOT_REQUEST has a handler", d.has_handler(mn::Opcode::LOOT_REQUEST));
        check("LOOT_TAKE has a handler", d.has_handler(mn::Opcode::LOOT_TAKE));
        check("LOOT_RELEASE has a handler", d.has_handler(mn::Opcode::LOOT_RELEASE));
        check("GOSSIP_HELLO has a handler", d.has_handler(mn::Opcode::GOSSIP_HELLO));
        check("TRAINER_LEARN has a handler", d.has_handler(mn::Opcode::TRAINER_LEARN));
    }

    char tmpl[] = "/tmp/meridian-qln-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); return 1; }
    std::string cert_path = std::string(dir) + "/cert.pem";
    std::string key_path = std::string(dir) + "/key.pem";
    check("generated self-signed cert+key", generate_self_signed(cert_path, key_path));

    try {
        net::ListenConfig lc;
        lc.cert_path = cert_path;
        lc.key_path = key_path;
        lc.bind_addr = "127.0.0.1";
        lc.port = 0;  // ephemeral
        net::TlsListener listener(lc);
        std::uint16_t port = listener.local_port();
        check("listener bound to ephemeral port", port != 0);

        mw::Dispatcher dispatcher;
        mw::WorldServer world(dispatcher, mw::WorldServerConfig{});
        world.start();

        // Seed two corpses: one owned by THIS session's char guid, one owned by a
        // stranger (the wrong-looter case).
        seed_corpse(world, kCorpse, kSelfGuid);
        seed_corpse(world, kCorpseForeign, /*owner=*/9999);

        // TEST WORLD_HELLO: promote IN-WORLD without a grant DB. Emplace a quest log
        // over the placeholder store, capture a synthetic class/level (Runcaller-ish
        // class 2, level 1 — so the Vanguard-only strike is WRONG_CLASS and the
        // level-5 heal is LEVEL_TOO_LOW in the trainer list), and spawn at (64,64,0).
        static mw::PlaceholderQuestStore quest_store;
        dispatcher.on(mn::Opcode::WORLD_HELLO,
                      [](net::Session& /*sess*/, const mw::Frame& /*f*/, mw::ConnCtx& ctx) {
                          ctx.authenticated = true;
                          ctx.account_id = 1;
                          ctx.phase = mw::SessionPhase::kInWorld;
                          ctx.char_id = kSelfGuid;
                          ctx.char_class = 2;
                          ctx.char_level = 1;
                          ctx.quests.emplace(quest_store);
                          mw::Position spawn;
                          spawn.x = 64.0f;
                          spawn.y = 64.0f;
                          spawn.z = 0.0f;
                          ctx.movement.emplace(spawn, /*spawn_time_ms=*/0);
                          ctx.movement->set_entity_guid(kSelfGuid);
                      });

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

        // ===== Pre-handshake: a quest opcode before WORLD_HELLO -> Disconnect =====
        {
            Client c(port);
            check("pre-auth client connected", c.connected());
            c.send_frame(mw::encode_frame(mn::Opcode::QUEST_ACCEPT, /*seq=*/1,
                                          enc_quest_accept(kQ1, kQuestGiverNpc)));
            std::optional<Bytes> reply = c.recv_frame();
            bool is_disc = false;
            if (reply) {
                std::optional<mw::Frame> rf = mw::decode_frame(*reply);
                is_disc = rf && rf->opcode == mn::Opcode::DISCONNECT;
            }
            check("pre-handshake QUEST_ACCEPT -> Disconnect", is_disc);
        }

        // ===== Main in-world flow ================================================
        {
            Client c(port);
            check("main client connected", c.connected());
            std::uint64_t seq = 1;
            // Promote in-world (the test stub replies nothing; ENTER is implicit).
            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, seq++, enc_world_hello()));

            // --- QUEST_ACCEPT(Q1) -> OK ---------------------------------------
            if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                     enc_quest_accept(kQ1, kQuestGiverNpc),
                                     mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                const auto* m = decode<mn::QuestAcceptResult>(*pl);
                check("accept Q1 -> OK",
                      m && m->quest_id() == kQ1 &&
                          m->status() == mn::QuestAcceptStatus::OK);
            } else {
                check("got a QuestAcceptResult (Q1)", false);
            }
            // On OK, a QuestLog snapshot follows — drain it.
            {
                std::optional<Bytes> snap = c.recv_frame();
                std::optional<mw::Frame> rf = snap ? mw::decode_frame(*snap) : std::nullopt;
                bool has_q1 = false;
                if (rf && rf->opcode == mn::Opcode::QUEST_LOG) {
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    const auto* ql = decode<mn::QuestLog>(pl);
                    if (ql && ql->quests())
                        for (const auto* e : *ql->quests())
                            if (e->quest_id() == kQ1) has_q1 = true;
                }
                check("QuestLog snapshot after accept lists Q1", has_q1);
            }

            // --- re-accept -> ALREADY_ACTIVE ----------------------------------
            if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                     enc_quest_accept(kQ1, kQuestGiverNpc),
                                     mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                const auto* m = decode<mn::QuestAcceptResult>(*pl);
                check("re-accept Q1 -> ALREADY_ACTIVE",
                      m && m->status() == mn::QuestAcceptStatus::ALREADY_ACTIVE);
            } else {
                check("got a QuestAcceptResult (re-accept)", false);
            }

            // --- unknown quest -> UNKNOWN_QUEST --------------------------------
            if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                     enc_quest_accept(0xDEADBEEF, 0),
                                     mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                const auto* m = decode<mn::QuestAcceptResult>(*pl);
                check("accept unknown quest -> UNKNOWN_QUEST",
                      m && m->status() == mn::QuestAcceptStatus::UNKNOWN_QUEST);
            } else {
                check("got a QuestAcceptResult (unknown)", false);
            }

            // --- wrong giver -> WRONG_GIVER (Q1's giver is kQuestGiverNpc) ------
            if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                     enc_quest_accept(kQ1, /*giver=*/999999),
                                     mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                const auto* m = decode<mn::QuestAcceptResult>(*pl);
                check("accept Q1 at the wrong giver -> WRONG_GIVER",
                      m && m->status() == mn::QuestAcceptStatus::WRONG_GIVER);
            } else {
                check("got a QuestAcceptResult (wrong giver)", false);
            }

            // --- QUEST_LOG request -> snapshot lists Q1 + its reward preview ---
            // The reward preview (always-granted items + XP + copper) rides on the
            // QuestLogEntry so the client can render the turn-in offer straight from
            // its log, without a separate quest-detail round-trip (#443).
            if (auto pl = round_trip(c, mn::Opcode::QUEST_LOG, enc_quest_log_req(),
                                     mn::Opcode::QUEST_LOG, seq++)) {
                const auto* ql = decode<mn::QuestLog>(*pl);
                const mn::QuestLogEntry* q1 = nullptr;
                if (ql && ql->quests())
                    for (const auto* e : *ql->quests())
                        if (e->quest_id() == kQ1) q1 = e;
                check("QUEST_LOG request returns a snapshot with Q1", q1 != nullptr);
                if (q1 != nullptr) {
                    check("Q1 reward preview: reward_xp == 50", q1->reward_xp() == 50u);
                    check("Q1 reward preview: reward_money == 120",
                          q1->reward_money() == 120);
                    const auto* items = q1->reward_items();
                    check("Q1 reward preview: 1 always-granted item",
                          items != nullptr && items->size() == 1);
                    if (items != nullptr && items->size() == 1)
                        check("Q1 reward preview: 2x Minor Health Potion",
                              items->Get(0)->item_id() == kMinorHealthPotion &&
                                  items->Get(0)->count() == 2u);
                    // Q1 is a flat-reward quest: no choice picker.
                    const auto* choices = q1->choice_items();
                    check("Q1 reward preview: no choice_items (flat reward)",
                          choices == nullptr || choices->size() == 0);
                }
            } else {
                check("got a QuestLog (request)", false);
            }

            // --- QUEST_ACCEPT(Q2) at the smith -> the log exposes its choice_items --
            // Q2 ("Ore for the Smith") is a CHOICE-reward quest: the client must see
            // its choice_items[] to render the picker before turn-in (#443).
            if (auto pl = round_trip(c, mn::Opcode::QUEST_ACCEPT,
                                     enc_quest_accept(kQ2, kSmithNpc),
                                     mn::Opcode::QUEST_ACCEPT_RESULT, seq++)) {
                const auto* m = decode<mn::QuestAcceptResult>(*pl);
                check("accept Q2 at the smith -> OK",
                      m && m->quest_id() == kQ2 &&
                          m->status() == mn::QuestAcceptStatus::OK);
            } else {
                check("got a QuestAcceptResult (Q2)", false);
            }
            // Drain the QuestLog snapshot that follows the accept and inspect Q2.
            {
                std::optional<Bytes> snap = c.recv_frame();
                std::optional<mw::Frame> rf = snap ? mw::decode_frame(*snap) : std::nullopt;
                const mn::QuestLogEntry* q2 = nullptr;
                const mn::QuestLog* ql = nullptr;
                Bytes pl;
                if (rf && rf->opcode == mn::Opcode::QUEST_LOG) {
                    pl.assign(rf->payload, rf->payload + rf->payload_len);
                    ql = decode<mn::QuestLog>(pl);
                    if (ql && ql->quests())
                        for (const auto* e : *ql->quests())
                            if (e->quest_id() == kQ2) q2 = e;
                }
                check("QuestLog after Q2 accept lists Q2", q2 != nullptr);
                if (q2 != nullptr) {
                    const auto* choices = q2->choice_items();
                    check("Q2 reward preview: 2 choice_items",
                          choices != nullptr && choices->size() == 2);
                    if (choices != nullptr && choices->size() == 2)
                        check("Q2 reward preview: Worn Shortsword OR Cracked Buckler",
                              choices->Get(0)->item_id() == kWornShortsword &&
                                  choices->Get(1)->item_id() == kCrackedBuckler);
                    check("Q2 reward preview: reward_xp == 40", q2->reward_xp() == 40u);
                    check("Q2 reward preview: reward_money == 75",
                          q2->reward_money() == 75);
                }
            }

            // --- GOSSIP on the quest giver -> menu carries a quest option ------
            if (auto pl = round_trip(c, mn::Opcode::GOSSIP_HELLO,
                                     enc_gossip_hello(kQuestGiverNpc),
                                     mn::Opcode::GOSSIP_MENU, seq++)) {
                const auto* m = decode<mn::GossipMenu>(*pl);
                bool in_progress = false;  // Q1 is accepted -> in-progress option
                if (m && m->options())
                    for (const auto* o : *m->options())
                        if (o->kind() == mn::GossipOptionKind::QUEST_IN_PROGRESS &&
                            o->target_id() == kQ1)
                            in_progress = true;
                check("gossip on giver shows Q1 as IN_PROGRESS",
                      m && m->npc_guid() == kQuestGiverNpc && in_progress);
            } else {
                check("got a GossipMenu (giver)", false);
            }

            // --- GOSSIP on the trainer -> menu + a pushed TrainerList ----------
            if (auto pl = round_trip(c, mn::Opcode::GOSSIP_HELLO,
                                     enc_gossip_hello(kTrainerNpc),
                                     mn::Opcode::GOSSIP_MENU, seq++)) {
                const auto* m = decode<mn::GossipMenu>(*pl);
                bool has_trainer = false;
                if (m && m->options())
                    for (const auto* o : *m->options())
                        if (o->kind() == mn::GossipOptionKind::TRAINER) has_trainer = true;
                check("gossip on trainer shows a TRAINER option", m && has_trainer);
            } else {
                check("got a GossipMenu (trainer)", false);
            }
            // The trainer NPC also pushes a TrainerList (S->C) — read + verify it.
            {
                std::optional<Bytes> tl = c.recv_frame();
                std::optional<mw::Frame> rf = tl ? mw::decode_frame(*tl) : std::nullopt;
                bool wrong_class = false, level_low = false;
                if (rf && rf->opcode == mn::Opcode::TRAINER_LIST) {
                    Bytes pl(rf->payload, rf->payload + rf->payload_len);
                    const auto* m = decode<mn::TrainerList>(pl);
                    if (m && m->entries())
                        for (const auto* e : *m->entries()) {
                            if (e->required_class() != 0 &&
                                e->state() == mn::TrainableState::WRONG_CLASS)
                                wrong_class = true;
                            if (e->required_level() > 1 &&
                                e->state() == mn::TrainableState::LEVEL_TOO_LOW)
                                level_low = true;
                        }
                }
                check("TrainerList gates a class-only ability as WRONG_CLASS", wrong_class);
                check("TrainerList gates a high-level ability as LEVEL_TOO_LOW", level_low);
            }

            // --- GOSSIP on an unknown NPC -> an empty menu --------------------
            if (auto pl = round_trip(c, mn::Opcode::GOSSIP_HELLO, enc_gossip_hello(123456),
                                     mn::Opcode::GOSSIP_MENU, seq++)) {
                const auto* m = decode<mn::GossipMenu>(*pl);
                check("gossip on an unknown NPC -> empty menu",
                      m && (m->options() == nullptr || m->options()->size() == 0));
            } else {
                check("got a GossipMenu (unknown)", false);
            }

            // --- LOOT_REQUEST on the owned corpse -> OK + a slot ---------------
            if (auto pl = round_trip(c, mn::Opcode::LOOT_REQUEST, enc_loot_request(kCorpse),
                                     mn::Opcode::LOOT_RESPONSE, seq++)) {
                const auto* m = decode<mn::LootResponse>(*pl);
                bool has_ore = false;
                if (m && m->items())
                    for (const auto* it : *m->items())
                        if (it->item_template_id() == kCopperOre) has_ore = true;
                check("loot the owned corpse -> OK + lists the ore + copper",
                      m && m->status() == mn::LootStatus::OK && m->copper() == 50 && has_ore);
            } else {
                check("got a LootResponse (owned)", false);
            }

            // --- LOOT_RELEASE -> LOOT_CLOSED ----------------------------------
            if (auto pl = round_trip(c, mn::Opcode::LOOT_RELEASE, enc_loot_release(kCorpse),
                                     mn::Opcode::LOOT_CLOSED, seq++)) {
                const auto* m = decode<mn::LootClosed>(*pl);
                check("release -> LOOT_CLOSED", m && m->corpse_guid() == kCorpse);
            } else {
                check("got a LootClosed", false);
            }

            // --- LOOT_REQUEST on an unknown corpse -> NO_SUCH_CORPSE ----------
            if (auto pl = round_trip(c, mn::Opcode::LOOT_REQUEST, enc_loot_request(0x1234),
                                     mn::Opcode::LOOT_RESPONSE, seq++)) {
                const auto* m = decode<mn::LootResponse>(*pl);
                check("loot an unknown corpse -> NO_SUCH_CORPSE",
                      m && m->status() == mn::LootStatus::NO_SUCH_CORPSE);
            } else {
                check("got a LootResponse (unknown)", false);
            }

            // --- LOOT_REQUEST on a foreign-owned corpse -> NOT_A_LOOTER -------
            if (auto pl = round_trip(c, mn::Opcode::LOOT_REQUEST,
                                     enc_loot_request(kCorpseForeign),
                                     mn::Opcode::LOOT_RESPONSE, seq++)) {
                const auto* m = decode<mn::LootResponse>(*pl);
                check("loot a corpse owned by another player -> NOT_A_LOOTER",
                      m && m->status() == mn::LootStatus::NOT_A_LOOTER);
            } else {
                check("got a LootResponse (foreign)", false);
            }
        }  // client closes

        server.join();
        world.stop();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wire test exception: %s\n", e.what());
        ++g_fail;
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    std::printf(g_fail == 0 ? "\nALL WORLDD QUEST/LOOT/NPC WIRE TESTS PASSED\n"
                            : "\n%d WORLDD QUEST/LOOT/NPC WIRE TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
