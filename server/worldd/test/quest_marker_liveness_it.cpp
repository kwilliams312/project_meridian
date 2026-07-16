// SPDX-License-Identifier: Apache-2.0
//
// worldd — MARKER LIVENESS integration test (#861). Proves the turn-in `?` over a
// DELIVER quest's target NPC lights PROACTIVELY — when the target is merely in
// visual range and the quest is completable (the deliver item was granted on
// accept) — WITHOUT any GOSSIP_HELLO interaction, driven by the THROTTLED marker-
// liveness heartbeat the serve loop polls (poll_quest_marker_heartbeat), NOT by
// movement or interaction:
//
//   * accept the deliver quest while its turn-in NPC (a courier) is visible ->
//     the courier's marker is a greyed `?` (TURN_IN_INCOMPLETE): on the quest, the
//     deliver objective not yet credited;
//   * the player STANDS STILL (no gossip, no movement that changes state); on the
//     next heartbeat past the throttle window the deliver credits on visual range ->
//     a QUEST_PROGRESS (deliver complete) is emitted and the courier's marker flips
//     to a lit `?` (TURN_IN_READY) — the `?` lit within ~1s, no interaction;
//   * a further heartbeat re-evaluates but must NOT re-credit (on_deliver is
//     idempotent) NOR re-push the (unchanged) marker (diffed) — no double-credit,
//     no per-frame wire spam.
//
// The heartbeat fires after each handled frame but only once per throttle window
// (~kMarkerHeartbeatMs). Production drives it via the client's ~10 Hz movement
// keepalive; here a benign QUEST_LOG request after a real sleep past the window
// stands in for "any frame arriving while the player is idle".
//
// Self-contained (no DB): synthetic quest content (a deliver quest) + a synthetic
// NpcStore modeling the courier as the quest's turn-in giver, installed globally,
// plus a stub WORLD_HELLO that promotes IN-WORLD and AoI-enters at the courier's
// spawn. So it always runs in the plain server ctest (like worldd-quest-live-credit).
// The deliver credit reuses on_deliver (no inventory scan — the item is granted on
// accept, so an active deliver objective whose target is in sight is deliverable),
// exactly the credit_deliver_at_npc logic #850 uses, so the DB-free path is faithful.
//
// CLEAN-ROOM: written from the server SAD, the world.fbs wire contract, and the
// worldd/npc module headers only. No GPL source consulted (CONTRIBUTING.md).

#include "world_dispatch.h"
#include "world_state.h"

#include "npc_def.h"      // NpcStore / NpcDef / NpcQuestRef — the courier turn-in NPC
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
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
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
namespace np = meridian::npc;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

using Bytes = std::vector<std::uint8_t>;

// ---- Synthetic content the test drives --------------------------------------
constexpr std::uint32_t kSelfGuid    = 4242ULL;              // this session's char guid
constexpr mw::QuestId   kQDeliver    = 0x61000001;           // deliver kDeliverItem -> courier
constexpr std::uint32_t kGiverNpc    = 0x6200'0001;          // the offering giver (not spawned)
constexpr std::uint32_t kDeliverItem = 0x6300'0009;          // the deliver item (granted on accept)
constexpr std::uint32_t kCourierNpc  = 0x6200'0003;          // the deliver-target / turn-in NPC
// The courier is SPAWNED — addressed on the wire by its world-entity guid (the band
// the client sees after ENTITY_ENTER), which resolves back to kCourierNpc.
constexpr std::uint64_t kCourierGuid = mw::kWorldEntityGuidBase;
// Session + courier share this spawn so the courier is at distance 0 (in AoI range).
constexpr float kSpawnX = -320.0f;
constexpr float kSpawnY = -320.0f;

