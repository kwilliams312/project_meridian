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

#include "characters.h"  // meridian-characters CRUD: list/create/delete (#286 / D-35)
#include "meridian/core/audit.hpp"
#include "meridian/core/log.hpp"
#include "meridian/metrics/catalog.h"
#include "meridian/trace/session_flow.h"
#include "meridian/trace/span.h"
#include "meridian/trace/tracer.h"
#include "roster.h"       // M0-frozen race/class roster (validated by create)

namespace meridian::worldd {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace log = meridian::core::log;
namespace audit = meridian::core::audit;
namespace metrics = meridian::metrics::catalog;
namespace tr = meridian::trace;
namespace chr = meridian::characters;

constexpr const char* kCat = "worldd";

// OPS-05 (#166): the "worldd.enter_world" session-flow span (grant validate →
// session establish → HandshakeOk). Parented onto the authd login span via the
// grant-derived trace context so the two hops stitch into ONE trace (D-29 §9 rule
// 5). `ctx` carries the exporter + realm; `grant_id` derives the trace ids;
// `start_ns` is the handler's start time; `ok` is the handshake result; `reject`
// is the server-side reject classification (empty on success). Fills `out_trace`/
// `out_span` with the emitted span's lower-hex ids for log correlation (#165).
// No-op when no exporter is configured. Kept as a free helper so BOTH the reject
// lambda and the success tail can emit exactly one span per WORLD_HELLO.
void emit_enter_world_span(ConnCtx& ctx, std::uint64_t grant_id, std::uint64_t start_ns,
                           bool ok, const std::string& reject_reason,
                           std::string& out_trace, std::string& out_span) {
    if (ctx.tracer == nullptr || !ctx.tracer->active()) return;

    tr::Span span;
    if (grant_id != 0) {
        // Same grant → same trace_id as the authd login span; parent onto the
        // authd-derived span_id (D-29 §9 rule 5 stitching).
        tr::SpanContext parent = tr::flow::trace_context_from_grant(grant_id);
        span.trace_id = parent.trace_id;
        span.parent_span_id = parent.span_id;   // the authd.login span
        tr::fill_random(span.span_id);          // this hop's own span
    } else {
        // No grant on the frame (malformed WorldHello) — a self-contained root.
        tr::fill_random(span.trace_id);
        tr::fill_random(span.span_id);
    }
    span.name = tr::flow::kWorlddEnterWorld;
    span.kind = tr::SpanKind::kServer;
    span.start_unix_nano = start_ns;
    span.end_unix_nano = tr::now_unix_nano();

    span.set(tr::attr(tr::flow::kAttrRealm, ctx.labels.realm));
    span.set(tr::attr(tr::flow::kAttrHandshake, ok));
    if (grant_id != 0) {
        span.set(tr::attr("meridian.grant_id", static_cast<std::int64_t>(grant_id)));
    }
    if (ok) {
        span.set(tr::attr(tr::flow::kAttrRealmId, static_cast<std::int64_t>(ctx.realm_id)));
        span.set_status(tr::StatusCode::kOk);
    } else {
        span.set(tr::attr(tr::flow::kAttrGrantReject, reject_reason));
        span.set_status(tr::StatusCode::kError, reject_reason);
    }

    out_trace = tr::to_hex(span.trace_id);
    out_span = tr::to_hex(span.span_id);
    ctx.tracer->export_span(std::move(span));
}

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
        case net::Opcode::CHAR_LIST_REQUEST:   return verify_table<mn::CharListRequest>(f);
        case net::Opcode::CHAR_CREATE_REQUEST: return verify_table<mn::CharCreateRequest>(f);
        case net::Opcode::CHAR_DELETE_REQUEST: return verify_table<mn::CharDeleteRequest>(f);
        case net::Opcode::ENTER_WORLD_REQUEST: return verify_table<mn::EnterWorldRequest>(f);
        case net::Opcode::CAST_REQUEST:        return verify_table<mn::CastRequest>(f);
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

// ---- character-management encoders (S→C, world.fbs; #286 / D-35) ------------

// Build a CharListResponse payload from the account's roster (each row is the
// character-select shape: id, name, race, class, level).
Bytes encode_char_list(const std::vector<chr::CharacterSummary>& roster) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::CharListEntry>> rows;
    rows.reserve(roster.size());
    for (const auto& c : roster) {
        auto name = b.CreateString(c.name);
        rows.push_back(mn::CreateCharListEntry(b, c.id, name, c.race, c.char_class,
                                               c.level));
    }
    auto vec = b.CreateVector(rows);
    b.Finish(mn::CreateCharListResponse(b, vec));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Build a CharCreateResponse payload: a typed status + (on OK) the minted id.
Bytes encode_char_create(mn::CharCreateStatus status, std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCharCreateResponse(b, status, character_id));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Build a CharDeleteResponse payload: a typed ok/refused status.
Bytes encode_char_delete(mn::CharDeleteStatus status) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCharDeleteResponse(b, status));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Build an EnterWorldResponse payload: the typed enter-world result. OK means the
// server spawned the named owned character (the client transitions in-world); any
// other status means NO spawn — the session stays at character-select.
Bytes encode_enter_world_response(mn::EnterWorldStatus status) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldResponse(b, status));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// ---- combat encoders (S→C, world.fbs; CMB-01 #344/#345) ---------------------

// Build a CastStart payload — the D-10 ACCEPT reply. cast_ms 0 = instant.
Bytes encode_cast_start(AbilityId ability_id, std::uint32_t cast_ms,
                        std::uint64_t server_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCastStart(b, ability_id, cast_ms, server_time_ms));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Build a CastFailed payload — the D-10 REJECT reply (client rolls back + resyncs
// its GCD clock from gcd_remaining_ms).
Bytes encode_cast_failed(AbilityId ability_id, mn::CastFailReason reason,
                         std::uint32_t gcd_remaining_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCastFailed(b, ability_id, reason, gcd_remaining_ms));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Build a CastResult payload — the server-authoritative resolution outcome.
Bytes encode_cast_result(AbilityId ability_id, std::uint64_t caster_guid,
                         std::uint64_t target_guid, mn::AttackOutcome outcome,
                         std::uint32_t amount, bool is_heal,
                         std::uint32_t target_health, bool target_dead,
                         std::uint64_t server_time_ms) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCastResult(b, ability_id, caster_guid, target_guid, outcome,
                                  amount, is_heal, target_health, target_dead,
                                  server_time_ms));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Map the resolver's CastReject -> the wire CastFailReason. kNone should never
