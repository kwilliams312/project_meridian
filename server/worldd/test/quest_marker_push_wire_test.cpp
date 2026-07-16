// SPDX-License-Identifier: Apache-2.0
//
// worldd — proactive OVERHEAD QUEST MARKER push WIRE test (#844 story A / #849).
// Proves the server pushes a QUEST_MARKER_UPDATE for a visible quest NPC over the
// REAL serve/dispatch loop + TLS 1.3 sockets, PROACTIVELY (no gossip interaction)
// and DIFFED (an unchanged marker never re-hits the wire), and that the icon tracks
// the player's quest state as they accept:
//   * on-sight (a MOVEMENT_INTENT recomputes the interest set) -> `!` AVAILABLE;
//   * after QUEST_ACCEPT (now on the giver's quest, objectives incomplete) ->
//     greyed `?` TURN_IN_INCOMPLETE;
//   * a second MOVEMENT_INTENT with no state change -> NO marker frame (diffed).
//
// CLEAN-ROOM: written from the server SAD, the world.fbs wire contract, the
// world_dispatch / world_state / npc module headers, and the OpenSSL public API
// only. No GPL source consulted (CONTRIBUTING.md).
//
// Self-contained (no DB): a test-installed WORLD_HELLO handler promotes the
// connection IN-WORLD without a grant DB — it emplaces a quest log over the
// placeholder store, spawns the session's authoritative movement, and REGISTERS it
// in the shared AoI relay (WorldState::enter) so a pre-spawned quest-giver NPC is in
// its interest set. So it always runs in the plain server ctest (like
// worldd-quest-loot-npc-wire). The 4-state marker computation is proven exhaustively
// by the DB-free meridian-npc-test; this test proves the LIVE push wiring.

#include "world_dispatch.h"
#include "world_state.h"

#include "npc_def.h"    // placeholder NPC ids (kNpcQuestGiver)
#include "quest_def.h"  // placeholder quest ids / store

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
namespace npc = meridian::npc;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

using Bytes = std::vector<std::uint8_t>;

// The M1 placeholder giver (gives + turns in the placeholder quest kQ1). Its marker
// walks `!` (available) -> greyed `?` (accepted, incomplete) -> none (turned in).
constexpr std::uint32_t kGiverNpc = mw::kPlaceholderNpcIdBase + 1;  // == npc::kNpcQuestGiver
constexpr std::uint32_t kQ1       = mw::kPlaceholderQuestIdBase + 1;
// A SPAWNED giver is addressed by its world-entity wire guid (kWorldEntityGuidBase
// band), NOT the raw template id — exactly what the client sends after ENTITY_ENTER.
constexpr std::uint64_t kGiverSpawnGuid = mw::kWorldEntityGuidBase;
constexpr std::uint64_t kSelfGuid       = 4242ULL;  // this session's synthetic char guid
// The session + NPC share this spawn so the NPC is at distance 0 (in AoI range).
constexpr float kSpawnX = -320.0f;
constexpr float kSpawnY = -320.0f;

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
            reinterpret_cast<const unsigned char*>("meridian-marker-wire-test"), -1, -1, 0);
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

// ---- Minimal TLS 1.3 IF-2 client (with a recv timeout so a "no frame" case does
//      not block) ------------------------------------------------------------
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

    // Bound blocking reads so draining "until quiet" terminates.
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
Bytes enc_movement_intent(std::uint32_t seq, std::uint64_t client_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateMovementIntent(b, seq, /*state_flags=*/0, kSpawnX, kSpawnY,
                                      /*z=*/0.0f, /*orientation=*/0.0f, client_time_ms));
    return bytes_of(b);
}
Bytes enc_quest_accept(std::uint32_t quest_id, std::uint64_t giver) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestAccept(b, quest_id, giver));
    return bytes_of(b);
}

// Drain every frame the server has queued until the recv times out, collecting the
// QUEST_MARKER_UPDATE frames (the only opcode this test asserts on). Other frames
// (ENTITY_ENTER at spawn, MOVEMENT_STATE, QUEST_ACCEPT_RESULT, QUEST_LOG) are read
// and ignored. Returns the markers in arrival order.
struct Marker {
    std::uint64_t npc_guid = 0;
    mn::QuestMarkerKind kind = mn::QuestMarkerKind::NONE;
};
std::vector<Marker> drain_markers(Client& c) {
    std::vector<Marker> out;
    for (;;) {
        std::optional<Bytes> frame = c.recv_frame();
        if (!frame) break;  // timed out — the server is quiet
        std::optional<mw::Frame> rf = mw::decode_frame(*frame);
        if (!rf) continue;
        if (rf->opcode != mn::Opcode::QUEST_MARKER_UPDATE) continue;
        Bytes pl(rf->payload, rf->payload + rf->payload_len);
        fb::Verifier v(pl.data(), pl.size());
        if (!v.VerifyBuffer<mn::QuestMarkerUpdate>(nullptr)) continue;
        const auto* m = fb::GetRoot<mn::QuestMarkerUpdate>(pl.data());
        out.push_back(Marker{m->npc_guid(), m->marker()});
    }
    return out;
}

