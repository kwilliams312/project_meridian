// SPDX-License-Identifier: Apache-2.0
//
// worldd — LIVE MapTick level-up → VITALS_UPDATE egress integration test (issue
// #437, UI-01 HUD; part of the client HUD epic #24). The sibling of the QST-01
// live-credit test (quest_live_credit_it.cpp, #396): the SAME MapTick↔session
// event-bus seam, a different payload.
//
// WHAT THIS PROVES ON THE LIVE PATH: a kill routed through a REAL MapTick that LEVELS
// a live player up produces a typed kVitalsChanged TickEvent carrying the new
// authoritative vitals; routed through the SAME route_vitals_events() the world
// thread runs, into the SAME vitals-egress registry the live session drains on its IO
// worker (poll_vitals_egress), it MIRRORS the new level + max-health/max-power onto
// the session's WorldState unit and pushes a VITALS_UPDATE to the LEVELER's own client
// (its HUD player frame) — and NOT to a different registered guid (per-leveler
// isolation). So the HUD reflects a MapTick-driven level-up at once instead of lagging
// until a later live-path delta (the #430/#436 gap this closes).
//
// The level-up chain is exercised end-to-end: a REAL MapTick grinds a level-1 player
// up one level by killing creatures (CHR-03 #360), producing the kVitalsChanged delta
// the world loop routes — so the test faithfully mirrors the production seam without
// needing the world thread to spawn content.
//
// Self-contained (no DB): a test-installed WORLD_HELLO handler promotes the connection
// IN-WORLD (AoI-enters with a REAL sealed SessionEgress so broadcast_vitals reaches the
// wire, and registers the session on the vitals-egress bus) without a grant DB. So it
// always runs in the plain server ctest (like worldd-quest-live-credit). The DB-free
// vitals broadcast LOGIC is proven by worldd-vitals-test; the ENTER_WORLD self-vitals
// wiring by worldd-self-vitals.
//
// CLEAN-ROOM: written from the server SAD, the world.fbs wire contract, and the worldd
// module headers only. No GPL source consulted (CONTRIBUTING.md).

#include "world_dispatch.h"
#include "world_session.h"
#include "world_state.h"

#include "ability_store.h"
#include "combat_unit.h"
#include "creature_ai.h"
#include "leveling.h"
#include "map_tick.h"
#include "movement_validation.h"
#include "vitals_egress.h"

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
namespace mc = meridian::worldd::movement;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

using Bytes = std::vector<std::uint8_t>;

// This session's synthetic char guid + a DIFFERENT registered guid (isolation check).
constexpr std::uint32_t kSelfGuid  = 4343ULL;
constexpr std::uint32_t kOtherGuid = 8888ULL;
constexpr std::uint8_t  kRuncaller = 2;  // roster class 2 → MANA pool (rich power assertion)

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
            reinterpret_cast<const unsigned char*>("meridian-vitals-levelup-it"), -1, -1, 0);
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

// A decoded VitalsUpdate captured off the wire.
struct Vitals {
    bool got = false;
    std::uint64_t guid = 0;
    std::uint32_t health = 0, max_health = 0, power = 0, max_power = 0;
    mn::PowerType power_type = mn::PowerType::NONE;
    std::uint16_t level = 0;
};

// Read frames until a VITALS_UPDATE arrives (skipping the interleaved reply frames —
// MOVEMENT_STATE / etc.), or an empty Vitals if the stream ends first.
Vitals recv_vitals(Client& c) {
    for (int i = 0; i < 12; ++i) {
        std::optional<Bytes> fr = c.recv_frame();
        if (!fr) return {};
        std::optional<mw::Frame> rf = mw::decode_frame(*fr);
        if (!rf) continue;
        if (rf->opcode != mn::Opcode::VITALS_UPDATE) continue;  // skip MOVEMENT_STATE etc.
        Bytes pl(rf->payload, rf->payload + rf->payload_len);
        const auto* u = decode<mn::VitalsUpdate>(pl);
        if (!u) continue;
        return Vitals{true, u->entity_guid(), u->health(), u->max_health(), u->power(),
                      u->max_power(), u->power_type(), u->level()};
    }
    return {};
}

mw::Position at(float x, float y) {
    mw::Position p;
    p.x = x;
    p.y = y;
    p.z = mc::kFlatGroundZ;
    return p;
}