// reach here (only rejects are sent) — it maps to UNKNOWN_ABILITY defensively.
mn::CastFailReason to_wire_reason(CastReject r) {
    switch (r) {
        case CastReject::kNone:                 return mn::CastFailReason::UNKNOWN_ABILITY;
        case CastReject::kUnknownAbility:       return mn::CastFailReason::UNKNOWN_ABILITY;
        case CastReject::kNotInWorld:           return mn::CastFailReason::NOT_IN_WORLD;
        case CastReject::kCasterDead:           return mn::CastFailReason::CASTER_DEAD;
        case CastReject::kOnGcd:                return mn::CastFailReason::ON_GCD;
        case CastReject::kAlreadyCasting:       return mn::CastFailReason::ALREADY_CASTING;
        case CastReject::kInsufficientResource: return mn::CastFailReason::INSUFFICIENT_RESOURCE;
        case CastReject::kNoTarget:             return mn::CastFailReason::NO_TARGET;
        case CastReject::kTargetDead:           return mn::CastFailReason::TARGET_DEAD;
        case CastReject::kWrongFaction:         return mn::CastFailReason::WRONG_FACTION;
        case CastReject::kOutOfRange:           return mn::CastFailReason::OUT_OF_RANGE;
        case CastReject::kNoLineOfSight:        return mn::CastFailReason::NO_LINE_OF_SIGHT;
        case CastReject::kInterrupted:          return mn::CastFailReason::INTERRUPTED;
    }
    return mn::CastFailReason::UNKNOWN_ABILITY;
}

// Map the resolver's AttackOutcome -> the wire AttackOutcome.
mn::AttackOutcome to_wire_outcome(AttackOutcome o) {
    switch (o) {
        case AttackOutcome::kMiss:  return mn::AttackOutcome::MISS;
        case AttackOutcome::kDodge: return mn::AttackOutcome::DODGE;
        case AttackOutcome::kParry: return mn::AttackOutcome::PARRY;
        case AttackOutcome::kHit:   return mn::AttackOutcome::HIT;
        case AttackOutcome::kCrit:  return mn::AttackOutcome::CRIT;
    }
    return mn::AttackOutcome::HIT;
}

// Char-management ops are legal only on an AUTHENTICATED session (post-WorldHello),
// and always act on the session's own grant-derived account (never a client field).
// A char request before the handshake is a protocol error — ask the serve loop to
// close (mirrors the MOVEMENT_INTENT pre-handshake guard, SAD §5.5). Returns false
// (and arms ctx.disconnect) when the session is not authenticated.
bool require_authenticated(ConnCtx& ctx, const char* op) {
    if (!ctx.authenticated || ctx.account_id == 0) {
        log::warn(kCat, std::string(op) + " before handshake — rejecting");
        ctx.disconnect = true;
        ctx.disconnect_reason = net::DisconnectReason::PROTOCOL_MISMATCH;
        ctx.disconnect_message = "char management before handshake";
        return false;
    }
    return true;
}