const Marker* find_marker(const std::vector<Marker>& ms, std::uint64_t guid) {
    for (const Marker& m : ms)
        if (m.npc_guid == guid) return &m;
    return nullptr;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd overhead quest-marker PUSH wire test (#844/#849)\n");

    // Wiring proof (pure): a default Dispatcher routes the S->C marker opcode's
    // triggers (accept + movement) — QUEST_MARKER_UPDATE itself is server-pushed,
    // so it has no inbound handler (the client never sends it).
    {
        mw::Dispatcher d;
        check("QUEST_ACCEPT has a handler", d.has_handler(mn::Opcode::QUEST_ACCEPT));
        check("MOVEMENT_INTENT has a handler", d.has_handler(mn::Opcode::MOVEMENT_INTENT));
    }

    char tmpl[] = "/tmp/meridian-marker-XXXXXX";
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

        // Spawn the quest giver into the AoI world at the session's spawn, so a session
        // entering there immediately has it in its interest set. Its wire guid is in the
        // world-entity band and resolves back to the giver template.
        {
            mw::EntityIdentity id;
            id.entity_guid = kGiverSpawnGuid;
            id.type_id = kGiverNpc;
            id.name = "Test Giver";
            mw::UnitStats st;
            st.level = 1;
            st.max_health = 100;
            st.faction = mw::Faction::kFriendly;
            mw::Position at;
            at.x = kSpawnX;
            at.y = kSpawnY;
            at.z = 0.0f;
            world.world_state().add_world_entity(id, st, kGiverNpc, at);
            check("spawned giver guid resolves to its npc_template id",
                  world.world_state().npc_template_for_guid(kGiverSpawnGuid).value_or(0) ==
                      kGiverNpc);
        }

        // TEST WORLD_HELLO: promote IN-WORLD without a grant DB. Emplace a quest log
        // over the placeholder store, spawn authoritative movement, and REGISTER the
        // session in the AoI relay so the giver is in its interest set (the enter()
        // seam the real ENTER_WORLD handler uses). The EgressFn writes relayed frames
        // (the giver's ENTITY_ENTER) straight to this connection's socket.
        static mw::PlaceholderQuestStore quest_store;
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
                          // Register in the AoI relay (sets ctx.slot/entered) so the
                          // giver is visible and push_quest_markers has an interest set.
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
            c.set_recv_timeout_ms(400);  // bound the drain-until-quiet reads
            std::uint32_t seq = 1;

            // Promote in-world (the stub replies nothing; ENTITY_ENTER for the giver
            // may follow from enter()). Drain any initial frames.
            c.send_frame(mw::encode_frame(mn::Opcode::WORLD_HELLO, seq++, enc_world_hello()));
            drain_markers(c);  // no marker expected yet (enter push is the real handler's)

            // --- ON-SIGHT: a MOVEMENT_INTENT recomputes the interest set + pushes the
            //     proactive `!` for the visible giver (no gossip interaction). --------
            c.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, seq++,
                                          enc_movement_intent(1, /*client_time_ms=*/1000)));
            std::vector<Marker> after_move = drain_markers(c);
            const Marker* avail = find_marker(after_move, kGiverSpawnGuid);
            check("on-sight push emits a QUEST_MARKER_UPDATE for the giver",
                  avail != nullptr);
            check("on-sight marker is AVAILABLE (`!`)",
                  avail != nullptr && avail->kind == mn::QuestMarkerKind::AVAILABLE);

            // --- ACCEPT: the giver's `!` flips to a greyed `?` (now on the quest,
            //     objectives incomplete). ------------------------------------------
            c.send_frame(mw::encode_frame(mn::Opcode::QUEST_ACCEPT, seq++,
                                          enc_quest_accept(kQ1, kGiverSpawnGuid)));
            std::vector<Marker> after_accept = drain_markers(c);
            const Marker* incomplete = find_marker(after_accept, kGiverSpawnGuid);
            check("accept pushes a QUEST_MARKER_UPDATE for the giver",
                  incomplete != nullptr);
            check("post-accept marker is TURN_IN_INCOMPLETE (greyed `?`)",
                  incomplete != nullptr &&
                      incomplete->kind == mn::QuestMarkerKind::TURN_IN_INCOMPLETE);

            // --- DIFF: a second MOVEMENT_INTENT with no quest-state change must NOT
            //     re-push the (unchanged) marker. --------------------------------
            c.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, seq++,
                                          enc_movement_intent(2, /*client_time_ms=*/1200)));
            std::vector<Marker> after_move2 = drain_markers(c);
            check("unchanged marker is NOT re-pushed (diffed)",
                  find_marker(after_move2, kGiverSpawnGuid) == nullptr);
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

    std::printf(g_fail == 0 ? "\nALL WORLDD QUEST-MARKER PUSH WIRE TESTS PASSED\n"
                            : "\n%d WORLDD QUEST-MARKER PUSH WIRE TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
