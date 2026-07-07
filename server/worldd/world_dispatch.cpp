// SPDX-License-Identifier: Apache-2.0
//
// worldd — IF-2 opcode dispatcher + world-process scaffold implementation
// (issues #82, #83). See world_dispatch.h for the provenance + clean-room
// statement and the M0 transport note.

#include "world_dispatch.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "meridian/core/log.hpp"

namespace meridian::worldd {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace log = meridian::core::log;

constexpr const char* kCat = "worldd";

// ---- little-endian scalar read/write (frame header codec) ------------------
// The IF-2 in-frame header (u16 opcode ‖ u64 seq) is little-endian on the wire,
// matching the net layer's u32 LE length prefix. Hand-rolled so the codec has no
// endianness surprise on a big-endian host.

void put_u16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put_u64(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

// ---- payload verification --------------------------------------------------
// world.fbs declares no root_type, so (as authd's login_session does) we verify
// with the generic flatbuffers::Verifier::VerifyBuffer<T> for the table the
// opcode selects. A frame whose payload does not verify as its declared table is
// a protocol error (untrusted peer — never GetRoot without verifying first).

template <typename T>
bool verify_table(const Frame& f) {
    fb::Verifier v(f.payload, f.payload_len);
    return v.VerifyBuffer<T>(nullptr);
}

// Verify the payload of `f` as the table its opcode selects. Only the opcodes we
// can receive from a client at M0 are checked here; server→client opcodes
// (HANDSHAKE_OK, MOVEMENT_STATE, ENTITY_*) are not expected inbound and fall
// through to "unknown for this direction" handling in the caller.
bool verify_payload_for(net::Opcode op, const Frame& f) {
    switch (op) {
        case net::Opcode::WORLD_HELLO:     return verify_table<mn::WorldHello>(f);
        case net::Opcode::CLOCK_SYNC:      return verify_table<mn::ClockSync>(f);
        case net::Opcode::MOVEMENT_INTENT: return verify_table<mn::MovementIntent>(f);
        case net::Opcode::DISCONNECT:      return verify_table<mn::Disconnect>(f);
        default:                           return false;
    }
}

std::string op_name(std::uint16_t op) {
    const char* n = mn::EnumNameOpcode(static_cast<net::Opcode>(op));
    if (n && n[0] != '\0') return n;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04X", op);
    return std::string(buf);
}

// Build a HandshakeOk payload (S→C). At M0 the content_hash is a placeholder
// (IF-4 world-content hashing is a later story) and the server_proof is left
// empty — the SAD's server proof over (nonce ∥ content_hash) with the
// session_key binds the realm to the client (§5.2); wiring the HMAC is a small
// follow-up once the client verifies it (A-11 protocol-design scope). The table
// still round-trips as HandshakeOk, which is what the enter-world reply needs.
Bytes encode_handshake_ok(const Bytes& content_hash, const Bytes& server_proof) {
    fb::FlatBufferBuilder b;
    auto ch = b.CreateVector(content_hash);
    auto sp = b.CreateVector(server_proof);
    auto ok = mn::CreateHandshakeOk(b, ch, sp);
    b.Finish(ok);
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Build a MovementState payload (S→C, world.fbs) from a validated/rejected
// MoveDecision. entity_guid + server_time_ms are session-scoped (supplied by the
// handler); the position/flags/ack come from the decision — which is the advanced
// position on accept or the snap-back (last authoritative) on reject. This is the
// authoritative state #87's AoI relay will forward to observers; here it is also
// sent back to the mover so it can reconcile / snap back.
Bytes encode_movement_state(std::uint64_t entity_guid, const MoveDecision& d,
                            std::uint64_t server_time_ms) {
    fb::FlatBufferBuilder b;
    auto st = mn::CreateMovementState(b, entity_guid, d.ack_seq, d.state_flags,
                                      d.pos.x, d.pos.y, d.pos.z, d.pos.orientation,
                                      server_time_ms);
    b.Finish(st);
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------

Bytes encode_frame(net::Opcode opcode, std::uint64_t seq, const Bytes& payload) {
    Bytes out;
    out.reserve(kFrameHeaderBytes + payload.size());
    put_u16(out, static_cast<std::uint16_t>(opcode));
    put_u64(out, seq);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<Frame> decode_frame(const Bytes& frame) {
    if (frame.size() < kFrameHeaderBytes) return std::nullopt;
    Frame f;
    f.opcode = static_cast<net::Opcode>(get_u16(frame.data()));
    f.seq = get_u64(frame.data() + sizeof(std::uint16_t));
    f.payload = frame.data() + kFrameHeaderBytes;
    f.payload_len = frame.size() - kFrameHeaderBytes;
    return f;
}

Bytes make_disconnect(net::DisconnectReason reason, const std::string& message,
                      std::uint64_t seq) {
    fb::FlatBufferBuilder b;
    auto msg = b.CreateString(message);
    auto d = mn::CreateDisconnect(b, reason, msg);
    b.Finish(d);
    Bytes payload(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
    return encode_frame(net::Opcode::DISCONNECT, seq, payload);
}

bool is_reserved_range(std::uint16_t opcode) {
    // Top nibble names the domain (world.fbs). Ranges 0x0/0x1/0x2 carry the M0
    // tables; 0x3..0xF are declared-but-reserved (combat, quest, inventory,
    // chat, group, shard, GM, hot-reload). A value in a reserved range is a
    // known-future opcode, distinct from a wholly unknown value.
    std::uint16_t domain = opcode >> 12;
    return domain >= 0x3;
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

Dispatcher::Dispatcher() { register_m0_stubs(); }

void Dispatcher::on(net::Opcode opcode, Handler handler) {
    handlers_[static_cast<std::uint16_t>(opcode)] = std::move(handler);
}

bool Dispatcher::has_handler(net::Opcode opcode) const {
    return handlers_.find(static_cast<std::uint16_t>(opcode)) != handlers_.end();
}

DispatchOutcome Dispatcher::dispatch(net::Session& sess, const Bytes& frame,
                                     ConnCtx& ctx, std::uint16_t& last_opcode,
                                     std::uint64_t& last_seq) const {
    last_opcode = 0;
    last_seq = 0;

    std::optional<Frame> decoded = decode_frame(frame);
    if (!decoded) return DispatchOutcome::kMalformedFrame;

    const Frame& f = *decoded;
    last_opcode = static_cast<std::uint16_t>(f.opcode);
    last_seq = f.seq;

    auto it = handlers_.find(last_opcode);
    if (it == handlers_.end()) {
        // No handler. Distinguish a reserved (declared-but-not-implemented)
        // domain from a wholly unknown value so the serve loop can report each
        // cleanly (both close the connection with a Disconnect).
        return is_reserved_range(last_opcode) ? DispatchOutcome::kReservedOpcode
                                              : DispatchOutcome::kUnknownOpcode;
    }

    // Known opcode: verify its FlatBuffer payload table before the handler sees
    // it (never hand an unverified buffer to a handler).
    if (!verify_payload_for(f.opcode, f)) {
        return DispatchOutcome::kBadPayload;
    }

    it->second(sess, f, ctx);
    return DispatchOutcome::kHandled;
}

void Dispatcher::register_m0_stubs() {
    // --- 0x0xxx session / system ------------------------------------------
    // WORLD_HELLO (#84): the IF-3 session-grant handshake — the authd→worldd
    // handoff. Validate + atomically consume the grant, establish the AEAD
    // session keyed by the grant's session_key, load the D-11 placeholder
    // character, and reply HandshakeOk. Any grant failure -> ask the serve loop
    // to send Disconnect{GRANT_INVALID} and close (SAD §5.3, §5.2).
    on(net::Opcode::WORLD_HELLO,
       [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
           const auto* hello = fb::GetRoot<mn::WorldHello>(f.payload);
           const std::uint64_t grant_id = hello ? hello->grant_id() : 0;

           auto reject = [&](const std::string& why) {
               // Every grant failure is GRANT_INVALID on the wire (no oracle to
               // the client which check failed); the reason is logged server-side.
               log::warn(kCat, "WORLD_HELLO rejected grant_id=" +
                                   std::to_string(grant_id) + ": " + why);
               ctx.disconnect = true;
               ctx.disconnect_reason = net::DisconnectReason::GRANT_INVALID;
               ctx.disconnect_message = "grant invalid";
           };

           // A second WORLD_HELLO on an already-authenticated connection is a
           // protocol error — the handshake happens exactly once.
           if (ctx.authenticated) {
               reject("duplicate WorldHello on an authenticated connection");
               return;
           }
           // No auth DB wired -> cannot validate a grant. Reject (the daemon can
           // run without a DB for smoke tests, but cannot admit players).
           if (ctx.db == nullptr) {
               reject("no auth DB configured (grant validation unavailable)");
               return;
           }

           // 1. Validate + atomically consume the grant (single-use guarantee).
           GrantReject gr = GrantReject::kUnknown;
           std::optional<GrantConsumed> consumed =
               validate_and_consume_grant(*ctx.db, grant_id, ctx.realm_id, gr);
           if (!consumed) {
               const char* why = "unknown grant";
               switch (gr) {
                   case GrantReject::kUnknown:         why = "unknown grant"; break;
                   case GrantReject::kExpired:         why = "expired grant"; break;
                   case GrantReject::kAlreadyConsumed: why = "already-consumed grant (replay)"; break;
                   case GrantReject::kWrongRealm:      why = "grant for a different realm"; break;
                   case GrantReject::kDbError:         why = "grant DB error"; break;
               }
               reject(why);
               return;
           }

           // 2. Establish the AEAD session keyed by the grant's session_key.
           try {
               ctx.session.emplace(consumed->session_key);
           } catch (const net::TlsError& e) {
               reject(std::string("session key setup failed: ") + e.what());
               return;
           }
           ctx.authenticated = true;

           // 3. Enter world: load the D-11 placeholder character.
           PlaceholderCharacter pc =
               load_placeholder_character(ctx.char_db, consumed->account_id);

           // 3a. Seed the authoritative movement state (#86) at the character's
           //     M0 spawn. D-19 flat bootstrap map: spawn at the play-area centre
           //     so the first legal moves in any direction stay in bounds. The
           //     guid lets the world thread stamp MovementState (world.fbs).
           //     spawn_time_ms = 0 seeds the first intent's Δt from spawn.
           Position spawn;
           spawn.x = movement::kZoneMaxXY * 0.5f;  // 64 m — bootstrap play-area centre
           spawn.y = movement::kZoneMaxXY * 0.5f;
           spawn.z = movement::kFlatGroundZ;       // flat ground (D-19)
           ctx.movement.emplace(spawn, /*spawn_time_ms=*/0);
           ctx.movement->set_entity_guid(pc.char_guid);

           log::info(kCat, "WORLD_HELLO accepted grant_id=" +
                               std::to_string(grant_id) + " account=" +
                               std::to_string(consumed->account_id) + " realm=" +
                               std::to_string(consumed->realm_id) + " char='" +
                               pc.name + "' class=" + std::to_string(pc.class_id) +
                               " -> HandshakeOk");

           // 4. Reply HandshakeOk. content_hash is an M0 placeholder (IF-4 world
           //    content hashing is later); server_proof empty (see encoder note).
           Bytes content_hash;  // empty M0 placeholder
           Bytes server_proof;  // empty M0 placeholder
           sess.write_frame(encode_frame(net::Opcode::HANDSHAKE_OK, f.seq,
                                         encode_handshake_ok(content_hash, server_proof)));
       });

    // CLOCK_SYNC (#65): the one M0 opcode with a natural echo. The client sends
    // client_time_ms (server_time_ms = 0); the server echoes both. A real
    // monotonic server clock is a #65 detail — the stub fills server_time_ms with
    // a steady-clock reading so the echo is well-formed and testable.
    on(net::Opcode::CLOCK_SYNC, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        (void)ctx;
        const auto* cs = fb::GetRoot<mn::ClockSync>(f.payload);
        std::uint64_t client_time = cs ? cs->client_time_ms() : 0;
        std::uint64_t server_time =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
        fb::FlatBufferBuilder b;
        auto reply = mn::CreateClockSync(b, client_time, server_time);
        b.Finish(reply);
        Bytes payload(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
        sess.write_frame(encode_frame(net::Opcode::CLOCK_SYNC, f.seq, payload));
        log::debug(kCat, "CLOCK_SYNC echo seq=" + std::to_string(f.seq));
    });

    // MOVEMENT_INTENT (#86): the only client input the server trusts, and only
    // after OPS-03 validation (SAD §5.5). worldd is the SERVER MOVEMENT AUTHORITY:
    // it re-checks every intent against the shared movement rules (the #101 LOCKED
    // constants) and produces the authoritative MovementState. "Server is law"
    // (Pillar 3) — a valid move advances the server's authoritative position; an
    // invalid one is REJECTED and answered with a snap-back correction rather than
    // trusting the client's claimed position.
    on(net::Opcode::MOVEMENT_INTENT, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        // Movement is only legal AFTER a valid HandshakeOk (SAD §5.5 trusts the
        // intent only post-auth). A pre-handshake MOVEMENT_INTENT is a protocol
        // error — ask the serve loop to close (no oracle, no simulation touched).
        if (!ctx.authenticated || !ctx.movement) {
            log::warn(kCat, "MOVEMENT_INTENT before handshake — rejecting");
            ctx.disconnect = true;
            ctx.disconnect_reason = net::DisconnectReason::PROTOCOL_MISMATCH;
            ctx.disconnect_message = "movement before handshake";
            return;
        }

        const auto* mi = fb::GetRoot<mn::MovementIntent>(f.payload);
        if (mi == nullptr) return;  // verified upstream; defensive

        // Lift the intent off the FlatBuffer into the validator's POD (keeps the
        // validator wire-free / pure).
        MovementIntentPod pod;
        pod.seq = mi->seq();
        pod.state_flags = mi->state_flags();
        pod.pos = {mi->x(), mi->y(), mi->z(), mi->orientation()};
        pod.client_time_ms = mi->client_time_ms();

        // R1 — RATE CLASS (≤ 10/s + state changes, SAD §5.5 / #101). An intent
        // above the cap that is not a state change is dropped/coalesced here,
        // BEFORE validation — throttling is not a violation, so it produces no
        // correction (the client simply sees no MovementState for that intent).
        if (!ctx.intake.admit(pod, pod.client_time_ms)) {
            log::debug(kCat, "MOVEMENT_INTENT seq=" + std::to_string(pod.seq) +
                                 " dropped (rate class > " +
                                 std::to_string(movement::kMovementIntentMaxHz) + "/s)");
            return;
        }

        // R2..R5 — validate against the shared constants, then commit the
        // decision to the authoritative state (advance on accept, snap-back on
        // reject). At M0 the per-connection movement state is single-threaded by
        // construction (one IO worker per connection); the seam onto the shared
        // world thread is the WorldServer queue (#87).
        MoveDecision decision = ctx.movement->validate_move(pod, pod.client_time_ms);
        ctx.movement->apply(decision, pod, pod.client_time_ms);

        // The authoritative timestamp for this MovementState (SAD §5.5 / world.fbs
        // server_time_ms). A real monotonic map-tick clock is a later concern; a
        // steady-clock reading is well-formed for M0.
        const std::uint64_t server_time_ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                           .count());

        // Send the authoritative MovementState back to the mover (accept =
        // advanced position; reject = snap-back correction to the last
        // authoritative position). #87's AoI pass relays this same state to
        // observers; here it goes to the mover for reconciliation / snap-back.
        sess.write_frame(encode_frame(
            net::Opcode::MOVEMENT_STATE, f.seq,
            encode_movement_state(ctx.movement->entity_guid(), decision, server_time_ms)));

        if (decision.accepted) {
            log::debug(kCat, "MOVEMENT_INTENT seq=" + std::to_string(pod.seq) +
                                 " accepted -> authoritative advanced");
        } else {
            log::warn(kCat, "MOVEMENT_INTENT seq=" + std::to_string(pod.seq) +
                                " REJECTED (reason=" +
                                std::to_string(static_cast<int>(decision.reject)) +
                                ") -> snap-back correction");
        }
    });

    // No handler is registered for server→client opcodes (HANDSHAKE_OK,
    // MOVEMENT_STATE, ENTITY_ENTER/UPDATE/LEAVE): a client sending one is
    // out-of-direction and is treated as an unknown opcode (Disconnect).
}

// ---------------------------------------------------------------------------
// WorldServer scaffold
// ---------------------------------------------------------------------------

struct WorldServer::Impl {
    // Inbound MPSC queue (many IO workers -> the one world thread).
    std::mutex q_mtx;
    std::condition_variable q_cv;
    std::deque<WorldEvent> queue;

    std::thread world_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<std::uint64_t> drained{0};
};

WorldServer::WorldServer(const Dispatcher& dispatcher, WorldServerConfig cfg)
    : dispatcher_(dispatcher), cfg_(cfg), impl_(std::make_unique<Impl>()) {}

WorldServer::~WorldServer() { stop(); }

void WorldServer::start() {
    if (impl_->running.exchange(true)) return;  // already started
    impl_->stop_requested.store(false);
    impl_->world_thread = std::thread([this] { world_thread_main(); });
    log::info(kCat, "world thread started (tick placeholder every " +
                        std::to_string(cfg_.tick_interval_ms) + " ms; " +
                        std::to_string(cfg_.io_workers) + " IO workers)");
}

void WorldServer::stop() {
    if (!impl_->running.exchange(false)) return;  // already stopped / never started
    impl_->stop_requested.store(true);
    impl_->q_cv.notify_all();
    if (impl_->world_thread.joinable()) impl_->world_thread.join();
    log::info(kCat, "world thread stopped (drained " +
                        std::to_string(impl_->drained.load()) + " events)");
}

void WorldServer::enqueue(WorldEvent ev) {
    {
        std::lock_guard<std::mutex> lk(impl_->q_mtx);
        impl_->queue.push_back(std::move(ev));
    }
    impl_->q_cv.notify_one();
}

std::uint64_t WorldServer::drained_count() const { return impl_->drained.load(); }

void WorldServer::world_thread_main() {
    // M0 TICK PLACEHOLDER (SAD §2.5): the real tick is
    //   drain inbound -> movement -> AI -> combat/auras -> spawns -> AoI delta -> flush
    // at 20 Hz. Here we ONLY drain the inbound queue at the configured cadence —
    // there are no maps to update yet. This is the seam the per-map update loop
    // replaces; the important property today is that this is the ONE thread that
    // owns simulation work (game state), so nothing on the IO/accept path does.
    using namespace std::chrono;
    const auto interval = milliseconds(cfg_.tick_interval_ms);

    while (!impl_->stop_requested.load()) {
        std::deque<WorldEvent> batch;
        {
            std::unique_lock<std::mutex> lk(impl_->q_mtx);
            impl_->q_cv.wait_for(lk, interval, [this] {
                return impl_->stop_requested.load() || !impl_->queue.empty();
            });
            batch.swap(impl_->queue);
        }

        // "drain inbound" — process this tick's batch. At M0 the world thread has
        // no simulation to run against these events; it just accounts for them so
        // the queue -> world-thread path is observable (drained_count()).
        for (const WorldEvent& ev : batch) {
            (void)ev;
            impl_->drained.fetch_add(1);
        }

        // (movement / AI / combat / spawns / AoI / flush would run here)
    }

    // Final drain on shutdown so no enqueued event is silently lost.
    std::deque<WorldEvent> tail;
    {
        std::lock_guard<std::mutex> lk(impl_->q_mtx);
        tail.swap(impl_->queue);
    }
    impl_->drained.fetch_add(tail.size());
}

void WorldServer::serve_connection(net::Session sess) {
    // Runs on an IO worker. Owns the socket; never touches game state. Reads
    // IF-2 frames, dispatches each through a per-connection ConnCtx, and on any
    // non-kHandled outcome (or a handler-requested ctx.disconnect) sends a
    // Disconnect and closes (SAD §5.2 reject-and-close policy).
    const std::string peer = sess.peer();
    log::info(kCat, "connection " + peer + " (" + sess.tls_version() + ")");

    // Per-connection context. Open THIS worker's own DB connection(s) for grant
    // validation + enter-world (like authd: one DB connection per served
    // connection, never shared across threads — SAD §2.2). When no auth DB is
    // configured (cfg_.auth_db.user empty), db stays null and a WORLD_HELLO is
    // rejected GRANT_INVALID — the daemon still runs, it just cannot admit
    // players (usable for --version / dispatch smoke tests without a DB).
    ConnCtx ctx;
    ctx.realm_id = cfg_.realm_id;
    std::optional<db::Connection> auth_conn;
    std::optional<db::Connection> char_conn;
    if (!cfg_.auth_db.user.empty()) {
        try {
            auth_conn.emplace(cfg_.auth_db);
            ctx.db = &*auth_conn;
        } catch (const db::DbError& e) {
            log::warn(kCat, "connection " + peer +
                                ": auth DB connect failed (grants disabled): " + e.what());
        }
    }
    if (!cfg_.char_db.user.empty()) {
        try {
            char_conn.emplace(cfg_.char_db);
            ctx.char_db = &*char_conn;
        } catch (const db::DbError& e) {
            log::warn(kCat, "connection " + peer +
                                ": characters DB connect failed (placeholder only): " + e.what());
        }
    }

    try {
        for (;;) {
            Bytes frame;
            try {
                frame = sess.read_frame();
            } catch (const mn::ConnectionClosed&) {
                log::info(kCat, "connection " + peer + " closed by peer");
                break;
            }

            std::uint16_t op = 0;
            std::uint64_t seq = 0;
            DispatchOutcome outcome = dispatcher_.dispatch(sess, frame, ctx, op, seq);

            // A handler (e.g. WORLD_HELLO on a grant reject) can ask for a clean
            // close even though dispatch itself succeeded (kHandled).
            if (outcome == DispatchOutcome::kHandled && ctx.disconnect) {
                log::warn(kCat, "disconnecting " + peer + ": " + ctx.disconnect_message);
                try {
                    sess.write_frame(
                        make_disconnect(ctx.disconnect_reason, ctx.disconnect_message, seq));
                } catch (const mn::TlsError&) {
                    // Peer may already be gone; closing below is enough.
                }
                break;
            }

            if (outcome == DispatchOutcome::kHandled) continue;

            // Error path: map the outcome to a DisconnectReason + message, send a
            // Disconnect, and close. One reject ends the connection.
            net::DisconnectReason reason = net::DisconnectReason::PROTOCOL_MISMATCH;
            std::string detail;
            switch (outcome) {
                case DispatchOutcome::kUnknownOpcode:
                    detail = "unknown opcode " + op_name(op);
                    break;
                case DispatchOutcome::kReservedOpcode:
                    detail = "reserved (not-yet-implemented) opcode " + op_name(op);
                    break;
                case DispatchOutcome::kBadPayload:
                    detail = "malformed payload for opcode " + op_name(op);
                    break;
                case DispatchOutcome::kMalformedFrame:
                    detail = "frame shorter than IF-2 header";
                    break;
                case DispatchOutcome::kHandled:
                    break;  // unreachable (handled above)
            }
            log::warn(kCat, "disconnecting " + peer + ": " + detail);
            try {
                sess.write_frame(make_disconnect(reason, detail, seq));
            } catch (const mn::TlsError&) {
                // Peer may already be gone; closing below is enough.
            }
            break;
        }
    } catch (const mn::TlsError& e) {
        log::warn(kCat, "connection " + peer + " transport error: " + e.what());
    }

    sess.close();
}

}  // namespace meridian::worldd
