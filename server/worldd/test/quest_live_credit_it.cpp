// SPDX-License-Identifier: Apache-2.0
//
// worldd — LIVE quest objective crediting integration test (issue #396, QST-01;
// epic #20's owner-signed-off MapTick↔session event-bus). Proves that on the LIVE
// session dispatch path a player's world actions advance the quests THEY accepted
// and emit QUEST_PROGRESS, per objective SOURCE:
//
//   * KILL    — a creature death routed THROUGH a real MapTick (the typed
//               kCreatureKill event) is credited to the KILLER's session via the
//               shared quest-credit bus, advancing that session's kill objective —
//               and NOT a different guid's identical quest (per-killer isolation).
//   * DELIVER — talking to the deliver-target NPC (GOSSIP_HELLO) completes the
//               deliver objective.
//   * EXPLORE — walking into a discovery area-trigger volume (MOVEMENT_INTENT →
//               the AoI relay's trigger phase) completes the explore objective.
//
// (The COLLECT source's live path — loot pull → sync_collect → QUEST_PROGRESS —
// is the regression guard covered by the DB-backed worldd-quest-loot-npc test,
// which drives a real LOOT_TAKE into the character's DB inventory.)
//
// The kill chain is exercised end-to-end: a REAL MapTick produces the kCreatureKill
// deltas, which are routed through the SAME route_tick_events() the world thread
// runs, into the SAME quest_credit registry the live session drains on its IO
// worker (poll_quest_credits) — so the test faithfully mirrors the production seam
// without needing the world thread to spawn content.
//
// Self-contained (no DB): a test-installed WORLD_HELLO handler promotes the
// connection IN-WORLD (emplaces a quest log over a SYNTHETIC store, AoI-enters, and
// registers the session on the quest-credit bus) without a grant/characters DB. So
// it always runs in the plain server `ctest` (like worldd-quest-loot-npc-wire).
//
// CLEAN-ROOM: written from the server SAD, the world.fbs wire contract, and the
// worldd module headers only. No GPL source consulted (CONTRIBUTING.md).

#include "world_dispatch.h"

#include "ability_store.h"
#include "area_triggers.h"
#include "combat_unit.h"
#include "creature_ai.h"
#include "map_tick.h"
#include "movement_validation.h"
#include "quest_def.h"

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

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

using Bytes = std::vector<std::uint8_t>;

// ---- Synthetic (ungated) quest content the test drives -----------------------
// Distinct ids clear of the placeholder range; each quest has ONE objective of a
// single source, no level/prerequisite gate, so the live path can be driven with a
// stub WORLD_HELLO (no DB) — the story's "synthetic quest content" option.
constexpr std::uint32_t kSelfGuid   = 4242ULL;  // this session's synthetic char guid
constexpr std::uint32_t kOtherGuid  = 7777ULL;  // a DIFFERENT session (isolation check)

constexpr mw::QuestId   kQKill    = 0x51000001;  // kill 1 of kTargetNpc
constexpr mw::QuestId   kQDeliver = 0x51000002;  // deliver kDeliverItem to kDeliverNpc
constexpr mw::QuestId   kQExplore = 0x51000003;  // explore (kExploreZone, "ashvent_overlook")

constexpr std::uint32_t kGiverNpc    = 0x5200'0001;  // offers all three (accept giver check)
constexpr std::uint32_t kTargetNpc   = 0x5200'0010;  // the kill-objective creature template
constexpr std::uint32_t kDeliverItem = 0x5300'0009;  // the deliver item (granted on accept)
constexpr std::uint32_t kDeliverNpc  = 0x5200'0003;  // the deliver-target NPC
constexpr std::uint32_t kExploreZone = 0x5400'0001;  // the explore zone id
const std::string       kExplorePoi  = "ashvent_overlook";  // the explore poi join key
constexpr std::uint32_t kExploreTrigger = 909;       // the area-trigger volume id