// Send a server→client reply frame. Routed through the per-session egress channel
// when one is established (so it serializes with the AoI relay's s2c writes from
// OTHER threads — SAD §5.2/§6), falling back to a direct write in the DB-less
// smoke path where no egress is wired. Mirrors the MOVEMENT_STATE reply routing.
void send_s2c(net::Session& sess, ConnCtx& ctx, const Bytes& frame) {
    if (ctx.egress) {
        ctx.egress->emit_frame(frame);
    } else {
        sess.write_frame(frame);
    }
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

    // OPS-05: the opcode label for every per-opcode series (Realm-health /
    // Errors dashboards). Computed once from the decoded opcode.
    const std::string op = opcode_label(last_opcode);

    auto it = handlers_.find(last_opcode);
    if (it == handlers_.end()) {
        // No handler. Distinguish a reserved (declared-but-not-implemented)
        // domain from a wholly unknown value so the serve loop can report each
        // cleanly (both close the connection with a Disconnect). Either way the
        // frame is DROPPED (meridian_opcode_dropped_total{...,opcode,reason}).
        const bool reserved = is_reserved_range(last_opcode);
        metrics::opcode_dropped_total()
            .with(ctx.labels.rzs_opcode_reason(op, reserved ? "reserved" : "unknown"))
            .inc();
        return reserved ? DispatchOutcome::kReservedOpcode
                        : DispatchOutcome::kUnknownOpcode;
    }

    // A known opcode was received (count it — meridian_opcode_total{...,opcode}).
    metrics::opcode_total().with(ctx.labels.rzs_opcode(op)).inc();

    // Verify its FlatBuffer payload table before the handler sees it (never hand
    // an unverified buffer to a handler). A verify failure is a per-opcode error.
    if (!verify_payload_for(f.opcode, f)) {
        metrics::opcode_errors_total().with(ctx.labels.rzs_opcode(op)).inc();
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

           // OPS-05 (#166): start of the "worldd.enter_world" span. Stamp the
           // start now; the span is emitted (once) at whichever exit this handler
           // takes — a reject or a successful HandshakeOk.
           const std::uint64_t trace_start_ns = tr::now_unix_nano();

           auto reject = [&](const std::string& why, const char* reason_code) {
               // OPS-05 traces: emit the enter-world span as a FAILURE at the
               // reject point (this is the failure point the trace surfaces).
               std::string trace_id, span_id;
               emit_enter_world_span(ctx, grant_id, trace_start_ns, /*ok=*/false, why,
                                     trace_id, span_id);
               // Every grant failure is GRANT_INVALID on the wire (no oracle to
               // the client which check failed); the reason is logged server-side.
               log::Fields wf{log::field("grant_id", grant_id),
                              log::field("reason", why)};
               if (!trace_id.empty()) {
                   wf.push_back(log::field("trace_id", trace_id));
                   wf.push_back(log::field("span_id", span_id));
               }
               log::warn(kCat, "WORLD_HELLO rejected", wf);
               // OPS-05 audit (#92): a denied session handshake is a security
               // event. Record the machine-readable reject code + the offending
               // grant as the correlation id; NO secret material (the session_key
               // is never touched here — the grant failed validation). No account
               // id: a rejected grant is not attributed to an authenticated actor.
               audit::emit(audit::Record{
                   .action = audit::Action::kGrantRejected,
                   .outcome = audit::Outcome::kFailure,
                   .reason = reason_code,
                   .correlation_id = grant_id,
               });
               // OPS-05: a rejected handshake is a WORLD_HELLO error (Errors dash).
               metrics::opcode_errors_total()
                   .with(ctx.labels.rzs_opcode(opcode_label(
                       static_cast<std::uint16_t>(net::Opcode::WORLD_HELLO))))
                   .inc();
               ctx.disconnect = true;
               ctx.disconnect_reason = net::DisconnectReason::GRANT_INVALID;
               ctx.disconnect_message = "grant invalid";
           };

           // A second WORLD_HELLO on an already-authenticated connection is a
           // protocol error — the handshake happens exactly once.
           if (ctx.authenticated) {
               reject("duplicate WorldHello on an authenticated connection",
                      "duplicate_hello");
               return;
           }
           // No auth DB wired -> cannot validate a grant. Reject (the daemon can
           // run without a DB for smoke tests, but cannot admit players).
           if (ctx.db == nullptr) {
               reject("no auth DB configured (grant validation unavailable)",
                      "no_auth_db");
               return;
           }

           // 1. Validate + atomically consume the grant (single-use guarantee).
           //    OPS-05: time the auth-DB round-trip (meridian_db_latency_seconds
           //    {realm,db="auth"}) — the grant consume is worldd's one M0 DB call.
           GrantReject gr = GrantReject::kUnknown;
           const auto db_t0 = std::chrono::steady_clock::now();
           std::optional<GrantConsumed> consumed =
               validate_and_consume_grant(*ctx.db, grant_id, ctx.realm_id, gr);
           metrics::db_latency_seconds()
               .with({ctx.labels.realm, "auth"})
               .observe(std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - db_t0)
                            .count());
           if (!consumed) {
               const char* why = "unknown grant";
               const char* code = "grant_unknown";  // audit reject code (#92)
               switch (gr) {
                   case GrantReject::kUnknown:
                       why = "unknown grant"; code = "grant_unknown"; break;
                   case GrantReject::kExpired:
                       why = "expired grant"; code = "grant_expired"; break;
                   case GrantReject::kAlreadyConsumed:
                       why = "already-consumed grant (replay)"; code = "grant_replay"; break;
                   case GrantReject::kWrongRealm:
                       why = "grant for a different realm"; code = "grant_wrong_realm"; break;
                   case GrantReject::kDbError:
                       why = "grant DB error"; code = "grant_db_error"; break;
               }
               reject(why, code);
               return;
           }

           // OPS-05 audit (#92): the grant was validated + atomically consumed
           // (single-use spent). Attribute it to the now-known account with the
           // grant as the correlation id. The session_key returned by the consume
           // is NEVER logged — only the account + grant id + realm target.
           ctx.account_id = consumed->account_id;
           ctx.grant_id = grant_id;
           audit::emit(audit::Record{
               .action = audit::Action::kGrantConsumed,
               .outcome = audit::Outcome::kSuccess,
               .account_id = consumed->account_id,
               .target = "realm:" + std::to_string(consumed->realm_id),
               .correlation_id = grant_id,
           });

           // 2. Establish the AEAD session keyed by the grant's session_key.
           try {
               ctx.session.emplace(consumed->session_key);
           } catch (const net::TlsError& e) {
               reject(std::string("session key setup failed: ") + e.what(),
                      "session_key_setup_failed");
               return;
           }
           ctx.authenticated = true;
           // Server-authoritative characters (D-35 / #341): a valid WORLD_HELLO now
           // leaves the session at CHARACTER-SELECT, NOT in the world. There is no
           // implicit spawn and no fabricated placeholder — the player must send an
           // explicit ENTER_WORLD_REQUEST naming a character it OWNS to be spawned.
           // Everything that makes a session "in-world" (the CCU/active gauges, the
           // authoritative movement state, the AoI registration, and the #326
           // single-active-session admission) moves to the ENTER_WORLD OK path.
           ctx.phase = SessionPhase::kCharSelect;

           // OPS-05 traces (#166): emit the handshake span as a SUCCESS (grant
           // validated → AEAD session established → about to reply HandshakeOk),
           // stitched to the authd login span via the grant. Carry its ids into
           // the accept log so the log line pivots to the trace (#165). This marks
           // reaching character-select — the enter-world (spawn) span is emitted
           // later by the ENTER_WORLD handler.
           std::string trace_id, span_id;
           emit_enter_world_span(ctx, grant_id, trace_start_ns, /*ok=*/true,
                                 /*reject_reason=*/{}, trace_id, span_id);
           log::Fields af{log::field("grant_id", grant_id),
                          log::field("account_id",
                                     static_cast<std::int64_t>(consumed->account_id)),
                          log::field("realm_id",
                                     static_cast<std::int64_t>(consumed->realm_id))};
           if (!trace_id.empty()) {
               af.push_back(log::field("trace_id", trace_id));
               af.push_back(log::field("span_id", span_id));
           }
           log::info(kCat, "WORLD_HELLO accepted -> HandshakeOk (character-select)", af);

           // Reply HandshakeOk. content_hash is an M0 placeholder (IF-4 world
           // content hashing is later); server_proof empty (see encoder note). The
           // session now sits at character-select: it may CHAR_LIST/CREATE/DELETE
           // and ENTER_WORLD. No entity, no movement, no CCU/AoI/single-active yet.
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
        // Movement is only legal once the session is IN WORLD (SAD §5.5 trusts the
        // intent only post-auth; D-35 additionally requires an explicit ENTER_WORLD
        // spawn). `ctx.movement` is emplaced only on ENTER_WORLD OK, so a movement
        // sent at character-select (authenticated but not spawned) or pre-handshake
        // is a protocol error — ask the serve loop to close (no simulation touched).
        if (!ctx.authenticated || !ctx.movement) {
            log::warn(kCat, "MOVEMENT_INTENT before entering world — rejecting");
            ctx.disconnect = true;
            ctx.disconnect_reason = net::DisconnectReason::PROTOCOL_MISMATCH;
            ctx.disconnect_message = "movement before entering world";
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
            // OPS-05: a rate-throttled intent is a drop (not a violation) —
            // meridian_opcode_dropped_total{...,opcode=MOVEMENT_INTENT,
            // reason=rate_limit}. Feeds the Errors dashboard's drop panel.
            metrics::opcode_dropped_total()
                .with(ctx.labels.rzs_opcode_reason(
                    opcode_label(static_cast<std::uint16_t>(net::Opcode::MOVEMENT_INTENT)),
                    "rate_limit"))
                .inc();
            return;
        }

        // R2..R5 — validate against the shared constants, then commit the
        // decision to the authoritative state (advance on accept, snap-back on
        // reject). At M0 the per-connection movement state is single-threaded by
        // construction (one IO worker per connection); the seam onto the shared
        // world thread is the WorldServer queue (#87).
        MoveDecision decision = ctx.movement->validate_move(pod, pod.client_time_ms);
        ctx.movement->apply(decision, pod, pod.client_time_ms);

        // OPS-05: a REJECTED intent is BOTH a movement-check violation (by kind —
        // meridian_movement_violations_total{...,kind}) AND a snap-back correction
        // issued to the client (meridian_movement_corrections_total{...}). These
        // are the Player-experience dashboard's correction/snap-back rate panels.
        if (!decision.accepted) {
            metrics::movement_violations_total()
                .with({ctx.labels.realm, ctx.labels.zone, ctx.labels.shard,
                       move_reject_kind(decision.reject)})
                .inc();
            metrics::movement_corrections_total().with(ctx.labels.rzs()).inc();
        }

        // The authoritative timestamp for this MovementState (SAD §5.5 / world.fbs
        // server_time_ms). A real monotonic map-tick clock is a later concern; a
        // steady-clock reading is well-formed for M0.
        const std::uint64_t server_time_ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                           .count());

        // Send the authoritative MovementState back to the mover (accept =
        // advanced position; reject = snap-back correction to the last
        // authoritative position). Routed through the egress channel when one is
        // established so it serializes with the relay's s2c writes (SAD §5.2/§6);
        // falls back to a direct write in the DB-less smoke test (no egress).
        Bytes state_frame = encode_frame(
            net::Opcode::MOVEMENT_STATE, f.seq,
            encode_movement_state(ctx.movement->entity_guid(), decision, server_time_ms));
        if (ctx.egress) {
            ctx.egress->emit_frame(state_frame);
        } else {
            sess.write_frame(state_frame);
        }

        // AoI RELAY (#87): on this authoritative MovementState, update the mover's
        // grid position and relay EntityEnter/Update/Leave to the OTHER sessions
        // in range (and this mover for the reciprocal enters/leaves). Only an
        // ACCEPTED move advances the authoritative position; a snap-back keeps the
        // last position, so relaying the (unchanged) authoritative position is
        // still correct — it re-affirms where observers see the mover. The relay
        // uses the authoritative position the validator committed.
        if (ctx.entered && ctx.world != nullptr) {
            const Position& auth = ctx.movement->authoritative();
            ctx.world->on_movement(ctx.slot, auth, decision.ack_seq, decision.state_flags,
                                   server_time_ms);
        }

        // COMBAT cast interrupt (CMB-01 #344): moving cancels an in-progress cast
        // (SAD §2.5 interrupt / §3.3 cast-while-moving rules). Only an ACCEPTED
        // move interrupts — a snap-back (reject) means the client did NOT move, so
        // the cast stands. The client sees CastFailed{INTERRUPTED} and rolls the
        // cast bar back (its GCD, already elapsing, is untouched).
        if (decision.accepted && ctx.combat.is_casting(server_time_ms)) {
            const AbilityId cast_ability =
                ctx.combat.pending() ? ctx.combat.pending()->ability_id : 0;
            ctx.combat.interrupt();
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::CAST_FAILED, f.seq,
                                  encode_cast_failed(cast_ability,
                                                     mn::CastFailReason::INTERRUPTED,
                                                     ctx.combat.gcd_remaining_ms(server_time_ms))));
            log::debug(kCat, "cast interrupted by movement ability=" +
                                 std::to_string(cast_ability));
        }

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

    // --- 0x0xxx character management (#286 / D-35) ------------------------
    // WoW-style list/create/delete over the AUTHENTICATED world session, backed
    // by the meridian-characters CRUD (#85). Every op acts on ctx.account_id —
    // the account the grant authenticated this session as (IF-3) — NEVER an
    // account named in the request, so a client can only ever manage its OWN
    // characters. Ownership is additionally enforced at the DB by the CRUD's
    // `WHERE id=? AND account_id=?` predicate (soft-ref rule, §4.4). The char DB
    // read/write runs synchronously on the IO worker, exactly like ENTER_WORLD's
    // load_owned_character + the grant consume (no world-thread involvement).

    // CHAR_LIST_REQUEST: return the account's roster (character-select screen).
    on(net::Opcode::CHAR_LIST_REQUEST,
       [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
           if (!require_authenticated(ctx, "CHAR_LIST_REQUEST")) return;

           std::vector<chr::CharacterSummary> roster;
           if (ctx.char_db != nullptr) {
               try {
                   roster = chr::list_characters(*ctx.char_db, ctx.account_id);
               } catch (const db::DbError& e) {
                   // No error channel on CharListResponse — degrade to an empty
                   // roster and log (the client sees "no characters"; the DB fault
                   // is server-side observable). Production always has char_db.
                   log::warn(kCat, "CHAR_LIST_REQUEST DB error — empty roster",
                             {log::field("account_id",
                                         static_cast<std::int64_t>(ctx.account_id)),
                              log::field("error", e.what())});
               }
           } else {
               log::warn(kCat, "CHAR_LIST_REQUEST with no characters DB — empty roster");
           }
           log::debug(kCat, "CHAR_LIST_REQUEST -> " + std::to_string(roster.size()) +
                                " character(s) for account " +
                                std::to_string(ctx.account_id));
           send_s2c(sess, ctx,
                    encode_frame(net::Opcode::CHAR_LIST_RESPONSE, f.seq,
                                 encode_char_list(roster)));
       });

    // CHAR_CREATE_REQUEST: create a character (name + race + class, D-11). Maps
    // each meridian-characters create exception to its typed CharCreateStatus.
    on(net::Opcode::CHAR_CREATE_REQUEST,
       [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
           if (!require_authenticated(ctx, "CHAR_CREATE_REQUEST")) return;
           const auto* req = fb::GetRoot<mn::CharCreateRequest>(f.payload);
           if (req == nullptr) return;  // verified upstream; defensive

           mn::CharCreateStatus status = mn::CharCreateStatus::OK;
           std::uint64_t minted = 0;

           if (ctx.char_db == nullptr) {
               log::warn(kCat, "CHAR_CREATE_REQUEST with no characters DB");
               status = mn::CharCreateStatus::INTERNAL;
           } else {
               chr::CreateRequest cr;
               cr.account_id = ctx.account_id;  // the session's account — NEVER the client's
               cr.name = req->name() ? req->name()->str() : std::string();
               cr.race = req->race();
               cr.char_class = req->char_class();
               try {
                   minted = chr::create_character(*ctx.char_db, cr).character_id;
               } catch (const chr::CharacterLimitReached&) {
                   status = mn::CharCreateStatus::LIMIT_REACHED;
               } catch (const chr::DuplicateName&) {
                   status = mn::CharCreateStatus::DUPLICATE_NAME;
               } catch (const chr::InvalidRace&) {
                   status = mn::CharCreateStatus::INVALID_RACE;
               } catch (const chr::InvalidClass&) {
                   status = mn::CharCreateStatus::INVALID_CLASS;
               } catch (const chr::InvalidName&) {
                   status = mn::CharCreateStatus::INVALID_NAME;
               } catch (const db::DbError& e) {
                   log::warn(kCat, "CHAR_CREATE_REQUEST DB error",
                             {log::field("account_id",
                                         static_cast<std::int64_t>(ctx.account_id)),
                              log::field("error", e.what())});
                   status = mn::CharCreateStatus::INTERNAL;
               }
           }
           if (status == mn::CharCreateStatus::OK) {
               log::info(kCat, "CHAR_CREATE_REQUEST created character",
                         {log::field("account_id",
                                     static_cast<std::int64_t>(ctx.account_id)),
                          log::field("character_id",
                                     static_cast<std::int64_t>(minted))});
           } else {
               log::debug(kCat, "CHAR_CREATE_REQUEST rejected (status=" +
                                    std::to_string(static_cast<int>(status)) + ")");
           }
           send_s2c(sess, ctx,
                    encode_frame(net::Opcode::CHAR_CREATE_RESPONSE, f.seq,
                                 encode_char_create(status, minted)));
       });

    // CHAR_DELETE_REQUEST: delete one owned character by id. The CRUD's
    // `WHERE id=? AND account_id=?` makes deleting another account's character (or
    // a non-existent id) a no-op -> REFUSED. Ownership is impossible to bypass.
    on(net::Opcode::CHAR_DELETE_REQUEST,
       [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
           if (!require_authenticated(ctx, "CHAR_DELETE_REQUEST")) return;
           const auto* req = fb::GetRoot<mn::CharDeleteRequest>(f.payload);
           if (req == nullptr) return;  // verified upstream; defensive

           mn::CharDeleteStatus status = mn::CharDeleteStatus::REFUSED;
           if (ctx.char_db == nullptr) {
               log::warn(kCat, "CHAR_DELETE_REQUEST with no characters DB");
               status = mn::CharDeleteStatus::INTERNAL;
           } else {
               try {
                   const bool deleted = chr::delete_character(
                       *ctx.char_db, ctx.account_id, req->character_id());
                   status = deleted ? mn::CharDeleteStatus::OK
                                    : mn::CharDeleteStatus::REFUSED;
               } catch (const db::DbError& e) {
                   log::warn(kCat, "CHAR_DELETE_REQUEST DB error",
                             {log::field("account_id",
                                         static_cast<std::int64_t>(ctx.account_id)),
                              log::field("error", e.what())});
                   status = mn::CharDeleteStatus::INTERNAL;
               }
           }
           log::debug(kCat, "CHAR_DELETE_REQUEST character_id=" +
                                std::to_string(req->character_id()) + " status=" +
                                std::to_string(static_cast<int>(status)));
           send_s2c(sess, ctx,
                    encode_frame(net::Opcode::CHAR_DELETE_RESPONSE, f.seq,
                                 encode_char_delete(status)));
       });

    // --- ENTER_WORLD (server-authoritative characters, D-35 / #341) ------------
    // Promote an authenticated CHARACTER-SELECT session INTO the world as a REAL,
    // OWNED character. This is the SINGLE seam where a session spawns: the server
    // loads the requested character ONLY if it belongs to this session's grant-
    // derived account (never a client-named account), spawns THAT character, and
    // replies OK. A bad/foreign/absent character is REJECTED with a typed status
    // and no spawn — the server is the definitive source of truth and never
    // fabricates a character. The CCU/active gauges, the authoritative movement
    // state, the AoI registration, and the #326 single IN-WORLD session admission
    // all live here (moved off the handshake path), so only a real in-world player
    // is counted and holds the account's slot.
    on(net::Opcode::ENTER_WORLD_REQUEST,
       [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
           if (!require_authenticated(ctx, "ENTER_WORLD_REQUEST")) return;

           auto reply = [&](mn::EnterWorldStatus st) {
               send_s2c(sess, ctx,
                        encode_frame(net::Opcode::ENTER_WORLD_RESPONSE, f.seq,
                                     encode_enter_world_response(st)));
           };

           // Double-enter guard: a session already in-world must not spawn twice
           // (that would leak an AoI slot + a second CCU count). Refuse politely.
           if (ctx.phase == SessionPhase::kInWorld) {
               log::warn(kCat, "ENTER_WORLD_REQUEST while already in world — ignoring",
                         {log::field("account_id",
                                     static_cast<std::int64_t>(ctx.account_id))});
               reply(mn::EnterWorldStatus::INTERNAL);
               return;
           }

           const auto* req = fb::GetRoot<mn::EnterWorldRequest>(f.payload);
           if (req == nullptr) return;  // verified upstream; defensive
           const std::uint64_t character_id = req->character_id();

           // No characters DB -> we cannot prove ownership, and we NEVER fabricate.
           if (ctx.char_db == nullptr) {
               log::warn(kCat, "ENTER_WORLD_REQUEST with no characters DB — INTERNAL");
               reply(mn::EnterWorldStatus::INTERNAL);
               return;
           }

           // Load the character IFF this account owns it. nullopt => reject; we
           // then distinguish "no character at all" (client must create first) from
           // "that id isn't yours / doesn't exist" (client must re-pick).
           std::optional<LoadedCharacter> loaded;
           try {
               loaded = load_owned_character(*ctx.char_db, ctx.account_id, character_id);
               if (!loaded) {
                   const bool has_any =
                       !chr::list_characters(*ctx.char_db, ctx.account_id).empty();
                   log::info(kCat, "ENTER_WORLD_REQUEST rejected (unowned/absent)",
                             {log::field("account_id",
                                         static_cast<std::int64_t>(ctx.account_id)),
                              log::field("character_id",
                                         static_cast<std::int64_t>(character_id))});
                   reply(has_any ? mn::EnterWorldStatus::NOT_FOUND
                                 : mn::EnterWorldStatus::NO_CHARACTER);
                   return;
               }
           } catch (const db::DbError& e) {
               log::warn(kCat, "ENTER_WORLD_REQUEST DB error",
                         {log::field("account_id",
                                     static_cast<std::int64_t>(ctx.account_id)),
                          log::field("character_id",
                                     static_cast<std::int64_t>(character_id)),
                          log::field("error", e.what())});
               reply(mn::EnterWorldStatus::INTERNAL);
               return;
           }

           // --- OK: spawn the REAL owned character in-world ----------------------
           const LoadedCharacter& pc = *loaded;

           // Seed the authoritative movement state (#86) at the character's M0
           // spawn. D-19 flat bootstrap map: spawn at the play-area centre so the
           // first legal moves in any direction stay in bounds. The guid lets the
           // world thread stamp MovementState; spawn_time_ms = 0 seeds Δt.
           Position spawn;
           spawn.x = movement::kZoneMaxXY * 0.5f;  // 64 m — bootstrap play-area centre
           spawn.y = movement::kZoneMaxXY * 0.5f;
           spawn.z = movement::kFlatGroundZ;       // flat ground (D-19)
           ctx.movement.emplace(spawn, /*spawn_time_ms=*/0);
           ctx.movement->set_entity_guid(pc.char_guid);  // may be refined below (AoI)

           // OPS-05: this session is now IN WORLD. Bump CCU (meridian_ccu) and the
           // "active" session gauge (meridian_sessions{state="active"}); the serve
           // loop decrements both on disconnect (guarded by entered_metrics).
           metrics::ccu().with(ctx.labels.rzs()).inc();
           metrics::sessions().with({ctx.labels.realm, "active"}).inc();
           ctx.entered_metrics = true;

           // OPS-05 audit (#92): an authenticated session entered the world. Actor
           // is the account; target is the character; correlation is the grant —
           // pairs with the kSessionLeave record the serve loop emits (also guarded
           // by entered_metrics) for a complete in-world lifetime.
           audit::emit(audit::Record{
               .action = audit::Action::kSessionEnter,
               .outcome = audit::Outcome::kSuccess,
               .account_id = ctx.account_id,
               .target = pc.name,
               .correlation_id = ctx.grant_id,
           });

           // Reply ENTER_WORLD_RESPONSE(OK) BEFORE the AoI relay so the client
           // transitions in-world ahead of any relayed EntityEnter frames (mirrors
           // the old HandshakeOk-before-AoI ordering). Sent directly: egress is
           // created by the AoI block just below.
           ctx.phase = SessionPhase::kInWorld;
           reply(mn::EnterWorldStatus::OK);

           log::info(kCat, "ENTER_WORLD accepted -> spawned",
                     {log::field("account_id",
                                 static_cast<std::int64_t>(ctx.account_id)),
                      log::field("character_id",
                                 static_cast<std::int64_t>(pc.char_guid)),
                      log::field("character", pc.name),
                      log::field("class_id",
                                 static_cast<std::int64_t>(pc.class_id))});

           // AoI ENTER (#87): register this session in the shared world state so it
           // is tracked spatially and relayed to/from the OTHER in-range sessions.
           // Build its serialized, WorldSession-sealed s2c egress channel (the relay
           // writes to this socket from OTHER threads — the channel serializes those
           // with the serve loop's own writes; SAD §5.2/§6). enter() sends this
           // session an EntityEnter for anyone already in range and sends THEM one
           // for this character. Skipped when no world registry is wired (DB-less
           // dispatch smoke test).
           if (ctx.world != nullptr && ctx.session) {
               ctx.egress = std::make_shared<SessionEgress>(sess, *ctx.session);
               std::shared_ptr<SessionEgress> egress = ctx.egress;
               EntityIdentity id;
               id.entity_guid = pc.char_guid;
               id.type_id = pc.class_id;     // M0: class stands in for type_id
               id.char_class = pc.class_id;  // #328: relay the class so clients color by class
               EnterResult er = ctx.world->enter(
                   id, spawn,
                   [egress](net::Opcode op, const Bytes& payload) {
                       return egress->emit(op, payload);
                   });
               ctx.slot = er.slot;
               ctx.entered = true;
               // Stamp the EFFECTIVE guid the relay assigned onto the movement state
               // so the mover's own MovementState echo and its relayed
               // EntityEnter/Update all carry the same entity id.
               ctx.movement->set_entity_guid(er.entity_guid);
               metrics::aoi_entities()
                   .with(ctx.labels.rzsm())
                   .set(static_cast<double>(ctx.world->session_count()));
           }

           // SINGLE ACTIVE IN-WORLD SESSION PER ACCOUNT (#326). Admit this account
           // into the shared registry with a KICK-OLD policy: if the account already
           // has a live IN-WORLD session, that one is KICKED now and this entry takes
           // over (WoW-style; lets a crashed client reconnect without waiting out the
           // stale session). Per the D-35 decision the slot is held by the IN-WORLD
           // session, not a char-select one — so this lives on the ENTER_WORLD path.
           // The KickFn tears the DISPLACED session down from THIS thread via a
           // shared atomic flag, the thread-safe AoI world, and the victim's
           // SessionEgress (the same serialized cross-thread write path the relay uses).
           if (ctx.active_sessions != nullptr) {
               std::shared_ptr<SessionEgress> kick_egress = ctx.egress;
               std::shared_ptr<std::atomic<bool>> kick_flag = ctx.kicked;
               WorldState* kick_world = ctx.world;
               const SessionSlot kick_slot = ctx.slot;
               const bool kick_entered = ctx.entered;
               const std::string realm = ctx.labels.realm;
               AdmitResult admitted = ctx.active_sessions->admit(
                   ctx.account_id,
                   [kick_egress, kick_flag, kick_world, kick_slot, kick_entered, realm]() {
                       if (kick_flag) kick_flag->store(true);
                       if (kick_world != nullptr && kick_entered) kick_world->leave(kick_slot);
                       if (kick_egress) {
                           kick_egress->emit_frame(
                               make_disconnect(net::DisconnectReason::KICKED,
                                               "logged in from another location", 0));
                           kick_egress->mark_closed();
                       }
                       metrics::disconnects_total()
                           .with({realm,
                                  disconnect_reason_label(net::DisconnectReason::KICKED)})
                           .inc();
                   });
               ctx.session_token = admitted.token;
               ctx.admitted = true;
               if (admitted.kicked_previous) {
                   log::info(kCat,
                             "single-session takeover: kicked prior in-world session",
                             {log::field("account_id",
                                         static_cast<std::int64_t>(ctx.account_id)),
                              log::field("grant_id", ctx.grant_id)});
               }
           }
       });

    // --- 0x3xxx COMBAT: ability use (CMB-01 #344/#345) --------------------------
    // The D-10 ability-use path (server SAD §3.3, client SAD §2.2/§3c): validate
    // the use (known ability, caster alive, GCD clock, in-progress cast, resource,
    // target legality/range/LoS) and reply ACCEPT (CastStart) or REJECT (CastFailed)
    // so the client's optimistic GCD/cast confirms or ROLLS BACK within one RTT
    // (resyncing its GCD clock from CastFailed.gcd_remaining_ms). On an INSTANT
    // accept the server ALSO rolls the attack table and applies damage/heal
    // (CastResult, #345) — outcomes are server-only, never predicted. A cast-time
    // ability's CastResult is emitted on cast completion by the map tick (the
    // CombatSession::take_completed poll seam — the 20 Hz tick loop is #349).
    on(net::Opcode::CAST_REQUEST, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        // A cast before the handshake is a protocol error (mirrors MOVEMENT_INTENT).
        if (!ctx.authenticated) {
            log::warn(kCat, "CAST_REQUEST before handshake — rejecting");
            ctx.disconnect = true;
            ctx.disconnect_reason = net::DisconnectReason::PROTOCOL_MISMATCH;
            ctx.disconnect_message = "combat before handshake";
            return;
        }

        const auto* req = fb::GetRoot<mn::CastRequest>(f.payload);
        if (req == nullptr) return;  // verified upstream; defensive
        const AbilityId ability_id = req->ability_id();
        const std::uint64_t target_guid = req->target_guid();

        const std::uint64_t now_ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                           .count());

        auto fail = [&](mn::CastFailReason reason, std::uint32_t gcd_rem) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::CAST_FAILED, f.seq,
                                  encode_cast_failed(ability_id, reason, gcd_rem)));
        };

        // Must be SPAWNED in-world (authenticated char-select is not enough). A
        // cast at char-select is a client bug, not hostile — REJECT (roll back),
        // do not disconnect.
        if (ctx.phase != SessionPhase::kInWorld || !ctx.movement) {
            fail(mn::CastFailReason::NOT_IN_WORLD, 0);
            return;
        }

        // Known ability? (SAD §3.3 first check.) An unknown id is the client SAD
        // §2.4 "never crash" case — reject cleanly, do not disconnect.
        const Ability* ability = ctx.abilities ? ctx.abilities->find(ability_id) : nullptr;
        if (ability == nullptr) {
            fail(mn::CastFailReason::UNKNOWN_ABILITY, ctx.combat.gcd_remaining_ms(now_ms));
            return;
        }

        // Reach the caster's Unit (its combat state) via the world registry.
        Unit* caster = (ctx.world != nullptr) ? ctx.world->unit_for_slot(ctx.slot) : nullptr;
        if (caster == nullptr) {
            fail(mn::CastFailReason::NOT_IN_WORLD, 0);
            return;
        }
        const std::uint64_t caster_guid = ctx.movement->entity_guid();

        // Resolve the target Unit: self for a self ability / no target / own guid;
        // otherwise the entity named by guid (absent -> null -> a NO_TARGET reject
        // inside validate_target).
        Unit* target = nullptr;
        if (ability->target == TargetKind::kSelf || target_guid == 0 ||
            target_guid == caster_guid) {
            target = caster;
        } else {
            target = ctx.world->unit_for_guid(target_guid);
        }

        // Validate + start the GCD/cast lifecycle (#344). flat_map_los: the D-19
        // bootstrap map has no occluders (the LoS seam).
        CastDecision decision = begin_ability_use(ctx.combat, *ability, *caster, target,
                                                  target_guid, flat_map_los, now_ms);
        if (!decision.accepted) {
            fail(to_wire_reason(decision.reject), decision.gcd_remaining_ms);
            log::debug(kCat, "CAST_REQUEST rejected ability=" + std::to_string(ability_id) +
                                 " reason=" +
                                 std::to_string(static_cast<int>(decision.reject)));
            return;
        }

        // ACCEPT (D-10): confirm the client's optimistic GCD/cast.
        send_s2c(sess, ctx,
                 encode_frame(net::Opcode::CAST_START, f.seq,
                              encode_cast_start(ability_id, decision.cast_ms, now_ms)));

        // INSTANT: resolve now (#345) — spend the resource, roll the attack table,
        // apply damage/heal, trigger death at 0 HP. A cast-time ability resolves on
        // completion instead (map tick polls CombatSession::take_completed — #349).
        if (decision.instant && target != nullptr) {
            if (ability->resource_amount > 0) caster->spend_resource(ability->resource_amount);
            ResolveResult rr =
                resolve_ability(*ability, *caster, *target, ctx.world->combat_rng());
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::CAST_RESULT, f.seq,
                                  encode_cast_result(ability_id, caster_guid, target_guid,
                                                     to_wire_outcome(rr.outcome), rr.amount,
                                                     rr.is_heal, rr.target_health,
                                                     rr.target_died, now_ms)));
            log::debug(kCat, "CAST_REQUEST resolved ability=" + std::to_string(ability_id) +
                                 " outcome=" + std::to_string(static_cast<int>(rr.outcome)) +
                                 " amount=" + std::to_string(rr.amount));
        }
    });

    // No handler is registered for server→client opcodes (HANDSHAKE_OK,
    // MOVEMENT_STATE, ENTITY_ENTER/UPDATE/LEAVE, CHAR_*_RESPONSE): a client
    // sending one is out-of-direction and is treated as an unknown opcode
    // (Disconnect).
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

    // Shared world state + AoI grid (#87). Thread-safe internally; shared across
    // every serve_connection so a mover relays to the OTHER sessions in range.
    WorldState world;

    // Single active session per account (#326). Thread-safe internally; shared
    // across every serve_connection so a second login for an account kicks the
    // first (kick-old) — an account holds at most one in-world session.
    ActiveSessionRegistry active_sessions;

    // The read-only ability template store (#343), loaded once at construction
    // with the M1 placeholder set (epic #28 swaps the source behind the store).
    // Shared read-only across every serve_connection — the CAST_REQUEST handler
    // looks abilities up here with no lock (O(1), single load — client SAD §2.4).
    AbilityStore abilities = load_placeholder_ability_store();
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

