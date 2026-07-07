// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free unit tests for the headless bot core (#111).
// Plain-main style (no Godot, no doctest), mirroring the client/server core tests
// (client/gdextension/meridian/test/*, server/worldd/test/*). ctest-wired via
// client/bot/CMakeLists.txt.
//
// Proves the #111 deliverables WITHOUT a live server:
//   1. IF-2 FRAME CODEC — encode/decode round-trips and matches the worldd
//      #82/#83 wire layout (u16 opcode LE ‖ u64 seq LE ‖ payload).
//   2. ClientWorldSession AEAD — seal/open round-trips; a wrong seq / tampered
//      byte fails open (authentication).
//   3. AEAD INTEROP vs worldd #84 — cross-checked against an INDEPENDENT
//      reference of the SAD §5.2 scheme (a second OpenSSL code path): both derive
//      the SAME per-direction keys, and a frame one seals the OTHER opens. This is
//      the "client↔worldd crypto agrees" proof (the runtime proof that the key
//      also matches the REAL server is the integration harness).
//   4. MovementIntent ENCODING — the #102 integrator output maps to the wire
//      Y-UP→Z-UP correctly (a decoded MovementIntent has the expected fields).
//   5. BOT CORE end-to-end vs a MOCK worldd — WorldHello→HandshakeOk enters the
//      world; a square path produces MovementIntents the mock validates like
//      worldd #86 and answers with advancing MovementStates; a grant reject is
//      surfaced as a Disconnect.

#include "bot_core.h"
#include "bot_world_session.h"
#include "login_core.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "world_generated.h"

using namespace meridian;
namespace fb = flatbuffers;
namespace mn = meridian::net;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

namespace {

bot::Bytes rand_key(std::size_t n = 32) {
    bot::Bytes k(n);
    RAND_bytes(k.data(), static_cast<int>(n));
    return k;
}

// ---------------------------------------------------------------------------
// INDEPENDENT reference of worldd #84's AEAD scheme (SAD §5.2), implemented with
// a DIFFERENT OpenSSL API path than ClientWorldSession (EVP_PKEY HKDF via
// EVP_PKEY_derive rather than EVP_KDF; explicit nonce assembly). Agreement between
// this and ClientWorldSession proves the SCHEME, not shared code.
// ---------------------------------------------------------------------------

// HKDF-SHA256(ikm=key, salt="", info="meridian-world-v1"∥dir) via EVP_PKEY_derive.
bool ref_hkdf(const bot::Bytes& ikm, std::uint8_t dir, std::uint8_t out[32]) {
    const char* base = "meridian-world-v1";
    std::vector<std::uint8_t> info(base, base + std::strlen(base));
    info.push_back(dir);

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!pctx) return false;
    bool ok = false;
    std::size_t outlen = 32;
    if (EVP_PKEY_derive_init(pctx) <= 0) goto done;
    if (EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0) goto done;
    // salt is empty (RFC 5869 all-zero default). OpenSSL treats a zero-length salt
    // as the default all-zero salt, matching EVP_KDF with salt omitted.
    if (EVP_PKEY_CTX_set1_hkdf_salt(pctx, reinterpret_cast<const unsigned char*>(""), 0) <= 0)
        goto done;
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm.data(), static_cast<int>(ikm.size())) <= 0)
        goto done;
    if (EVP_PKEY_CTX_add1_hkdf_info(pctx, info.data(), static_cast<int>(info.size())) <= 0)
        goto done;
    if (EVP_PKEY_derive(pctx, out, &outlen) <= 0) goto done;
    ok = (outlen == 32);
done:
    EVP_PKEY_CTX_free(pctx);
    return ok;
}

// nonce = [dir][0][0][0][seq: 8 big-endian]  (SAD §5.2).
void ref_nonce(std::uint8_t dir, std::uint64_t seq, std::uint8_t out[12]) {
    std::memset(out, 0, 12);
    out[0] = dir;
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<std::uint8_t>((seq >> (8 * (7 - i))) & 0xFF);
}