// Drive a REAL MapTick to grind `killer` (a level-1 Runcaller) up until it LEVELS UP,
// returning ALL the tick deltas produced. With report_vitals ON the level-up delta is
// tagged kVitalsChanged carrying the new authoritative vitals (the seam under test).
std::vector<mw::TickEvent> map_tick_level_up(mw::ObjectGuid killer) {
    const mw::AbilityStore abilities = mw::load_placeholder_ability_store();
    mw::MapTick mt(abilities, /*rng_seed=*/0x0437ULL, /*dt_ms=*/1600);
    mt.set_report_vitals(true);  // UI-01 event-bus: tag the level-up as kVitalsChanged

    // A level-1 Runcaller — the class curve drives the level-up stat growth (#360).
    mt.add_player(killer, at(0, 0), mw::placeholder_player_stats(kRuncaller, 1), kRuncaller);
    mw::Unit* pu = mt.unit_for_guid(killer);

    std::vector<mw::TickEvent> all;
    // Grind level-1 creatures until the player gains a level (two same-level kills
    // cross the level-1 threshold: xp_for_kill(1,1)=25, xp_to_next_level(1)=50).
    for (int guard = 0; guard < 100 && pu->level() < 2; ++guard) {
        mw::CreatureSpawnDef d;
        d.template_id = 5000;
        d.level = 1;
        d.faction = mw::Faction::kHostile;
        d.home = at(2, 0);
        d.aggro_base_radius = 0;
        d.leash_radius = 1000;
        d.respawn_ms = 999999;
        d.move_speed = 0;
        d.patrol_mode = mw::PatrolMode::kStationary;
        const mw::ObjectGuid crt = mt.add_creature(d);
        mw::Unit* cu = mt.unit_for_guid(crt);
        cu->set_max_health(8);  // one melee strike is lethal
        for (int t = 0; t < 12 && cu->is_alive(); ++t) {
            mt.enqueue_cast(mw::AbilityUseCmd{killer, mw::kPlaceholderMeleeStrikeId, crt});
            std::vector<mw::TickEvent> deltas = mt.advance();
            for (mw::TickEvent& ev : deltas) all.push_back(std::move(ev));
        }
    }
    return all;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);
    std::printf("worldd LIVE MapTick level-up → VITALS_UPDATE egress test (#437, UI-01)\n");

    char tmpl[] = "/tmp/meridian-vlu-XXXXXX";
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

        // Also register a DIFFERENT session guid on the vitals bus (no live client) so
        // we can assert a level-up for kSelfGuid is NOT routed to it.
        world.vitals_egress().register_session(kOtherGuid);

        // TEST WORLD_HELLO: promote IN-WORLD without a grant DB — spawn a Runcaller at
        // (64,64,0), AoI-enter with kSelfGuid through a REAL sealed SessionEgress (so
        // broadcast_vitals reaches THIS client), and register on the vitals-egress bus.
        dispatcher.on(mn::Opcode::WORLD_HELLO,
                      [](net::Session& sess, const mw::Frame& /*f*/, mw::ConnCtx& ctx) {
                          ctx.authenticated = true;
                          ctx.account_id = 1;
                          ctx.phase = mw::SessionPhase::kInWorld;
                          ctx.char_id = kSelfGuid;
                          ctx.char_class = kRuncaller;
                          ctx.char_level = 1;
                          mw::Position spawn = at(-320.0f, -320.0f);  // Zone-01 centre (#562)
                          ctx.movement.emplace(spawn, /*spawn_time_ms=*/0);
                          ctx.movement->set_entity_guid(kSelfGuid);
                          // Seal a real s2c egress (the §5.2 WorldSession seam) so the
                          // relay/broadcast path writes to THIS client's socket.
                          ctx.session.emplace(Bytes(32, 0xAB));
                          ctx.egress = std::make_shared<mw::SessionEgress>(sess, *ctx.session);
                          if (ctx.world != nullptr) {
                              std::shared_ptr<mw::SessionEgress> egress = ctx.egress;
                              mw::EntityIdentity id;
                              id.entity_guid = kSelfGuid;  // kept (non-zero) by enter()
                              id.type_id = kRuncaller;
                              id.char_class = kRuncaller;
                              mw::EnterResult er = ctx.world->enter(
                                  id, spawn,
                                  [egress](mn::Opcode op, const std::vector<std::uint8_t>& p) {
                                      return egress->emit(op, p);
                                  });
                              ctx.slot = er.slot;
                              ctx.entered = true;
                              ctx.movement->set_entity_guid(er.entity_guid);
                          }
                          if (ctx.vitals_egress != nullptr) {
                              ctx.credit_guid = ctx.movement->entity_guid();
                              ctx.vitals_token =
                                  ctx.vitals_egress->register_session(ctx.credit_guid);
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

            // ===== LEVEL-UP: real MapTick grinds kSelfGuid up a level ==============
            std::vector<mw::TickEvent> deltas = map_tick_level_up(kSelfGuid);

            // The MapTick tagged the level-up as kVitalsChanged carrying the new vitals.
            mw::VitalsSnapshot snap{};
            std::size_t vitals_events = 0;
            for (const mw::TickEvent& ev : deltas) {
                if (ev.kind == mw::TickEventKind::kVitalsChanged &&
                    ev.vitals.guid == kSelfGuid) {
                    ++vitals_events;
                    snap = ev.vitals;
                }
            }
            check("MapTick emitted a kVitalsChanged for the leveler", vitals_events == 1);
            check("the snapshot is a real level-up (level >= 2)", snap.level >= 2);
            check("the snapshot tops the leveler off (health == max_health)",
                  snap.health == snap.max_health && snap.max_health > 0);
            check("the snapshot carries a mana pool (power == max_power > 0)",
                  snap.power == snap.max_power && snap.max_power > 0);
            const std::uint32_t l1_hp = mw::placeholder_player_stats(kRuncaller, 1).max_health;
            check("the new max_health grew above the level-1 curve",
                  snap.max_health > l1_hp);

            // BARRIER: a legal movement step whose MOVEMENT_STATE echo proves the
            // server has PROCESSED the WORLD_HELLO stub (AoI enter + vitals-egress
            // REGISTRATION of kSelfGuid) — so the push below is retained, not dropped
            // as unregistered. Spawn (-320) → -319.90 is one legal walk step.
            c.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, seq++,
                                          enc_movement_intent(/*seq=*/10, /*Walk=*/1,
                                                              -319.90f, -320.0f, 0.0f,
                                                              /*client_time_ms=*/100)));
            check("session entered + registered (movement echo received)",
                  c.recv_frame().has_value());

            // Route the deltas through the SAME path the world thread runs, into the
            // SAME registry the session drains (now that kSelfGuid is registered).
            mw::route_vitals_events(deltas, world.vitals_egress());

            // Isolation: the level-up (guid = kSelfGuid) leaves the OTHER registered
            // guid's queue empty — a vitals change reaches ONLY the owning session.
            check("a level-up does NOT route to a different session's guid",
                  !world.vitals_egress().drain_vitals(kOtherGuid).has_value());

            // Poke the session with a second legal step: any handled frame drains its
            // pending vitals (poll_vitals_egress) → mirrors onto the WorldState unit →
            // pushes the VITALS_UPDATE to this client. -319.90 → -319.80 is one legal step.
            c.send_frame(mw::encode_frame(mn::Opcode::MOVEMENT_INTENT, seq++,
                                          enc_movement_intent(/*seq=*/11, /*Walk=*/1,
                                                              -319.80f, -319.90f, 0.0f,
                                                              /*client_time_ms=*/200)));

            const Vitals v = recv_vitals(c);
            check("1: the leveler received a VITALS_UPDATE", v.got);
            check("1: the vitals are FOR this character (matching guid)",
                  v.got && v.guid == kSelfGuid);
            check("2: the wire level is the new level (matches the snapshot)",
                  v.got && v.level == snap.level && v.level >= 2);
            check("3: the wire max_health is the new max (matches the snapshot)",
                  v.got && v.max_health == snap.max_health && v.max_health > l1_hp);
            check("3: the leveler is at full health (health == max_health)",
                  v.got && v.health == v.max_health);
            check("4: the wire max_power is the new max (matches the snapshot)",
                  v.got && v.max_power == snap.max_power && v.max_power > 0);
            check("4: the leveler is at full mana (power == max_power)",
                  v.got && v.power == v.max_power);
            check("4: the power_type is MANA (Runcaller)",
                  v.got && v.power_type == mn::PowerType::MANA);
        }  // client closes

        server.join();
        world.stop();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vitals-levelup test exception: %s\n", e.what());
        ++g_fail;
    }

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(dir);

    std::printf(g_fail == 0 ? "\nALL WORLDD VITALS LEVEL-UP EGRESS TESTS PASSED\n"
                            : "\n%d WORLDD VITALS LEVEL-UP EGRESS TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