// A minimal read-only QuestStore over the three synthetic quests.
class TestQuestStore final : public mw::QuestStore {
public:
    TestQuestStore() {
        {
            mw::QuestDef q;
            q.id = kQKill;
            q.name = "Test Kill";
            q.giver_npc_id = kGiverNpc;
            mw::QuestObjective o;
            o.type = mw::ObjectiveType::kKill;
            o.target_npc_id = kTargetNpc;
            o.count = 1;
            q.objectives = {o};
            defs_.push_back(q);
        }
        {
            mw::QuestDef q;
            q.id = kQDeliver;
            q.name = "Test Deliver";
            q.giver_npc_id = kGiverNpc;
            mw::QuestObjective o;
            o.type = mw::ObjectiveType::kDeliver;
            o.item_id = kDeliverItem;
            o.to_npc_id = kDeliverNpc;
            o.count = 1;
            q.objectives = {o};
            defs_.push_back(q);
        }
        {
            mw::QuestDef q;
            q.id = kQExplore;
            q.name = "Test Explore";
            q.giver_npc_id = kGiverNpc;
            mw::QuestObjective o;
            o.type = mw::ObjectiveType::kExplore;
            o.zone_id = kExploreZone;
            o.poi = kExplorePoi;
            o.count = 1;
            q.objectives = {o};
            defs_.push_back(q);
        }
    }
    const mw::QuestDef* find(mw::QuestId id) const override {
        for (const mw::QuestDef& q : defs_)
            if (q.id == id) return &q;
        return nullptr;
    }
    std::vector<mw::QuestId> ids() const override {
        std::vector<mw::QuestId> out;
        for (const mw::QuestDef& q : defs_) out.push_back(q.id);
        return out;
    }

private:
    std::vector<mw::QuestDef> defs_;
};

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
            reinterpret_cast<const unsigned char*>("meridian-qlc-it"), -1, -1, 0);
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
Bytes enc_gossip_hello(std::uint64_t npc) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateGossipHello(b, npc));
    return bytes_of(b);
}
Bytes enc_movement_intent(std::uint32_t seq, std::uint32_t flags, float x, float y, float z,
                          std::uint64_t client_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateMovementIntent(b, seq, flags, x, y, z, /*orientation=*/0.0f,
                                      client_time_ms));
    return bytes_of(b);
}

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

// Read frames until a QUEST_PROGRESS arrives (skipping the interleaved reply frames
// — QUEST_LOG / GOSSIP_MENU / MOVEMENT_STATE / etc.), or nullopt if the stream ends
// first. Returns the decoded QuestProgress fields of interest.
struct Progress { std::uint32_t quest_id; std::uint16_t have; bool complete; bool got; };
Progress read_quest_progress(Client& c) {
    for (int i = 0; i < 8; ++i) {
        std::optional<Bytes> raw = c.recv_frame();
        if (!raw) break;
        std::optional<mw::Frame> rf = mw::decode_frame(*raw);
        if (!rf) break;
        if (rf->opcode != mn::Opcode::QUEST_PROGRESS) continue;  // skip reply frames
        Bytes pl(rf->payload, rf->payload + rf->payload_len);
        const auto* qp = decode<mn::QuestProgress>(pl);
        if (qp == nullptr) break;
        return Progress{qp->quest_id(), qp->have(), qp->complete(), true};
    }
    return Progress{0, 0, false, false};
}