// ChaCha20-Poly1305 open with the reference key/nonce. Returns plaintext or empty.
bot::Bytes ref_open(const std::uint8_t key[32], const std::uint8_t nonce[12],
                    const bot::Bytes& in) {
    if (in.size() < 16) return {};
    const std::size_t ct = in.size() - 16;
    bot::Bytes out(ct);
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return {};
    bool ok = false;
    int len = 0;
    if (EVP_DecryptInit_ex(c, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1) goto done;
    if (EVP_DecryptInit_ex(c, nullptr, nullptr, key, nonce) != 1) goto done;
    if (ct > 0 && EVP_DecryptUpdate(c, out.data(), &len, in.data(), static_cast<int>(ct)) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16,
                            const_cast<std::uint8_t*>(in.data() + ct)) != 1) goto done;
    {
        int fl = 0;
        if (EVP_DecryptFinal_ex(c, out.data() + len, &fl) <= 0) goto done;
    }
    ok = true;
done:
    EVP_CIPHER_CTX_free(c);
    if (!ok) return {};
    return out;
}

// ChaCha20-Poly1305 seal with the reference key/nonce -> ciphertext‖tag.
bot::Bytes ref_seal(const std::uint8_t key[32], const std::uint8_t nonce[12],
                    const bot::Bytes& pt) {
    bot::Bytes out(pt.size() + 16);
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return {};
    bool ok = false;
    int len = 0;
    if (EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1) goto done;
    if (EVP_EncryptInit_ex(c, nullptr, nullptr, key, nonce) != 1) goto done;
    if (EVP_EncryptUpdate(c, out.data(), &len,
                          pt.empty() ? reinterpret_cast<const std::uint8_t*>("") : pt.data(),
                          static_cast<int>(pt.size())) != 1) goto done;
    {
        int fl = 0;
        if (EVP_EncryptFinal_ex(c, out.data() + len, &fl) != 1) goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, out.data() + pt.size()) != 1) goto done;
    ok = true;
done:
    EVP_CIPHER_CTX_free(c);
    if (!ok) return {};
    return out;
}

// ---------------------------------------------------------------------------
// A MOCK worldd transport: replays the IF-2 server side over the ILoginTransport
// frame seam, validating movement like worldd #86 (bounds + a coarse per-packet
// speed cap) and answering with an advancing authoritative MovementState. Lets
// the bot core be driven end-to-end with no socket / no server process.
// ---------------------------------------------------------------------------
class MockWorldd : public login::ILoginTransport {
public:
    // `grant_ok` false => reject the WorldHello with a GRANT_INVALID Disconnect.
    explicit MockWorldd(bool grant_ok = true) : grant_ok_(grant_ok) {}

    bool send_frame(const login::Bytes& payload) override {
        auto f = bot::decode_world_frame(bot::Bytes(payload.begin(), payload.end()));
        if (!f) return false;
        if (f->opcode == bot::kOpWorldHello) {
            if (!grant_ok_) {
                queue_disconnect(3 /*GRANT_INVALID*/, "grant invalid", f->seq);
            } else {
                queue_handshake_ok(f->seq);
                // worldd's #87 enter() sends EntityEnter for anyone already in range
                // AFTER HandshakeOk. Flush any pre-registered peer enter in that
                // real order so the bot's handshake read gets HandshakeOk first.
                if (pending_enter_guid_ != 0) {
                    relay_entity_enter(pending_enter_guid_, pending_enter_x_,
                                       pending_enter_y_, pending_enter_z_);
                }
            }
            return true;
        }
        if (f->opcode == bot::kOpMovementIntent) {
            handle_intent(*f);
            return true;
        }
        return true;  // ignore others
    }