// A minimal read-only QuestStore over the one synthetic deliver quest.
class TestQuestStore final : public mw::QuestStore {
public:
    TestQuestStore() {
        mw::QuestDef q;
        q.id = kQDeliver;
        q.name = "Deliver Liveness";
        q.giver_npc_id = kGiverNpc;
        q.turn_in_npc_id = kCourierNpc;  // courier takes the turn-in
        mw::QuestObjective o;
        o.type = mw::ObjectiveType::kDeliver;
        o.item_id = kDeliverItem;
        o.to_npc_id = kCourierNpc;
        o.count = 1;
        q.objectives = {o};
        defs_.push_back(q);
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

// A minimal NpcStore modeling the courier as the deliver quest's TURN-IN giver, so
// compute_quest_marker resolves a `?` for it (greyed while active-incomplete, lit
// once the deliver objective completes). Only kCourierNpc is modeled.
class TestNpcStore final : public np::NpcStore {
public:
    TestNpcStore() {
        courier_.id = kCourierNpc;
        courier_.name = "Test Courier";
        courier_.quests.push_back(np::NpcQuestRef{kQDeliver, /*gives=*/false, /*turn_in=*/true});
    }
    const np::NpcDef* find(np::NpcId npc_id) const override {
        return npc_id == kCourierNpc ? &courier_ : nullptr;
    }
    std::vector<np::NpcId> ids() const override { return {kCourierNpc}; }

private:
    np::NpcDef courier_;
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
            reinterpret_cast<const unsigned char*>("meridian-marker-liveness-it"), -1, -1, 0);
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

// ---- Minimal TLS 1.3 IF-2 client (bounded reads so a "no frame" drain ends) ---
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

    void set_recv_timeout_ms(int ms) {
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

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
            if (r <= 0) return false;  // timeout / close
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

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

// Everything the tests assert on, collected from one "drain until quiet" pass: the
// last marker seen for the courier (arrival order — the final wins) and whether a
// QUEST_PROGRESS marking the deliver quest complete arrived.
struct Drained {
    bool got_marker = false;
    mn::QuestMarkerKind marker = mn::QuestMarkerKind::NONE;
    bool deliver_complete = false;
    bool got_gossip_menu = false;  // must stay false — no interaction anywhere
};
Drained drain(Client& c) {
    Drained out;
    for (;;) {
        std::optional<Bytes> raw = c.recv_frame();
        if (!raw) break;  // timed out — the server is quiet
        std::optional<mw::Frame> rf = mw::decode_frame(*raw);
        if (!rf) continue;
        Bytes pl(rf->payload, rf->payload + rf->payload_len);
        if (rf->opcode == mn::Opcode::QUEST_MARKER_UPDATE) {
            if (const auto* m = decode<mn::QuestMarkerUpdate>(pl)) {
                if (m->npc_guid() == kCourierGuid) {
                    out.got_marker = true;
                    out.marker = m->marker();
                }
            }
        } else if (rf->opcode == mn::Opcode::QUEST_PROGRESS) {
            if (const auto* qp = decode<mn::QuestProgress>(pl))
                if (qp->quest_id() == kQDeliver && qp->complete())
                    out.deliver_complete = true;
        } else if (rf->opcode == mn::Opcode::GOSSIP_MENU) {
            out.got_gossip_menu = true;
        }
    }
    return out;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd marker-liveness (deliver-on-visual-range) test (#861)\n");

    static TestQuestStore quest_store;
    static TestNpcStore   npc_store;
    mw::install_content_stores(nullptr, nullptr, &quest_store, &npc_store);

    char tmpl[] = "/tmp/meridian-marker-liveness-XXXXXX";
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

        // Spawn the courier (the deliver-target) into the AoI world at the session's
        // spawn, so it is in the session's interest set the instant it enters. Its wire
        // guid resolves back to the courier template.
        {
            mw::EntityIdentity id;
            id.entity_guid = kCourierGuid;
            id.type_id = kCourierNpc;
            id.name = "Test Courier";
            mw::UnitStats st;
            st.level = 1;
            st.max_health = 100;
            st.faction = mw::Faction::kFriendly;
            mw::Position at;
            at.x = kSpawnX;
            at.y = kSpawnY;
            at.z = 0.0f;
            world.world_state().add_world_entity(id, st, kCourierNpc, at);
            check("spawned courier guid resolves to its npc_template id",
                  world.world_state().npc_template_for_guid(kCourierGuid).value_or(0) ==
                      kCourierNpc);
        }

        // TEST WORLD_HELLO: promote IN-WORLD without a grant DB — emplace the quest log
        // over the synthetic store, spawn authoritative movement, and AoI-enter at the
        // courier's spawn (so it is visible). The EgressFn writes relayed frames straight
        // to the socket.
        dispatcher.on(mn::Opcode::WORLD_HELLO,
                      [](net::Session& sess, const mw::Frame& /*f*/, mw::ConnCtx& ctx) {
                          ctx.authenticated = true;
                          ctx.account_id = 1;
                          ctx.phase = mw::SessionPhase::kInWorld;
                          ctx.char_id = kSelfGuid;
                          ctx.char_class = 1;
                          ctx.char_level = 1;
                          ctx.quests.emplace(quest_store);
                          mw::Position spawn;
                          spawn.x = kSpawnX;
                          spawn.y = kSpawnY;
                          spawn.z = 0.0f;
                          ctx.movement.emplace(spawn, /*spawn_time_ms=*/0);
                          ctx.movement->set_entity_guid(kSelfGuid);
                          mw::EntityIdentity id;
                          id.entity_guid = kSelfGuid;
                          id.type_id = 0;
                          mw::EnterResult er = ctx.world->enter(
                              id, spawn,
                              [&sess](mn::Opcode op, const std::vector<std::uint8_t>& payload) {
                                  sess.write_frame(mw::encode_frame(op, 0, payload));
                                  return true;
                              });
                          ctx.slot = er.slot;
                          ctx.entered = true;
                          ctx.movement->set_entity_guid(er.entity_guid);
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
            c.set_recv_timeout_ms(300);  // bound the drain-until-quiet reads
            std::uint32_t seq = 1;

            // Enter the world (courier becomes visible). Drain the initial frames.
            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, seq++, enc_world_hello()));
            drain(c);

            // --- ACCEPT the deliver quest (giver_guid=0 -> self-serve, no giver spawn
            //     needed). The accept-time marker push shows the courier greyed `?`
            //     (on the quest, deliver not yet credited). The deliver has NOT been
            //     credited: the throttle window opened by WORLD_HELLO's heartbeat is not
            //     yet elapsed, so the post-accept heartbeat does not credit. ---------
            c.send_frame(mw::encode_frame(mn::Opcode::QUEST_ACCEPT, seq++,
                                          enc_quest_accept(kQDeliver, /*giver=*/0)));
            Drained after_accept = drain(c);
            check("post-accept: courier marker is greyed `?` (TURN_IN_INCOMPLETE)",
                  after_accept.got_marker &&
                      after_accept.marker == mn::QuestMarkerKind::TURN_IN_INCOMPLETE);
            check("post-accept: deliver NOT yet credited (no complete QUEST_PROGRESS)",
                  !after_accept.deliver_complete);

            // --- STAND STILL past the heartbeat throttle window, then let ANY frame
            //     arrive (a benign QUEST_LOG stands in for the client's idle keepalive).
            //     The heartbeat now credits the deliver on VISUAL RANGE — NO gossip, NO
            //     movement — lighting the `?` to TURN_IN_READY within ~1s. -----------
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            c.send_frame(mw::encode_frame(mn::Opcode::QUEST_LOG, seq++, enc_quest_log_req()));
            Drained beat1 = drain(c);
            check("#861: deliver credits on VISUAL RANGE without a GOSSIP_HELLO "
                  "(complete QUEST_PROGRESS from the heartbeat)",
                  beat1.deliver_complete);
            check("#861: the turn-in `?` lights (TURN_IN_READY) proactively, no interact",
                  beat1.got_marker && beat1.marker == mn::QuestMarkerKind::TURN_IN_READY);
            check("no gossip menu was ever served (proactive, not on interaction)",
                  !after_accept.got_gossip_menu && !beat1.got_gossip_menu);

            // --- Another heartbeat window: the deliver is already satisfied
            //     (on_deliver idempotent) and the marker is unchanged (diffed) — so NO
            //     second credit and NO redundant QUEST_MARKER_UPDATE. Proves no
            //     double-credit and no per-frame wire spam. ------------------------
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            c.send_frame(mw::encode_frame(mn::Opcode::QUEST_LOG, seq++, enc_quest_log_req()));
            Drained beat2 = drain(c);
            check("no double-credit: deliver is not re-credited on a later heartbeat",
                  !beat2.deliver_complete);
            check("diffed: an unchanged marker is NOT re-pushed on a later heartbeat",
                  !beat2.got_marker);
        }  // client closes

        server.join();
        world.stop();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "marker-liveness test exception: %s\n", e.what());
        ++g_fail;
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    std::printf(g_fail == 0 ? "\nALL WORLDD MARKER-LIVENESS TESTS PASSED\n"
                            : "\n%d WORLDD MARKER-LIVENESS TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