// Drive a real MapTick to kill ONE creature of `template_id` by `killer`, returning
// the tick deltas (which carry the typed kCreatureKill the world loop routes; #396).
std::vector<mw::TickEvent> map_tick_one_kill(mw::ObjectGuid killer, std::uint32_t template_id) {
    const mw::AbilityStore abilities = mw::load_placeholder_ability_store();
    mw::MapTick mt(abilities, /*rng_seed=*/0x0396ULL, /*dt_ms=*/1600);
    mt.set_report_kills(true);

    mw::UnitStats st;
    st.level = 5;
    st.max_health = 1000;
    st.resource_type = mw::ResourceType::kMana;
    st.max_resource = 1000;
    st.faction = mw::Faction::kPlayer;
    mw::Position home;
    home.x = 0.0f; home.y = 0.0f; home.z = 0.0f;
    mt.add_player(killer, home, st);

    mw::CreatureSpawnDef d;
    d.template_id = template_id;
    d.level = 1;
    d.faction = mw::Faction::kHostile;
    d.home = home;
    d.aggro_base_radius = 0;
    d.leash_radius = 1000;
    d.respawn_ms = 999999;
    d.move_speed = 0;
    d.patrol_mode = mw::PatrolMode::kStationary;
    const mw::ObjectGuid crt = mt.add_creature(d);
    mw::Unit* cu = mt.unit_for_guid(crt);
    cu->set_max_health(8);  // one melee strike is lethal

    std::vector<mw::TickEvent> all;
    for (int t = 0; t < 12 && cu->is_alive(); ++t) {
        mt.enqueue_cast(mw::AbilityUseCmd{killer, mw::kPlaceholderMeleeStrikeId, crt});
        std::vector<mw::TickEvent> deltas = mt.advance();
        for (mw::TickEvent& ev : deltas) all.push_back(std::move(ev));
    }
    return all;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd LIVE quest objective crediting test (#396)\n");

    static TestQuestStore quest_store;
    // Install the synthetic store GLOBALLY so encode_quest_log / the objective-source
    // credit paths + the session's QuestLog all resolve the SAME quest content.
    mw::install_content_stores(nullptr, nullptr, &quest_store, nullptr);

    char tmpl[] = "/tmp/meridian-qlc-XXXXXX";
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

        // Load a discovery area-trigger volume just EAST of the spawn (-320,-320,
        // the Zone-01 play-area centre — #562): a single legal walk step (→ -319.80)
        // crosses IN, carrying the explore poi the Test Explore quest matches. Spawn
        // (x=-320) is OUTSIDE (x-min -319.9) so the crossing fires on the MOVE, not
        // on enter.
        {
            mw::TriggerVolume v;
            v.id = kExploreTrigger;
            v.kind = mw::TriggerKind::kDiscovery;
            v.area_id = kExploreZone;
            v.poi = kExplorePoi;
            v.min_x = -319.9f; v.max_x = -319.0f;
            v.min_y = -321.0f; v.max_y = -319.0f;
            world.world_state().load_area_triggers({v});
        }

        world.start();

        // Also register a DIFFERENT session guid on the credit bus (no live client)
        // so we can assert a kill credited to kSelfGuid is NOT routed to it.
        world.quest_credit().register_session(kOtherGuid);

        // TEST WORLD_HELLO: promote IN-WORLD without a grant DB — emplace a quest log
        // over the synthetic store, capture a synthetic level, spawn at (64,64,0),
        // AoI-enter with kSelfGuid, and register on the quest-credit bus.
        dispatcher.on(mn::Opcode::WORLD_HELLO,
                      [](net::Session& /*sess*/, const mw::Frame& /*f*/, mw::ConnCtx& ctx) {
                          ctx.authenticated = true;
                          ctx.account_id = 1;
                          ctx.phase = mw::SessionPhase::kInWorld;
                          ctx.char_id = kSelfGuid;
                          ctx.char_class = 2;
                          ctx.char_level = 5;
                          ctx.quests.emplace(quest_store);  // the installed synthetic store
                          mw::Position spawn;
                          spawn.x = -320.0f;
                          spawn.y = -320.0f;
                          spawn.z = 0.0f;
                          ctx.movement.emplace(spawn, /*spawn_time_ms=*/0);
                          ctx.movement->set_entity_guid(kSelfGuid);
                          if (ctx.world != nullptr) {
                              mw::EntityIdentity id;
                              id.entity_guid = kSelfGuid;  // kept (non-zero) by enter()
                              id.type_id = 2;
                              id.char_class = 2;
                              mw::EnterResult er = ctx.world->enter(
                                  id, spawn,
                                  [](mn::Opcode, const std::vector<std::uint8_t>&) {
                                      return true;  // no other sessions -> relay never called
                                  });
                              ctx.slot = er.slot;
                              ctx.entered = true;
                              ctx.movement->set_entity_guid(er.entity_guid);
                          }
                          if (ctx.quest_credit != nullptr) {
                              ctx.credit_guid = ctx.movement->entity_guid();
                              ctx.credit_token =
                                  ctx.quest_credit->register_session(ctx.credit_guid);
                          }
                      });

        std::thread server([&] {
            try {
                net::Session s = listener.accept();
                world.serve_connection(std::move(s));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "  server thread error: %s\n", e.what());
            }
        });

        {
            Client c(port);
            check("client connected", c.connected());
            std::uint64_t seq = 1;
            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, seq++, enc_world_hello()));

            // --- Accept all three quests (each replies ACCEPT_RESULT + a QuestLog) ---
            auto accept = [&](std::uint32_t qid, const char* label) {
                c.send_frame(mw::encode_frame(mn::Opcode::QUEST_ACCEPT, seq++,
                                              enc_quest_accept(qid, kGiverNpc)));
                bool ok = false;
                std::optional<Bytes> reply = c.recv_frame();
                if (reply) {
                    std::optional<mw::Frame> rf = mw::decode_frame(*reply);
                    if (rf && rf->opcode == mn::Opcode::QUEST_ACCEPT_RESULT) {
                        Bytes pl(rf->payload, rf->payload + rf->payload_len);
                        const auto* m = decode<mn::QuestAcceptResult>(pl);
                        ok = m && m->quest_id() == qid &&
                             m->status() == mn::QuestAcceptStatus::OK;
                    }
                }
                check(label, ok);
                // Drain the post-accept QuestLog snapshot.
                if (std::optional<Bytes> snap = c.recv_frame()) {
                    std::optional<mw::Frame> rf = mw::decode_frame(*snap);
                    (void)rf;
                }
            };
            accept(kQKill, "accept Test Kill -> OK");
            accept(kQDeliver, "accept Test Deliver -> OK");
            accept(kQExplore, "accept Test Explore -> OK");

            // ===== KILL: MapTick death → route → session drains → QUEST_PROGRESS =====
            // A real MapTick kills one kTargetNpc by kSelfGuid; route the deltas through
            // the SAME path the world thread runs into the SAME registry the session
            // drains. The credit is NOT routed to the other registered guid.
            std::vector<mw::TickEvent> deltas = map_tick_one_kill(kSelfGuid, kTargetNpc);
            std::size_t kill_events = 0;
            for (const mw::TickEvent& ev : deltas)
                if (ev.kind == mw::TickEventKind::kCreatureKill && ev.killer_guid == kSelfGuid &&
                    ev.npc_template_id == kTargetNpc)
                    ++kill_events;
            check("MapTick emitted a kCreatureKill for the killer", kill_events == 1);

            mw::route_tick_events(deltas, world.quest_credit());
            // Isolation: the kill (killer = kSelfGuid) leaves the OTHER session's queue
            // empty — a kill credits ONLY the owning killer's session.
            check("a kill does NOT route to a different session's guid",
                  world.quest_credit().drain_kills(kOtherGuid).empty());

            // Poll the session: any handled frame drains its pending kill credits.
            c.send_frame(mw::encode_frame(mn::Opcode::QUEST_LOG, seq++, enc_quest_log_req()));
            Progress kp = read_quest_progress(c);
            check("kill advances the owning session's kill objective (QUEST_PROGRESS)",
                  kp.got && kp.quest_id == kQKill && kp.have == 1 && kp.complete);

            // ===== DELIVER: talk to the deliver NPC → QUEST_PROGRESS =====
            c.send_frame(mw::encode_frame(mn::Opcode::GOSSIP_HELLO, seq++,
                                          enc_gossip_hello(kDeliverNpc)));
            Progress dp = read_quest_progress(c);  // skips the GOSSIP_MENU reply
            check("deliver interaction advances the deliver objective (QUEST_PROGRESS)",
                  dp.got && dp.quest_id == kQDeliver && dp.complete);

            // ===== EXPLORE: walk into the discovery volume → QUEST_PROGRESS =====
            // One legal walk step east (-320 → -319.80) crosses into the volume.
            c.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, seq++,
                                          enc_movement_intent(/*seq=*/10, /*Walk=*/1,
                                                              -319.80f, -320.0f, 0.0f,
                                                              /*client_time_ms=*/100)));
            Progress ep = read_quest_progress(c);  // skips the MOVEMENT_STATE reply
            check("area-trigger enter advances the explore objective (QUEST_PROGRESS)",
                  ep.got && ep.quest_id == kQExplore && ep.complete);
        }  // client closes

        server.join();
        world.stop();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "live-credit test exception: %s\n", e.what());
        ++g_fail;
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    std::printf(g_fail == 0 ? "\nALL WORLDD LIVE QUEST CREDIT TESTS PASSED\n"
                            : "\n%d WORLDD LIVE QUEST CREDIT TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