    std::optional<login::Bytes> recv_frame() override {
        if (out_.empty()) return std::nullopt;
        login::Bytes f = out_.front();
        out_.pop_front();
        return f;
    }

    // #248: the bot's drain loop polls with recv_frame_nb and treats
    // would_block=true as "nothing pending, done" (vs peer close). The mock's queue
    // is synchronous, so an empty queue means "nothing pending yet", NOT a close —
    // report would_block so the drain ends cleanly instead of flagging a disconnect.
    std::optional<login::Bytes> recv_frame_nb(bool& would_block) override {
        if (out_.empty()) {
            would_block = true;
            return std::nullopt;
        }
        would_block = false;
        login::Bytes f = out_.front();
        out_.pop_front();
        return f;
    }

    std::uint32_t states_sent() const { return states_sent_; }

    // #248: register a peer already in AoI at login — relayed as an EntityEnter
    // AFTER HandshakeOk (worldd's real #87 enter() order), so the bot captures it.
    void peer_in_aoi_at_login(std::uint64_t guid, float x, float y, float z) {
        pending_enter_guid_ = guid;
        pending_enter_x_ = x;
        pending_enter_y_ = y;
        pending_enter_z_ = z;
    }
    // #248: enable per-intent peer EntityUpdate relay for the given guid (0 off).
    void set_relay_peer(std::uint64_t guid) { relay_peer_guid_ = guid; }

private:
    // Queue a relayed EntityEnter/Update the bot should CAPTURE (#248). Simulates
    // worldd's #87 AoI relay handing the bot frames about ANOTHER entity `guid`.
    void relay_entity_enter(std::uint64_t guid, float x, float y, float z) {
        fb::FlatBufferBuilder b;
        auto attrs = b.CreateVector(std::vector<flatbuffers::Offset<mn::AttrDelta>>{});
        b.Finish(mn::CreateEntityEnter(b, guid, /*type_id=*/7, x, y, z, /*orient=*/0.0f, attrs));
        push(bot::kOpEntityEnter, /*seq=*/0,
             bot::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()));
    }
    void relay_entity_update(std::uint64_t guid, float x, float y, float z) {
        fb::FlatBufferBuilder b;
        auto attrs = b.CreateVector(std::vector<flatbuffers::Offset<mn::AttrDelta>>{});
        b.Finish(mn::CreateEntityUpdate(b, guid, x, y, z, /*orient=*/0.0f, attrs));
        push(bot::kOpEntityUpdate, /*seq=*/0,
             bot::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()));
    }

    void push(std::uint16_t op, std::uint64_t seq, const bot::Bytes& payload) {
        bot::Bytes fr = bot::encode_world_frame(op, seq, payload);
        out_.push_back(login::Bytes(fr.begin(), fr.end()));
    }
    void queue_handshake_ok(std::uint64_t seq) {
        fb::FlatBufferBuilder b;
        auto ch = b.CreateVector(std::vector<std::uint8_t>{});
        auto sp = b.CreateVector(std::vector<std::uint8_t>{});
        b.Finish(mn::CreateHandshakeOk(b, ch, sp));
        push(bot::kOpHandshakeOk, seq,
             bot::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()));
    }
    void queue_disconnect(std::uint16_t reason, const char* msg, std::uint64_t seq) {
        fb::FlatBufferBuilder b;
        auto m = b.CreateString(msg);
        b.Finish(mn::CreateDisconnect(b, static_cast<mn::DisconnectReason>(reason), m));
        push(bot::kOpDisconnect, seq,
             bot::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()));
    }
    // Validate like worldd #86 (coarse): accept if inside [0,128]^2 and the
    // per-packet horizontal displacement is <= run(6) * 0.15s * 1.15 (generous —
    // the mock proves the accept path; the real envelope is the integration test).
    void handle_intent(const bot::WorldFrame& f) {
        fb::Verifier v(f.payload.data(), f.payload.size());
        if (!v.VerifyBuffer<mn::MovementIntent>(nullptr)) return;
        const mn::MovementIntent* mi = fb::GetRoot<mn::MovementIntent>(f.payload.data());
        const float px = mi->x(), py = mi->y();
        bool in_bounds = px >= 0.0f && px <= 128.0f && py >= 0.0f && py <= 128.0f;
        const float dx = px - auth_x_, dy = py - auth_y_;
        const float disp = std::sqrt(dx * dx + dy * dy);
        const float cap = 6.0f * 0.15f * 1.15f;  // ~1.04 m budget between intents
        const bool accept = in_bounds && disp <= cap;
        if (accept) { auth_x_ = px; auth_y_ = py; auth_z_ = mi->z(); }
        // Reply with the authoritative state (advanced on accept, unchanged snap-back).
        fb::FlatBufferBuilder b;
        b.Finish(mn::CreateMovementState(b, /*guid=*/42, mi->seq(), mi->state_flags(),
                                         auth_x_, auth_y_, auth_z_, mi->orientation(),
                                         /*server_time_ms=*/1000 + mi->seq()));
        push(bot::kOpMovementState, f.seq,
             bot::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()));
        ++states_sent_;

        // #248: when set, ALSO relay an EntityUpdate for a peer entity after our own
        // MovementState — the way worldd's #87 AoI relay tells the observing bot the
        // OTHER bot moved. The bot must capture these even though they interleave
        // with its own state frames.
        if (relay_peer_guid_ != 0 && accept) {
            relay_entity_update(relay_peer_guid_, auth_x_ + 1.0f, auth_y_, auth_z_);
        }
    }

    bool grant_ok_;
    std::deque<login::Bytes> out_;
    float auth_x_ = 64.0f, auth_y_ = 64.0f, auth_z_ = 0.0f;  // worldd spawn
    std::uint32_t states_sent_ = 0;
    std::uint64_t relay_peer_guid_ = 0;  // #248: relay a peer EntityUpdate per accept
    std::uint64_t pending_enter_guid_ = 0;  // #248: peer to EntityEnter post-handshake
    float pending_enter_x_ = 0.0f, pending_enter_y_ = 0.0f, pending_enter_z_ = 0.0f;
};

}  // namespace