WorldState& WorldServer::world_state() { return impl_->world; }

ActiveSessionRegistry& WorldServer::active_sessions() { return impl_->active_sessions; }

void WorldServer::world_thread_main() {
    // M0 TICK PLACEHOLDER (SAD §2.5): the real tick is
    //   drain inbound -> movement -> AI -> combat/auras -> spawns -> AoI delta -> flush
    // at 20 Hz. Here we ONLY drain the inbound queue at the configured cadence —
    // there are no maps to update yet. This is the seam the per-map update loop
    // replaces; the important property today is that this is the ONE thread that
    // owns simulation work (game state), so nothing on the IO/accept path does.
    using namespace std::chrono;
    const auto interval = milliseconds(cfg_.tick_interval_ms);

    // OPS-05: the tick-duration histogram (meridian_tick_duration_seconds
    // {realm,zone,shard,map}; p99 = tick health, the Realm-health headline). We
    // time the TICK BODY (drain + process), NOT the wait_for idle — the metric is
    // "how long the tick's work took", so an idle tick with an empty batch is not
    // measured (it would report ~0 and drown the real work in the histogram).
    auto& tick_hist = metrics::tick_duration_seconds().with(cfg_.labels.rzsm());

    while (!impl_->stop_requested.load()) {
        std::deque<WorldEvent> batch;
        {
            std::unique_lock<std::mutex> lk(impl_->q_mtx);
            impl_->q_cv.wait_for(lk, interval, [this] {
                return impl_->stop_requested.load() || !impl_->queue.empty();
            });
            batch.swap(impl_->queue);
        }

        if (batch.empty()) continue;  // idle tick — no work to time

        const auto tick_t0 = steady_clock::now();
        // "drain inbound" — process this tick's batch. At M0 the world thread has
        // no simulation to run against these events; it just accounts for them so
        // the queue -> world-thread path is observable (drained_count()).
        for (const WorldEvent& ev : batch) {
            (void)ev;
            impl_->drained.fetch_add(1);
        }

        // (movement / AI / combat / spawns / AoI / flush would run here)
        tick_hist.observe(duration<double>(steady_clock::now() - tick_t0).count());
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
    ctx.labels = cfg_.labels;   // OPS-05: stamp {realm,zone,shard,map} on metrics
    ctx.tracer = cfg_.tracer;   // OPS-05: session-flow trace exporter (#166)
    ctx.world = &impl_->world;  // shared AoI relay registry (#87)
    ctx.abilities = &impl_->abilities;  // shared ability template store (#343 / CMB-01)
    ctx.active_sessions = &impl_->active_sessions;  // single-session registry (#326)
    ctx.kicked = std::make_shared<std::atomic<bool>>(false);  // set if a later login kicks us
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

            // Single-session takeover (#326): a later login for this account has
            // kicked us. The KickFn already sent Disconnect{KICKED} + dropped us
            // from the AoI world; just stop serving so the socket tears down.
            if (ctx.kicked && ctx.kicked->load()) {
                log::info(kCat, "connection " + peer +
                                    " kicked by a newer login (single-session takeover)");
                break;
            }

            std::uint16_t op = 0;
            std::uint64_t seq = 0;
            DispatchOutcome outcome = dispatcher_.dispatch(sess, frame, ctx, op, seq);

            // A handler (e.g. WORLD_HELLO on a grant reject) can ask for a clean
            // close even though dispatch itself succeeded (kHandled).
            if (outcome == DispatchOutcome::kHandled && ctx.disconnect) {
                log::warn(kCat, "disconnecting " + peer + ": " + ctx.disconnect_message);
                // OPS-05: disconnect by reason (meridian_disconnects_total{realm,
                // reason}) — Player-experience dashboard.
                metrics::disconnects_total()
                    .with({ctx.labels.realm, disconnect_reason_label(ctx.disconnect_reason)})
                    .inc();
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
            // OPS-05: server-initiated disconnect by reason.
            metrics::disconnects_total()
                .with({ctx.labels.realm, disconnect_reason_label(reason)})
                .inc();
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

    // AoI world-leave (#87): if this session entered the world, tell everyone who
    // saw it that it despawned and drop it from the grid, THEN stop the egress
    // channel so no relay write races the socket close. Order matters: leave()
    // may still emit EntityLeave frames to OTHER sessions (safe — those write to
    // OTHER sockets), but no further write must target THIS socket after
    // mark_closed(), and none can once we drop out of the registry.
    if (ctx.entered && ctx.world != nullptr) {
        ctx.world->leave(ctx.slot);
        // OPS-05: AoI population dropped by one (meridian_aoi_entities).
        metrics::aoi_entities()
            .with(ctx.labels.rzsm())
            .set(static_cast<double>(ctx.world->session_count()));
    }
    if (ctx.egress) ctx.egress->mark_closed();

    // Single active session per account (#326): release this account's registry
    // slot. Compare-and-remove by token — if a newer login already took the slot
    // (this session was kicked), release() no-ops and leaves the new holder intact.
    if (ctx.admitted && ctx.active_sessions != nullptr) {
        ctx.active_sessions->release(ctx.account_id, ctx.session_token);
    }

    // OPS-05: this session is leaving — undo the CCU / active-session gauge bumps
    // it made on WORLD_HELLO (exactly once, guarded by entered_metrics), so the
    // Realm-health CCU panel tracks live sessions accurately.
    if (ctx.entered_metrics) {
        metrics::ccu().with(ctx.labels.rzs()).dec();
        metrics::sessions().with({ctx.labels.realm, "active"}).dec();
        // OPS-05 audit (#92): the session left the world. Attributed to the same
        // account + grant correlation as its session_enter, so an audit query can
        // pair enter/leave for a session's lifetime. Emitted once (entered_metrics
        // is the same exactly-once guard the gauges use). No secrets.
        audit::emit(audit::Record{
            .action = audit::Action::kSessionLeave,
            .outcome = audit::Outcome::kSuccess,
            .account_id = ctx.account_id,
            .correlation_id = ctx.grant_id,
        });
    }
    // OPS-05 net signal: connection closed (SAD §8.5 accept/close). `process`
    // label matches the accept counter's ("worldd").
    metrics::connections_closed_total().with({ctx.labels.realm, "worldd"}).inc();

    sess.close();
}

}  // namespace meridian::worldd