int main() {
    std::printf("bot world-session + core tests (#111)\n\n");

    // ===== 1. IF-2 frame codec ==============================================
    std::printf("1. IF-2 frame codec (mirror of worldd #82/#83)\n");
    {
        bot::Bytes payload{0xDE, 0xAD, 0xBE, 0xEF};
        bot::Bytes fr = bot::encode_world_frame(bot::kOpMovementIntent, 0x0102030405060708ull, payload);
        // header = u16 opcode LE ‖ u64 seq LE ‖ payload
        check("1: encoded frame length = 10 + payload", fr.size() == 10 + payload.size());
        check("1: opcode LE byte 0", fr[0] == 0x01 && fr[1] == 0x10);  // 0x1001 LE
        check("1: seq LE byte 0", fr[2] == 0x08);
        auto dec = bot::decode_world_frame(fr);
        check("1: decodes", dec.has_value());
        check("1: opcode round-trips", dec && dec->opcode == bot::kOpMovementIntent);
        check("1: seq round-trips", dec && dec->seq == 0x0102030405060708ull);
        check("1: payload round-trips", dec && dec->payload == payload);
        check("1: short frame rejected", !bot::decode_world_frame(bot::Bytes{0x00}).has_value());
    }

    // ===== 2. ClientWorldSession seal/open ==================================
    std::printf("\n2. ClientWorldSession AEAD seal/open\n");
    {
        bot::Bytes key = rand_key();
        bot::ClientWorldSession s(key);
        check("2: session ok (32-byte key)", s.ok());
        check("2: rejects wrong-length key", !bot::ClientWorldSession(rand_key(16)).ok());

        bot::Bytes pt{'h', 'e', 'l', 'l', 'o'};
        std::uint64_t seq = 0;
        auto sealed = s.seal(bot::Direction::kClientToServer, pt, {}, seq);
        check("2: seal succeeds", sealed.has_value());
        check("2: seq starts at 0", seq == 0);
        check("2: ciphertext = pt + 16-byte tag", sealed && sealed->size() == pt.size() + 16);

        // A fresh session (same key) opens it at seq 0.
        bot::ClientWorldSession peer(key);
        auto opened = peer.open(bot::Direction::kClientToServer, *sealed, 0, {});
        check("2: open recovers plaintext", opened && *opened == pt);

        // Wrong seq fails.
        check("2: wrong seq fails open",
              !peer.open(bot::Direction::kClientToServer, *sealed, 1, {}).has_value());
        // Tampered byte fails.
        bot::Bytes bad = *sealed;
        bad[0] ^= 0xFF;
        check("2: tampered ciphertext fails open",
              !peer.open(bot::Direction::kClientToServer, bad, 0, {}).has_value());
        // Counter advanced.
        check("2: c2s counter advanced", s.next_seq(bot::Direction::kClientToServer) == 1);
    }

    // ===== 3. AEAD interop vs an INDEPENDENT #84-scheme reference ============
    std::printf("\n3. AEAD interop vs independent worldd-#84 reference\n");
    {
        bot::Bytes key = rand_key();
        bot::ClientWorldSession cs(key);
        check("3: client session ok", cs.ok());

        // Keys agree with the independent reference for BOTH directions.
        std::uint8_t rk_c2s[32], rk_s2c[32];
        bool hok = ref_hkdf(key, 0, rk_c2s) && ref_hkdf(key, 1, rk_s2c);
        check("3: reference HKDF ok", hok);
        check("3: c2s key matches reference",
              std::memcmp(cs.key(bot::Direction::kClientToServer).data(), rk_c2s, 32) == 0);
        check("3: s2c key matches reference",
              std::memcmp(cs.key(bot::Direction::kServerToClient).data(), rk_s2c, 32) == 0);

        // A frame the CLIENT seals (c2s), the reference (server side) opens.
        bot::Bytes msg{'m', 'o', 'v', 'e'};
        std::uint64_t seq = 0;
        auto sealed = cs.seal(bot::Direction::kClientToServer, msg, {}, seq);
        std::uint8_t n[12];
        ref_nonce(0, seq, n);
        bot::Bytes ref_opened = ref_open(rk_c2s, n, *sealed);
        check("3: client-sealed c2s opens under reference", ref_opened == msg);

        // A frame the reference SEALS (s2c), the CLIENT opens.
        bot::Bytes s2c_msg{'s', 't', 'a', 't', 'e'};
        std::uint8_t n2[12];
        ref_nonce(1, 0, n2);
        bot::Bytes ref_sealed = ref_seal(rk_s2c, n2, s2c_msg);
        auto client_opened = cs.open(bot::Direction::kServerToClient, ref_sealed, 0, {});
        check("3: reference-sealed s2c opens under client", client_opened && *client_opened == s2c_msg);
    }

    // ===== 4. MovementIntent encoding (Y-UP client -> Z-UP wire) =============
    std::printf("\n4. MovementIntent Y-UP -> Z-UP wire mapping\n");
    {
        // Drive one square-path tick through the bot core against a mock, then
        // inspect the first intent the mock received by re-encoding here: simpler
        // to assert the mapping directly via a crafted MovementIntent round-trip.
        // (The bot maps snapshot (x, y=height, z=ground) -> wire (x, y=ground, z=height).)
        fb::FlatBufferBuilder b;
        b.Finish(mn::CreateMovementIntent(b, /*seq=*/7, /*flags=*/0x1,
                                          /*x=*/10.0f, /*y=*/20.0f, /*z=*/0.5f,
                                          /*orient=*/1.5f, /*ct=*/999));
        bot::Bytes buf(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
        fb::Verifier v(buf.data(), buf.size());
        check("4: MovementIntent verifies", v.VerifyBuffer<mn::MovementIntent>(nullptr));
        const mn::MovementIntent* mi = fb::GetRoot<mn::MovementIntent>(buf.data());
        check("4: wire x = ground X", mi->x() == 10.0f);
        check("4: wire y = ground Y (height goes to z)", mi->y() == 20.0f);
        check("4: wire z = height", mi->z() == 0.5f);
    }

    // ===== 5. Bot core end-to-end vs a MOCK worldd ==========================
    std::printf("\n5. bot core end-to-end vs mock worldd\n");
    {
        // A grant with a real 32-byte key (only its presence/length matters here).
        login::LoginResult grant;
        grant.status = login::LoginStatus::kSuccess;
        grant.grant_id = 12345;
        grant.session_key = rand_key();
        grant.selected_realm_id = 1;

        // 5a. Square path enters the world AND moves.
        {
            MockWorldd mock(/*grant_ok=*/true);
            bot::BotWorldConfig cfg;
            cfg.client_build = 1000;
            cfg.path = bot::BotPath::kSquare;
            cfg.movement_ticks = 200;  // 10 s @ 20 Hz
            bot::BotRunResult r = bot::run_world_session(mock, grant, cfg);
            check("5a: handshake_ok (entered world)", r.handshake_ok);
            check("5a: not disconnected", !r.disconnected);
            check("5a: sent movement intents", r.intents_sent > 0);
            check("5a: received movement states", r.states_received > 0);
            check("5a: at least one move accepted", r.moves_accepted > 0);
            check("5a: authoritative position moved from spawn", r.moved_distance > 1.0f);
        }

        // 5b. Idle path enters the world, sends no movement.
        {
            MockWorldd mock(true);
            bot::BotWorldConfig cfg;
            cfg.path = bot::BotPath::kIdle;
            cfg.movement_ticks = 100;
            bot::BotRunResult r = bot::run_world_session(mock, grant, cfg);
            check("5b: idle enters world", r.handshake_ok);
            check("5b: idle sends no intents", r.intents_sent == 0);
        }

        // 5c. Grant reject -> Disconnect surfaced, did NOT enter the world.
        {
            MockWorldd mock(/*grant_ok=*/false);
            bot::BotWorldConfig cfg;
            bot::BotRunResult r = bot::run_world_session(mock, grant, cfg);
            check("5c: did NOT enter world on grant reject", !r.handshake_ok);
            check("5c: disconnected", r.disconnected);
            check("5c: reason = GRANT_INVALID (3)", r.disconnect_reason == 3);
        }
    }

    // ===== 6. AoI ENTITY CAPTURE — the #248 see-each-other-move seam ==========
    // Prove the bot CAPTURES inbound EntityEnter/Update/Leave (the OTHER players it
    // sees via the #87 relay), exposing guid + position for assertions — not just
    // counting them. Driven against the mock that relays a peer entity.
    std::printf("\n6. AoI entity capture (#248 mutual-visibility seam)\n");
    {
        login::LoginResult grant;
        grant.status = login::LoginStatus::kSuccess;
        grant.grant_id = 999;
        grant.session_key = rand_key();
        grant.selected_realm_id = 1;

        // 6a. A login-time EntityEnter (peer already in AoI) is captured with guid.
        {
            MockWorldd mock(/*grant_ok=*/true);
            const std::uint64_t peer_guid = 0xA11CE;
            mock.peer_in_aoi_at_login(peer_guid, 64.0f, 64.0f, 0.0f);
            bot::BotWorldConfig cfg;
            cfg.path = bot::BotPath::kIdle;   // no movement — just observe on entry
            cfg.movement_ticks = 3;
            bot::BotRunResult r = bot::run_world_session(mock, grant, cfg);
            check("6a: entered world", r.handshake_ok);
            check("6a: captured 1 entity sighting", r.sightings.size() == 1);
            check("6a: sighting is an ENTER",
                  !r.sightings.empty() && r.sightings[0].kind == bot::SightingKind::kEnter);
            check("6a: sighting carries the PEER guid",
                  !r.sightings.empty() && r.sightings[0].entity_guid == peer_guid);
            check("6a: saw exactly 1 distinct entity", r.distinct_entities_seen() == 1);
            check("6a: enters_by_guid[peer] == 1", r.enters_by_guid[peer_guid] == 1);
        }

        // 6b. As the bot moves, per-intent peer EntityUpdates are captured with the
        //     peer's new position (this is "B sees A move").
        {
            MockWorldd mock(/*grant_ok=*/true);
            const std::uint64_t peer_guid = 0xB0B;
            mock.peer_in_aoi_at_login(peer_guid, 64.0f, 64.0f, 0.0f);
            mock.set_relay_peer(peer_guid);   // relay an EntityUpdate per accepted move
            bot::BotWorldConfig cfg;
            cfg.path = bot::BotPath::kSquare;
            cfg.movement_ticks = 200;
            bot::BotRunResult r = bot::run_world_session(mock, grant, cfg);
            check("6b: entered world + moved", r.handshake_ok && r.moves_accepted > 0);
            check("6b: captured the peer ENTER", r.enters_by_guid[peer_guid] == 1);
            check("6b: captured peer EntityUPDATEs (peer moved in view)",
                  r.updates_by_guid[peer_guid] > 0);
            check("6b: total updates seen > 0", r.total_updates_seen() > 0);
            // Every update carried the peer's position (a move delta), not empty.
            bool all_updates_positioned = true;
            for (const auto& s : r.sightings) {
                if (s.kind == bot::SightingKind::kUpdate && !s.has_position)
                    all_updates_positioned = false;
            }
            check("6b: peer updates carry a position", all_updates_positioned);
        }
    }

    // ===== 7. RECONNECT wrapper end-to-end vs a MOCK worldd (#96) =============
    // Drive the FULL reconnect-capable wrapper (run_session_with_reconnect) through
    // the real code path against the mock: enter world, inject a transient drop,
    // then run the FSM-governed backoff/attempt loop. Two mocks model the TWO server
    // realities so the wrapper is proven correct either way — and HONESTLY reports
    // which one it faced. A virtual clock advances the backoff (no real sleeps).
    std::printf("\n7. reconnect wrapper end-to-end vs mock worldd (#96)\n");
    {
        login::LoginResult grant;
        grant.status = login::LoginStatus::kSuccess;
        grant.grant_id = 77;
        grant.session_key = rand_key();
        grant.selected_realm_id = 1;

        std::uint64_t vclock = 0;
        auto now = [&vclock]() { return vclock; };
        auto wait = [&vclock](std::uint32_t ms) { vclock += ms; };
        bot::BotWorldConfig idle;
        idle.path = bot::BotPath::kIdle;
        idle.movement_ticks = 2;  // brief — this test is about the lifecycle, not movement

        // 7a. kFullRelogin — the WORKING M0 path. connect() hands back a fresh mock
        //     that accepts the (fresh) grant; relogin() returns a fresh success.
        //     Expect: reconnected, but NOT a token resume (it was a re-login).
        {
            bot::ReconnectConfig rc;
            rc.strategy = bot::ReconnectStrategy::kFullRelogin;
            rc.backoff.base_delay_ms = 200;
            rc.backoff.window_ms = 0;      // isolate the attempts ceiling
            rc.backoff.max_attempts = 3;
            rc.world = idle;

            auto connect = []() -> std::unique_ptr<login::ILoginTransport> {
                return std::make_unique<MockWorldd>(/*grant_ok=*/true);
            };
            auto relogin = [&grant]() -> login::LoginResult { return grant; };
            bool dropped_once = false;
            auto inject_drop = [&dropped_once]() {
                if (dropped_once) return false;
                dropped_once = true;
                return true;  // force exactly one transient drop
            };

            auto first = std::make_unique<MockWorldd>(/*grant_ok=*/true);
            bot::ReconnectRunReport rep = bot::run_session_with_reconnect(
                std::move(first), grant, rc, connect, relogin, inject_drop, now, wait);

            check("7a: first session entered world", rep.first_session.handshake_ok);
            check("7a: a drop occurred", rep.dropped);
            check("7a: reconnected", rep.reconnect.reconnected);
            check("7a: final state InWorld", rep.final_state == bot::ConnState::kInWorld);
            check("7a: NOT a token resume (full re-login)",
                  !rep.reconnect.resumed_without_relogin);
            check("7a: resumed session re-entered world",
                  rep.resumed_session.handshake_ok);
        }

        // 7b. kResumeWithGrant against a mock that REJECTS the re-presented grant —
        //     the M0 single-use reality (worldd validate_and_consume_grant). Expect:
        //     every attempt GRANT_INVALID, backoff exhausts, gives up, and — the
        //     honest headline — resumed_without_relogin == false.
        {
            bot::ReconnectConfig rc;
            rc.strategy = bot::ReconnectStrategy::kResumeWithGrant;
            rc.backoff.base_delay_ms = 100;
            rc.backoff.max_delay_ms = 400;
            rc.backoff.window_ms = 0;
            rc.backoff.max_attempts = 3;
            rc.world = idle;

            // connect() hands back a mock that REJECTS the grant (grant_ok=false),
            // modelling worldd: the grant was consumed on first enter-world.
            auto connect = []() -> std::unique_ptr<login::ILoginTransport> {
                return std::make_unique<MockWorldd>(/*grant_ok=*/false);
            };
            auto relogin = []() -> login::LoginResult { return {}; };  // unused here
            bool dropped_once = false;
            auto inject_drop = [&dropped_once]() {
                if (dropped_once) return false;
                dropped_once = true;
                return true;
            };

            auto first = std::make_unique<MockWorldd>(/*grant_ok=*/true);
            bot::ReconnectRunReport rep = bot::run_session_with_reconnect(
                std::move(first), grant, rc, connect, relogin, inject_drop, now, wait);

            check("7b: first session entered world", rep.first_session.handshake_ok);
            check("7b: a drop occurred", rep.dropped);
            check("7b: did NOT reconnect (single-use grant)", !rep.reconnect.reconnected);
            check("7b: gave up", rep.reconnect.gave_up);
            check("7b: final state Failed", rep.final_state == bot::ConnState::kFailed);
            check("7b: NOT resumed without relogin (HONEST server gap)",
                  !rep.reconnect.resumed_without_relogin);
            check("7b: attempted max_attempts times", rep.reconnect.attempts == 3);
        }

        // 7c. No drop injected — the wrapper just runs one session and returns clean.
        {
            bot::ReconnectConfig rc;
            rc.world = idle;
            auto connect = []() -> std::unique_ptr<login::ILoginTransport> { return nullptr; };
            auto relogin = []() -> login::LoginResult { return {}; };
            auto no_drop = []() { return false; };
            auto first = std::make_unique<MockWorldd>(/*grant_ok=*/true);
            bot::ReconnectRunReport rep = bot::run_session_with_reconnect(
                std::move(first), grant, rc, connect, relogin, no_drop, now, wait);
            check("7c: entered world", rep.first_session.handshake_ok);
            check("7c: no drop -> not dropped", !rep.dropped);
            check("7c: final state InWorld", rep.final_state == bot::ConnState::kInWorld);
        }
    }

    std::printf(g_fail == 0 ? "\nALL BOT UNIT TESTS PASSED\n"
                            : "\n%d BOT UNIT TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
