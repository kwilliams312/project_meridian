// SPDX-License-Identifier: Apache-2.0
//
// worldd — IF-2 opcode dispatcher + world-process scaffold implementation
// (issues #82, #83). See world_dispatch.h for the provenance + clean-room
// statement and the M0 transport note.

#include "world_dispatch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "characters.h"  // meridian-characters CRUD: list/create/delete (#286 / D-35)
#include "creature_ai.h"  // CreatureSpawnDef — content spawns into the map tick (#486)
#include "currency.h"    // ECO-01 int64 copper: add_money / get_money (quest + trainer)
#include "db_content_store.h"  // SpawnPlacement — authored spawn placements (#486)
#include "economy_sanity.h"  // OPS-03b defense-in-depth economy delta checks (#421)
#include "equipment_service.h"  // authoritative durable equip/unequip/replace (#802)
#include "gm_command.h"  // OPS-02a GM command framework: registry + parse + gate + audit (#417)
#include "gossip.h"      // NPC-01 gossip menu planner (#372)
#include "item_store.h"  // ITM-01 durable item persistence: mint/place (loot + quest reward)
#include "map_tick.h"    // per-map tick orchestrator (SAD §2.5 phase order; #349)
#include "movement_audit.h"  // OPS-03a anti-cheat audit record builder (#420)
#include "meridian/bans/bans.h"  // OPS-02c ban/mute enforcement + issuance (#419)
#include "meridian/core/audit.hpp"
#include "meridian/core/log.hpp"
#include "meridian/metrics/catalog.h"
#include "meridian/trace/session_flow.h"
#include "meridian/trace/span.h"
#include "meridian/trace/tracer.h"
#include "npc_def.h"      // NPC-01/02 NpcStore / PlaceholderNpcStore (#372)
#include "quest_def.h"    // QST-01 QuestStore / PlaceholderQuestStore (#371)
#include "trainer.h"      // NPC-02 trainer plan/learn (#372)
#include "vendor.h"          // ECO-01 vendor buy/sell/buyback (#370)
#include "vendor_catalog.h"  // placeholder vendor inventories (M1; mcc #28 later)

namespace meridian::worldd {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace vend = meridian::vendor;
namespace itm = meridian::items;
namespace lo = meridian::loot;
namespace npc = meridian::npc;
namespace log = meridian::core::log;
namespace audit = meridian::core::audit;
namespace metrics = meridian::metrics::catalog;
namespace tr = meridian::trace;
namespace chr = meridian::characters;
namespace bans = meridian::bans;

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
        case net::Opcode::CHAT_MESSAGE:        return verify_table<mn::ChatMessage>(f);
        case net::Opcode::VENDOR_BUY_REQUEST:     return verify_table<mn::VendorBuyRequest>(f);
        case net::Opcode::VENDOR_SELL_REQUEST:    return verify_table<mn::VendorSellRequest>(f);
        case net::Opcode::VENDOR_BUYBACK_REQUEST: return verify_table<mn::VendorBuybackRequest>(f);
        case net::Opcode::EQUIPMENT_CHANGE_REQUEST:
            return verify_table<mn::EquipmentChangeRequest>(f);
        case net::Opcode::QUEST_ACCEPT:    return verify_table<mn::QuestAccept>(f);
        case net::Opcode::QUEST_TURN_IN:   return verify_table<mn::QuestTurnIn>(f);
        case net::Opcode::QUEST_LOG:       return verify_table<mn::QuestLog>(f);
        case net::Opcode::LOOT_REQUEST:    return verify_table<mn::LootRequest>(f);
        case net::Opcode::LOOT_TAKE:       return verify_table<mn::LootTake>(f);
        case net::Opcode::LOOT_RELEASE:    return verify_table<mn::LootRelease>(f);
        case net::Opcode::GOSSIP_HELLO:    return verify_table<mn::GossipHello>(f);
        case net::Opcode::TRAINER_LEARN:   return verify_table<mn::TrainerLearn>(f);
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
// character-select shape: id, name, race, class, level) plus a typed status.
// status=INTERNAL (with an empty roster) means the roster could NOT be read — a
// DB fault, NOT an empty account (#479): the client must render a load-error
// state rather than "you have no characters".
Bytes encode_char_list(const std::vector<chr::CharacterSummary>& roster,
                       mn::CharListStatus status) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::CharListEntry>> rows;
    rows.reserve(roster.size());
    for (const auto& c : roster) {
        auto name = b.CreateString(c.name);
        // §5.2 appearance record (morphs empty at M1). Built before the entry so
        // the nested table is a completed offset when CreateCharListEntry runs.
        auto appearance = mn::CreateAppearance(b, c.appearance.version,
                                               c.appearance.hair, c.appearance.face,
                                               c.appearance.skin);
        rows.push_back(mn::CreateCharListEntry(b, c.id, name, c.race, c.char_class,
                                               c.level, appearance));
    }
    auto vec = b.CreateVector(rows);
    b.Finish(mn::CreateCharListResponse(b, vec, status));
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

// ---- chat encoders (S→C, world.fbs; SOC-01 #367) ---------------------------

// Build a ChatRejected payload — the typed refusal reply. `target` echoes the
// attempted whisper target (empty for non-whisper) so the client can surface it.
Bytes encode_chat_rejected(mn::ChatChannel channel, mn::ChatRejectReason reason,
                           const std::string& target) {
    fb::FlatBufferBuilder b;
    auto t = b.CreateString(target);
    b.Finish(mn::CreateChatRejected(b, channel, reason, t));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// ---- vendor encoders + shared data (S→C, world.fbs; ECO-01 #370) ------------

// The M1 content stores behind the seams, shared read-only across all connections
// (thread-safe, immutable after construction — like the ability store). Each accessor
// returns the world-DB-backed store INSTALLED at boot (install_content_stores, #390)
// when one is present, else the M1 PLACEHOLDER set via a function-local static (a
// thread-safe lazy magic static, so DB-free unit tests keep working unchanged). The
// installed pointers are set ONCE at boot before any connection is served, and only
// ever read afterwards, so a plain non-owning pointer needs no lock. When mcc #28
// lands, the world DB is always present and the placeholder statics fall out of use.
const itm::TemplateStore* g_db_item_templates = nullptr;   // set by install_content_stores
const vend::VendorCatalog* g_db_vendor_catalog = nullptr;
const QuestStore* g_db_quest_store = nullptr;
const npc::NpcStore* g_db_npc_store = nullptr;

const itm::TemplateStore& item_templates() {
    if (g_db_item_templates != nullptr) return *g_db_item_templates;
    static const itm::PlaceholderTemplateStore store;
    return store;
}
const vend::VendorCatalog& vendor_catalog() {
    if (g_db_vendor_catalog != nullptr) return *g_db_vendor_catalog;
    static const vend::PlaceholderVendorCatalog cat;
    return cat;
}

Bytes encode_vendor_buy_result(mn::VendorBuyStatus status, std::uint32_t vendor_id,
                               std::uint32_t template_id, std::uint32_t quantity,
                               std::uint64_t item_guid, std::int64_t total_price,
                               std::int64_t balance) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuyResult(b, status, vendor_id, template_id, quantity,
                                       item_guid, total_price, balance));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_vendor_sell_result(mn::VendorSellStatus status, std::uint16_t backpack_slot,
                                std::uint32_t template_id, std::uint32_t quantity,
                                std::int64_t total_credit, std::int64_t balance,
                                std::uint16_t buyback_slot) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorSellResult(b, status, backpack_slot, template_id, quantity,
                                        total_credit, balance, buyback_slot));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_vendor_buyback_result(mn::VendorBuybackStatus status, std::uint32_t template_id,
                                   std::uint32_t quantity, std::uint64_t item_guid,
                                   std::int64_t price, std::int64_t balance,
                                   std::uint16_t buyback_slot) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuybackResult(b, status, template_id, quantity, item_guid,
                                           price, balance, buyback_slot));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_equipment_change_result(mn::EquipmentChangeStatus status,
                                     mn::EquipmentChangeAction action,
                                     std::uint16_t slot,
                                     std::uint8_t equipped_slot = 255) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEquipmentChangeResult(b, status, action, slot, equipped_slot));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Map the pure npc::QuestMarker (gossip.h) to its wire QuestMarkerKind (world.fbs).
// One-to-one, so the marker the client shows can never disagree with the planner.
mn::QuestMarkerKind to_wire(npc::QuestMarker m) {
    switch (m) {
        case npc::QuestMarker::kNone:             return mn::QuestMarkerKind::NONE;
        case npc::QuestMarker::kAvailable:        return mn::QuestMarkerKind::AVAILABLE;
        case npc::QuestMarker::kTurnInReady:      return mn::QuestMarkerKind::TURN_IN_READY;
        case npc::QuestMarker::kTurnInIncomplete: return mn::QuestMarkerKind::TURN_IN_INCOMPLETE;
    }
    return mn::QuestMarkerKind::NONE;
}

// Encode a QUEST_MARKER_UPDATE payload (#844/#849): the NPC's wire guid + the icon.
Bytes encode_quest_marker_update(std::uint64_t npc_guid, mn::QuestMarkerKind marker) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestMarkerUpdate(b, npc_guid, marker));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Best-effort current balance for a reject reply (the transaction changed nothing,
// so the client sees the unchanged money). Never throws — a DB fault degrades to 0.
std::int64_t safe_balance(ConnCtx& ctx) {
    if (ctx.char_db == nullptr || ctx.char_id == 0) return 0;
    try {
        return static_cast<std::int64_t>(itm::get_money(*ctx.char_db, ctx.char_id));
    } catch (...) {
        return 0;
    }
}

// ---- quest / npc shared read-only stores (M1 placeholder or DB-backed, #390) --
// Same pattern as item_templates()/vendor_catalog(): the DB-backed store installed
// at boot when a world DB is present, else the placeholder magic static.
const QuestStore& quest_store() {
    if (g_db_quest_store != nullptr) return *g_db_quest_store;
    static const PlaceholderQuestStore store;
    return store;
}
const npc::NpcStore& npc_store() {
    if (g_db_npc_store != nullptr) return *g_db_npc_store;
    static const npc::PlaceholderNpcStore store;
    return store;
}

// Adapt a session's QuestLog to the npc gossip planner's read-only QuestStateView
// (gossip.h). `can_accept` is gated at the session's server-authoritative level.
class QuestLogView : public npc::QuestStateView {
public:
    QuestLogView(const QuestLog& log, std::uint16_t level) : log_(log), level_(level) {}
    bool can_accept(std::uint32_t quest_id) const override {
        return log_.can_accept(quest_id, level_) == AcceptStatus::kOk;
    }
    bool is_active(std::uint32_t quest_id) const override { return log_.is_active(quest_id); }
    bool is_complete(std::uint32_t quest_id) const override { return log_.is_complete(quest_id); }

private:
    const QuestLog& log_;
    std::uint16_t level_;
};

// ---- quest wire enum mapping (lib status -> world.fbs status) ----------------

mn::QuestAcceptStatus to_wire(AcceptStatus s) {
    switch (s) {
        case AcceptStatus::kOk:                  return mn::QuestAcceptStatus::OK;
        case AcceptStatus::kUnknownQuest:        return mn::QuestAcceptStatus::UNKNOWN_QUEST;
        case AcceptStatus::kAlreadyActive:       return mn::QuestAcceptStatus::ALREADY_ACTIVE;
        case AcceptStatus::kAlreadyCompleted:    return mn::QuestAcceptStatus::ALREADY_COMPLETED;
        case AcceptStatus::kLevelTooLow:         return mn::QuestAcceptStatus::LEVEL_TOO_LOW;
        case AcceptStatus::kMissingPrerequisite: return mn::QuestAcceptStatus::MISSING_PREREQUISITE;
    }
    return mn::QuestAcceptStatus::UNKNOWN_QUEST;
}

// #838 — resolve a client-supplied giver / turn-in NPC guid to its npc_template id.
// A SPAWNED world NPC's wire guid lives in the kWorldEntityGuidBase band (assigned by
// install_spawns), NOT the low-32-bit npc_template id space — so the GOSSIP_HELLO path
// maps it back via WorldState::npc_template_for_guid before serving that NPC's content.
// The quest accept / turn-in giver checks MUST do the same resolution: otherwise a
// spawned giver's guid (e.g. 0xE000'0000'0000'0000, whose low 32 bits are 0) never
// matches def->giver_npc_id and the accept is wrongly rejected WRONG_GIVER — the bug the
// client saw as "clicking Accept does nothing". Falls back to treating the guid AS the
// template id, the pre-spawn 1:1 placeholder mapping (no world entity registered).
std::uint32_t resolve_npc_template_id(const ConnCtx& ctx, std::uint64_t wire_guid) {
    if (ctx.world != nullptr) {
        if (auto tmpl = ctx.world->npc_template_for_guid(wire_guid)) return *tmpl;
    }
    return static_cast<std::uint32_t>(wire_guid);
}

mn::QuestTurnInStatus to_wire(TurnInStatus s) {
    switch (s) {
        case TurnInStatus::kOk:            return mn::QuestTurnInStatus::OK;
        case TurnInStatus::kUnknownQuest:  return mn::QuestTurnInStatus::UNKNOWN_QUEST;
        case TurnInStatus::kNotActive:     return mn::QuestTurnInStatus::NOT_ACTIVE;
        case TurnInStatus::kWrongNpc:      return mn::QuestTurnInStatus::WRONG_NPC;
        case TurnInStatus::kIncomplete:    return mn::QuestTurnInStatus::INCOMPLETE;
        case TurnInStatus::kBadChoice:     return mn::QuestTurnInStatus::BAD_CHOICE;
        case TurnInStatus::kInventoryFull: return mn::QuestTurnInStatus::INVENTORY_FULL;
    }
    return mn::QuestTurnInStatus::UNKNOWN_QUEST;
}

mn::QuestObjectiveType to_wire(ObjectiveType t) {
    switch (t) {
        case ObjectiveType::kKill:    return mn::QuestObjectiveType::KILL;
        case ObjectiveType::kCollect: return mn::QuestObjectiveType::COLLECT;
        case ObjectiveType::kDeliver: return mn::QuestObjectiveType::DELIVER;
        case ObjectiveType::kExplore: return mn::QuestObjectiveType::EXPLORE;
    }
    return mn::QuestObjectiveType::KILL;
}

// The subject content id of an objective (creature / item / zone), for the wire
// QuestObjectiveState.target_id (the client labels progress without re-reading content).
std::uint32_t objective_target_id(const QuestObjective& o) {
    switch (o.type) {
        case ObjectiveType::kKill:    return o.target_npc_id;
        case ObjectiveType::kCollect: return o.item_id;
        case ObjectiveType::kDeliver: return o.item_id;
        case ObjectiveType::kExplore: return o.zone_id;
    }
    return 0;
}

// ---- quest encoders (S→C, world.fbs; QST-01 #371) ---------------------------

Bytes encode_quest_accept_result(QuestId quest_id, mn::QuestAcceptStatus status) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestAcceptResult(b, quest_id, status));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_quest_progress(QuestId quest_id, std::uint8_t objective_index,
                            mn::QuestObjectiveType type, std::uint16_t have,
                            std::uint16_t need, bool complete) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestProgress(b, quest_id, objective_index, type, have, need, complete));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_quest_turn_in_result(QuestId quest_id, mn::QuestTurnInStatus status,
                                  std::uint32_t reward_xp, std::int64_t reward_money,
                                  const std::vector<QuestRewardItem>& items,
                                  std::uint16_t new_level) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::QuestRewardItem>> rows;
    rows.reserve(items.size());
    for (const auto& it : items) rows.push_back(mn::CreateQuestRewardItem(b, it.item_id, it.count));
    auto vec = b.CreateVector(rows);
    b.Finish(mn::CreateQuestTurnInResult(b, quest_id, status, reward_xp, reward_money, vec,
                                         new_level));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Build the QuestLog snapshot (S→C) from a session's live QuestLog + the quest defs
// (for objective type/target, quest level, and the reward PREVIEW). Active quests
// only, ascending. The reward preview (always-granted items, one-of choice_items,
// flat XP + copper) is server-authoritative, sourced from the QuestDef, so the
// client can render the turn-in offer + choice picker straight from its log
// (issue #443) without a separate quest-detail round-trip.
Bytes encode_quest_log(const QuestLog& log) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::QuestLogEntry>> entries;
    for (QuestId id : log.active_quests()) {
        const QuestDef* def = quest_store().find(id);
        const std::vector<ObjectiveState>* obj = log.objectives(id);
        std::vector<fb::Offset<mn::QuestObjectiveState>> ostates;
        if (def != nullptr && obj != nullptr) {
            for (std::size_t i = 0; i < def->objectives.size() && i < obj->size(); ++i) {
                const QuestObjective& od = def->objectives[i];
                const ObjectiveState& os = (*obj)[i];
                ostates.push_back(mn::CreateQuestObjectiveState(
                    b, to_wire(od.type), objective_target_id(od), os.have, os.need,
                    os.complete()));
            }
        }
        auto ovec = b.CreateVector(ostates);

        // Reward preview (server-authoritative, from the QuestDef): the flat XP +
        // copper, the always-granted items, and the one-of choice options. Lets the
        // client render the turn-in offer + choice picker from its log alone (#443).
        std::vector<fb::Offset<mn::QuestRewardItem>> rewards;
        std::vector<fb::Offset<mn::QuestRewardItem>> choices;
        std::uint32_t reward_xp = 0;
        std::int64_t reward_money = 0;
        if (def != nullptr) {
            reward_xp = def->reward_xp;
            reward_money = static_cast<std::int64_t>(def->reward_money);
            rewards.reserve(def->reward_items.size());
            for (const auto& it : def->reward_items)
                rewards.push_back(mn::CreateQuestRewardItem(b, it.item_id, it.count));
            choices.reserve(def->choice_items.size());
            for (const auto& it : def->choice_items)
                choices.push_back(mn::CreateQuestRewardItem(b, it.item_id, it.count));
        }
        auto rvec = b.CreateVector(rewards);
        auto cvec = b.CreateVector(choices);

        entries.push_back(mn::CreateQuestLogEntry(
            b, id, def != nullptr ? def->level : std::uint16_t{1}, log.is_complete(id), ovec,
            reward_xp, reward_money, rvec, cvec));
    }
    auto qvec = b.CreateVector(entries);
    b.Finish(mn::CreateQuestLog(b, qvec));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// ---- loot wire enum mapping + encoders (S→C, world.fbs; ITM-02 #369) --------

mn::LootStatus to_wire(LootOpenStatus s) {
    switch (s) {
        case LootOpenStatus::kOk:            return mn::LootStatus::OK;
        case LootOpenStatus::kNotALooter:    return mn::LootStatus::NOT_A_LOOTER;
        case LootOpenStatus::kOutOfRange:    return mn::LootStatus::OUT_OF_RANGE;
        case LootOpenStatus::kNoSuchCorpse:  return mn::LootStatus::NO_SUCH_CORPSE;
        case LootOpenStatus::kAlreadyLooted: return mn::LootStatus::ALREADY_LOOTED;
    }
    return mn::LootStatus::NO_SUCH_CORPSE;
}

mn::LootTakeStatus to_wire(LootTakeResult s) {
    switch (s) {
        case LootTakeResult::kOk:            return mn::LootTakeStatus::OK;
        case LootTakeResult::kNotALooter:    return mn::LootTakeStatus::NOT_A_LOOTER;
        case LootTakeResult::kOutOfRange:    return mn::LootTakeStatus::OUT_OF_RANGE;
        case LootTakeResult::kAlreadyLooted: return mn::LootTakeStatus::ALREADY_LOOTED;
        case LootTakeResult::kQuestRequired: return mn::LootTakeStatus::QUEST_REQUIRED;
        case LootTakeResult::kInventoryFull: return mn::LootTakeStatus::INVENTORY_FULL;
        case LootTakeResult::kInvalidSlot:   return mn::LootTakeStatus::INVALID_SLOT;
        case LootTakeResult::kNoSuchCorpse:  return mn::LootTakeStatus::NO_SUCH_CORPSE;
    }
    return mn::LootTakeStatus::NO_SUCH_CORPSE;
}

// The rarity tier of a template (LootItem.quality), 0 (kPoor/common) when unknown.
std::uint8_t template_quality(std::uint32_t template_id) {
    const itm::ItemTemplate* t = item_templates().find(template_id);
    return t != nullptr ? static_cast<std::uint8_t>(t->rarity) : 0;
}

Bytes encode_loot_response(std::uint64_t corpse_guid, mn::LootStatus status,
                           std::int64_t copper, const std::vector<lo::LootSlotView>& slots) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::LootItem>> rows;
    rows.reserve(slots.size());
    for (const auto& v : slots) {
        rows.push_back(mn::CreateLootItem(b, static_cast<std::uint32_t>(v.slot),
                                          v.item_template_id, v.count,
                                          template_quality(v.item_template_id), v.is_quest()));
    }
    auto vec = b.CreateVector(rows);
    b.Finish(mn::CreateLootResponse(b, corpse_guid, status, copper, vec));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_loot_result(std::uint64_t corpse_guid, std::uint32_t slot,
                         mn::LootTakeStatus status, std::uint32_t template_id,
                         std::uint32_t count, std::int64_t copper) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootResult(b, corpse_guid, slot, status, template_id, count, copper));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_loot_closed(std::uint64_t corpse_guid) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootClosed(b, corpse_guid));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// ---- inventory-snapshot encoder (S→C, world.fbs; ITM-01 #453) ----------------

// The character's backpack contents + money (INVENTORY_SNAPSHOT S→C, #453). Only
// OCCUPIED backpack slots are emitted, keyed by their GRID index (0-based); each
// carries the item's rarity + bind rule from its template for the bags window.
// EQUIPMENT is intentionally excluded at M1 (bags window shows loose items only).
Bytes encode_inventory_snapshot(std::int64_t money, const itm::Inventory& inv) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::InventoryItem>> rows;
    const auto& slots = inv.backpack();
    rows.reserve(slots.size());
    for (std::uint16_t i = 0; i < slots.size(); ++i) {
        const std::optional<itm::ItemInstance>& s = slots[i];
        if (!s.has_value()) continue;  // omit empty slots
        std::uint8_t binding = 0;
        if (const itm::ItemTemplate* t = item_templates().find(s->template_id))
            binding = static_cast<std::uint8_t>(t->binding);
        rows.push_back(mn::CreateInventoryItem(b, i, s->template_id, s->stack,
                                               template_quality(s->template_id), binding));
    }
    auto vec = b.CreateVector(rows);
    std::vector<fb::Offset<mn::EquipmentItem>> equipped;
    const auto& paperdoll = inv.equipment();
    equipped.reserve(paperdoll.size());
    for (std::size_t i = 0; i < paperdoll.size(); ++i) {
        const auto& item = paperdoll[i];
        if (!item) continue;
        std::uint8_t binding = 0;
        if (const itm::ItemTemplate* t = item_templates().find(item->template_id))
            binding = static_cast<std::uint8_t>(t->binding);
        equipped.push_back(mn::CreateEquipmentItem(
            b, static_cast<std::uint8_t>(i), item->template_id,
            template_quality(item->template_id), binding));
    }
    b.Finish(mn::CreateInventorySnapshot(b, money, vec, inv.backpack_capacity(),
                                         b.CreateVector(equipped)));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// ---- EntityEnter visual-assembly builder (S→C; ②/T1 #538) --------------------

// Which paperdoll positions are VISUALLY rendered on an assembled character
// (client-character-assembler design §2/§4). Armour + weapons appear on the model
// and drive geoset hides / socketed weapons; jewellery (neck/finger/trinket) has
// no visual and is never broadcast (it would leak equipment state for no render
// gain). This is the "slot-visibility set" the design references; it lives here
// (not the items lib) because visibility is a RENDER concern, not an equip rule.
bool is_visual_equip_slot(itm::EquipSlot slot) {
    switch (slot) {
        case itm::EquipSlot::kNeck:
        case itm::EquipSlot::kFinger:
        case itm::EquipSlot::kTrinket:
            return false;  // jewellery — no visual
        default:
            return true;   // armour + weapons render on the body
    }
}

// Project a character's equipped set to the wire's visible-equipment list. Only
// OCCUPIED, VISUAL slots are emitted (an empty slot / jewellery is omitted), so an
// item_template is never 0. `dyes` is left EMPTY at M1: dye@1 is pck-only (#467),
// the world DB has no dye table, and no dye-application path writes item_instance.
// dyes yet — so worldd carries authored colours (design §2 dye-resolution note).
// The populated dye shape is exercised by the conformance corpus + the worldd
// entity-enter-visual unit test (hand-seeded numeric ids).
std::vector<EquippedVisualRec> visible_equipment_visuals(const itm::Inventory& inv) {
    std::vector<EquippedVisualRec> out;
    const auto& equipped = inv.equipment();
    for (std::size_t i = 0; i < equipped.size(); ++i) {
        const std::optional<itm::ItemInstance>& s = equipped[i];
        if (!s.has_value()) continue;  // empty paperdoll position
        const itm::EquipSlot slot = static_cast<itm::EquipSlot>(i);
        if (!is_visual_equip_slot(slot)) continue;  // jewellery — no visual
        EquippedVisualRec ev;
        ev.slot = static_cast<std::uint8_t>(i);
        ev.item_template = s->template_id;
        // ev.dyes stays empty at M1 (no dye-application path — see note above).
        out.push_back(std::move(ev));
    }
    return out;
}

// Assemble the EntityEnter visual-assembly block for a player from its loaded
// character row (race + §5.2 appearance) and equipment container (visible slots).
// This is the PLAYER source of EntityIdentity::visual; since npc@2 (contract ①/§7,
// #821) an NPC that carries an appearance_catalog also sets visual — but from the DB
// projection in install_spawns, not here. A model-only NPC leaves visual nullopt, so
// its EntityEnter omits race/appearance/equipment entirely.
//
// Best-effort on the equipment load, exactly like push_inventory_snapshot: a
// transient DB fault (or a minimal DB without the inventory tables) degrades to
// appearance-without-equipment rather than throwing into enter-world (spec §6:
// content problems degrade, never crash). Appearance + race always come through
// (they are already in hand on `pc`).
CharacterVisual build_character_visual(const LoadedCharacter& pc, db::Connection& char_db) {
    CharacterVisual vis;
    vis.race = pc.race;
    vis.sex = 0;  // M1 ships male only; the wire field reserves the additive future.
    vis.appearance_version = pc.appearance.version;
    vis.hair = pc.appearance.hair;
    vis.face = pc.appearance.face;
    vis.skin = pc.appearance.skin;
    try {
        const itm::Inventory inv =
            itm::load_inventory(char_db, pc.char_guid, item_templates());
        vis.equipment = visible_equipment_visuals(inv);
    } catch (const std::exception& e) {
        log::warn(kCat, "EntityEnter equipment load failed (visuals degrade)",
                  {log::field("error", e.what())});
    }
    return vis;
}

// ---- known-abilities encoder (S→C, world.fbs; CMB-01 #457) -------------------

// Project AbilityStore's AbilityResourceType to the wire AbilityResource enum. The
// two mirror each other 1:1 (world.fbs comment) so this is a straight cast; kNone
// maps to NONE (a free ability).
mn::AbilityResource to_wire(AbilityResourceType r) {
    switch (r) {
        case AbilityResourceType::kNone:   return mn::AbilityResource::NONE;
        case AbilityResourceType::kMana:   return mn::AbilityResource::MANA;
        case AbilityResourceType::kRage:   return mn::AbilityResource::RAGE;
        case AbilityResourceType::kEnergy: return mn::AbilityResource::ENERGY;
    }
    return mn::AbilityResource::NONE;
}

// The character's KNOWN ability set (KNOWN_ABILITIES S→C, #457): one row per learned
// ability, each carrying the cast/GCD/resource/range metadata the action bar (#456)
// needs, read from the AbilityStore (#343). The known set is `learned` (the M1
// in-memory learned set, #372); metadata comes from `store`. Ids are sorted so the
// wire order is DETERMINISTIC (the learned set is a hash set — unspecified order). A
// learned id with no AbilityStore entry (unresolvable — should not happen, since you
// can only learn what a trainer teaches) is SKIPPED with telemetry rather than
// emitted with no metadata (client SAD §2.4 "never crash"), keeping every row's
// metadata authoritative.
Bytes encode_known_abilities(const AbilityStore& store,
                             const npc::LearnedAbilitySet& learned) {
    fb::FlatBufferBuilder b;
    std::vector<AbilityId> ids = learned.ids();
    std::sort(ids.begin(), ids.end());
    std::vector<fb::Offset<mn::KnownAbility>> rows;
    rows.reserve(ids.size());
    for (AbilityId id : ids) {
        const Ability* a = store.find(id);
        if (a == nullptr) {
            log::warn(kCat, "KNOWN_ABILITIES: learned ability not in the store — skipping",
                      {log::field("ability_id", static_cast<std::int64_t>(id))});
            continue;
        }
        rows.push_back(mn::CreateKnownAbility(b, a->id, a->cast_time_ms, a->triggers_gcd,
                                              to_wire(a->resource_type), a->resource_amount,
                                              a->range_m));
    }
    auto vec = b.CreateVector(rows);
    b.Finish(mn::CreateKnownAbilities(b, vec));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// ---- vendor-catalog encoder (S→C, world.fbs; ECO-01 #453) --------------------

// The vendor's for-sale catalog (VENDOR_LIST S→C, #453): each listing's item id +
// its SERVER-COMPUTED buy price (override-or-template, via VendorCatalog::buy_price)
// + rarity + stock. A listing whose price cannot be resolved (unknown template with
// no override) is a stocked-but-not-purchasable row and is OMITTED — it is NOT_SOLD
// on the buy path, so it never appears in the buy window. Stock is -1 (unlimited) at
// M1 (limited stock is carried but unenforced — vendor_catalog.h).
Bytes encode_vendor_list(std::uint32_t vendor_id,
                         const std::vector<vend::VendorListing>& listings) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::VendorItem>> items;
    items.reserve(listings.size());
    for (const vend::VendorListing& l : listings) {
        const std::optional<itm::Copper> price =
            vend::VendorCatalog::buy_price(l, item_templates());
        if (!price) continue;  // not purchasable -> NOT_SOLD; keep it out of the window
        const std::int32_t stock =
            l.limited ? static_cast<std::int32_t>(l.limited->count) : std::int32_t{-1};
        items.push_back(mn::CreateVendorItem(b, l.item_template_id,
                                             static_cast<std::int64_t>(*price),
                                             template_quality(l.item_template_id), stock));
    }
    auto vec = b.CreateVector(items);
    b.Finish(mn::CreateVendorList(b, vendor_id, vec));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// ---- gossip / trainer wire enum mapping + encoders (S→C; NPC-01/02 #372) -----

mn::GossipOptionKind to_wire(npc::GossipOptionKind k) {
    switch (k) {
        case npc::GossipOptionKind::kQuestAvailable:  return mn::GossipOptionKind::QUEST_AVAILABLE;
        case npc::GossipOptionKind::kQuestInProgress: return mn::GossipOptionKind::QUEST_IN_PROGRESS;
        case npc::GossipOptionKind::kQuestComplete:   return mn::GossipOptionKind::QUEST_COMPLETE;
        case npc::GossipOptionKind::kVendor:          return mn::GossipOptionKind::VENDOR;
        case npc::GossipOptionKind::kTrainer:         return mn::GossipOptionKind::TRAINER;
    }
    return mn::GossipOptionKind::QUEST_AVAILABLE;
}

mn::TrainerLearnStatus to_wire(npc::TrainStatus s) {
    switch (s) {
        case npc::TrainStatus::kOk:                 return mn::TrainerLearnStatus::OK;
        case npc::TrainStatus::kNotTrainer:         return mn::TrainerLearnStatus::NOT_TRAINER;
        case npc::TrainStatus::kUnknownAbility:     return mn::TrainerLearnStatus::UNKNOWN_ABILITY;
        case npc::TrainStatus::kWrongClass:         return mn::TrainerLearnStatus::WRONG_CLASS;
        case npc::TrainStatus::kLevelTooLow:        return mn::TrainerLearnStatus::LEVEL_TOO_LOW;
        case npc::TrainStatus::kAlreadyKnown:       return mn::TrainerLearnStatus::ALREADY_KNOWN;
        case npc::TrainStatus::kInsufficientFunds:  return mn::TrainerLearnStatus::INSUFFICIENT_FUNDS;
    }
    return mn::TrainerLearnStatus::NOT_TRAINER;
}

// A trainer row's per-player learn eligibility (TrainableState) from its plan status.
mn::TrainableState trainable_state(npc::TrainStatus s) {
    switch (s) {
        case npc::TrainStatus::kOk:                return mn::TrainableState::LEARNABLE;
        case npc::TrainStatus::kAlreadyKnown:      return mn::TrainableState::ALREADY_KNOWN;
        case npc::TrainStatus::kWrongClass:        return mn::TrainableState::WRONG_CLASS;
        case npc::TrainStatus::kLevelTooLow:       return mn::TrainableState::LEVEL_TOO_LOW;
        case npc::TrainStatus::kInsufficientFunds: return mn::TrainableState::CANT_AFFORD;
        default:                                   return mn::TrainableState::LEARNABLE;
    }
}

Bytes encode_gossip_menu(std::uint64_t npc_guid, const npc::GossipMenu& menu) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::GossipOption>> rows;
    rows.reserve(menu.options.size());
    for (const auto& o : menu.options)
        rows.push_back(mn::CreateGossipOption(b, to_wire(o.kind), o.target_id));
    auto vec = b.CreateVector(rows);
    b.Finish(mn::CreateGossipMenu(b, npc_guid, vec));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// One trainer-list row as THIS player sees it (a projection of a TrainerAbility +
// the player's computed learn eligibility), for the TrainerList S→C wire table.
struct TrainerRow {
    std::uint32_t ability_id = 0;
    std::int64_t  cost = 0;
    std::uint8_t  required_class = 0;
    std::uint16_t required_level = 1;
    mn::TrainableState state = mn::TrainableState::LEARNABLE;
};

Bytes encode_trainer_list(std::uint64_t npc_guid, const std::vector<TrainerRow>& rows) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::TrainerListEntry>> entries;
    entries.reserve(rows.size());
    for (const auto& r : rows)
        entries.push_back(mn::CreateTrainerListEntry(b, r.ability_id, r.cost, r.required_class,
                                                     r.required_level, r.state));
    auto vec = b.CreateVector(entries);
    b.Finish(mn::CreateTrainerList(b, npc_guid, vec));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_trainer_learn_result(std::uint64_t npc_guid, std::uint32_t ability_id,
                                  mn::TrainerLearnStatus status, std::int64_t cost,
                                  std::int64_t new_balance) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateTrainerLearnResult(b, npc_guid, ability_id, status, cost, new_balance));
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// First free backpack index of `inv`, or nullopt when full (loot/quest reward
// placement target — mirrors the vendor buy path's first-free placement over the
// public backpack view; Inventory::first_free_backpack is private).
std::optional<std::uint16_t> first_free_slot(const itm::Inventory& inv) {
    const auto& slots = inv.backpack();
    for (std::uint16_t i = 0; i < slots.size(); ++i)
        if (!slots[i].has_value()) return i;
    return std::nullopt;
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
        // The current wire contract has one generic interruption reason for
        // crowd-control gates; retain the more specific reason in server logs.
        case CastReject::kCasterStunned:        return mn::CastFailReason::INTERRUPTED;
        case CastReject::kCasterSilenced:       return mn::CastFailReason::INTERRUPTED;
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

// Push the session's OWN INVENTORY_SNAPSHOT (S→C, #453): the character's live
// backpack contents + money, read straight off the characters DB. Sent at
// ENTER_WORLD and after every server-authoritative inventory change (loot / vendor
// / quest reward / GM .additem) so the bags window (#452) tracks durable state.
// Best-effort — a session with no characters DB (the DB-less dispatch smoke test)
// or a transient DB fault is skipped, never throwing into a handler. Reloads from
// the DB so the snapshot reflects the just-committed mutation.
void push_inventory_snapshot(net::Session& sess, ConnCtx& ctx) {
    if (ctx.char_db == nullptr || ctx.char_id == 0) return;
    try {
        const std::int64_t money =
            static_cast<std::int64_t>(itm::get_money(*ctx.char_db, ctx.char_id));
        const itm::Inventory inv =
            itm::load_inventory(*ctx.char_db, ctx.char_id, item_templates());
        send_s2c(sess, ctx, encode_frame(net::Opcode::INVENTORY_SNAPSHOT, 0,
                                         encode_inventory_snapshot(money, inv)));
    } catch (const std::exception& e) {
        log::warn(kCat, "INVENTORY_SNAPSHOT push failed", {log::field("error", e.what())});
    }
}

// Push the session's OWN KNOWN_ABILITIES (S→C, #457): the character's known ability
// set + per-ability cast/GCD/resource metadata, so the action bar (#456) seeds from
// the REAL learned set (#372) instead of a greybox one. Sent at ENTER_WORLD (next to
// the self VITALS_UPDATE #439 + INVENTORY_SNAPSHOT #453) and again on a TrainerLearn
// that grows the set. No-op without the ability store (the DB-less dispatch smoke
// path wires no store); a fresh character's set is empty (M1 has no durable ability
// table) and grows as it trains.
void push_known_abilities(net::Session& sess, ConnCtx& ctx) {
    if (ctx.abilities == nullptr) return;
    send_s2c(sess, ctx, encode_frame(net::Opcode::KNOWN_ABILITIES, 0,
                                     encode_known_abilities(*ctx.abilities, ctx.learned)));
}

// Push the OVERHEAD QUEST MARKERS (#844/#849) for every NPC currently in this
// session's interest set. For each visible content NPC (WorldState::visible_world_
// entities — the relay-tracked visibility, quest-free/DB-free), resolve its template
// via npc_store(), compute this player's marker (npc::compute_quest_marker over the
// SAME QuestLogView the gossip planner uses — so a `!`/`?` can never contradict the
// menu), diff it against the per-session last-marker cache, and emit a
// QUEST_MARKER_UPDATE only for a CHANGE. Called PROACTIVELY (no interaction) after
// enter-world / accept / progress / turn-in / on-sight movement so the icons track
// live state. Runs on this connection's own IO worker (ctx.quests + ctx.last_markers
// are single-threaded, like the rest of the per-connection quest state). No-op before
// spawn (no quest log / not entered).
void push_quest_markers(net::Session& sess, ConnCtx& ctx) {
    if (ctx.world == nullptr || !ctx.entered || !ctx.quests) return;
    const std::vector<VisibleWorldEntity> visible =
        ctx.world->visible_world_entities(ctx.slot);
    // Prune cache entries for NPCs no longer in view: the client dropped them on
    // ENTITY_LEAVE, so a later re-enter must re-push from a clean (default-none) slate.
    {
        std::unordered_set<std::uint64_t> live;
        live.reserve(visible.size());
        for (const VisibleWorldEntity& e : visible) live.insert(e.guid);
        for (auto it = ctx.last_markers.begin(); it != ctx.last_markers.end();) {
            if (live.count(it->first) == 0)
                it = ctx.last_markers.erase(it);
            else
                ++it;
        }
    }
    const QuestLogView view(*ctx.quests, ctx.char_level);
    for (const VisibleWorldEntity& e : visible) {
        const npc::NpcDef* def =
            npc_store().find(static_cast<npc::NpcId>(e.npc_template_id));
        const npc::QuestMarker marker =
            def != nullptr ? npc::compute_quest_marker(*def, view) : npc::QuestMarker::kNone;
        const auto it = ctx.last_markers.find(e.guid);
        const npc::QuestMarker prev =
            it != ctx.last_markers.end() ? it->second : npc::QuestMarker::kNone;
        if (marker == prev) continue;  // unchanged since the last push (or still default-none)
        if (marker == npc::QuestMarker::kNone)
            ctx.last_markers.erase(e.guid);  // cleared — back to the client's default (no icon)
        else
            ctx.last_markers[e.guid] = marker;
        send_s2c(sess, ctx,
                 encode_frame(net::Opcode::QUEST_MARKER_UPDATE, 0,
                              encode_quest_marker_update(e.guid, to_wire(marker))));
    }
}

// Snapshot every active quest's per-objective `have` (the pre-mutation baseline the
// QUEST_PROGRESS delta is computed against). Deterministic (std::map).
std::map<QuestId, std::vector<std::uint16_t>> snapshot_quest_haves(const QuestLog& log) {
    std::map<QuestId, std::vector<std::uint16_t>> before;
    for (QuestId id : log.active_quests()) {
        const std::vector<ObjectiveState>* objs = log.objectives(id);
        if (objs == nullptr) continue;
        std::vector<std::uint16_t> hv;
        hv.reserve(objs->size());
        for (const auto& o : *objs) hv.push_back(o.have);
        before.emplace(id, std::move(hv));
    }
    return before;
}

// Push a QUEST_PROGRESS (S→C) for every active objective whose `have` moved off its
// `before` baseline. Shared by every objective source (collect / kill / deliver /
// explore) so each emits the identical per-objective progress frame.
void emit_quest_progress_deltas(net::Session& sess, ConnCtx& ctx,
                                const std::map<QuestId, std::vector<std::uint16_t>>& before) {
    for (QuestId id : ctx.quests->active_quests()) {
        const QuestDef* def = quest_store().find(id);
        const std::vector<ObjectiveState>* objs = ctx.quests->objectives(id);
        if (def == nullptr || objs == nullptr) continue;
        auto prev_it = before.find(id);
        for (std::size_t i = 0; i < objs->size() && i < def->objectives.size(); ++i) {
            const std::uint16_t prev =
                (prev_it != before.end() && i < prev_it->second.size()) ? prev_it->second[i] : 0;
            const ObjectiveState& os = (*objs)[i];
            if (os.have == prev) continue;
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::QUEST_PROGRESS, 0,
                                  encode_quest_progress(id, static_cast<std::uint8_t>(i),
                                                        to_wire(def->objectives[i].type), os.have,
                                                        os.need, os.complete())));
        }
    }
}

// Apply-then-emit: snapshot the quest log, run `mutate` (an objective source —
// sync_collect / on_kill / on_deliver / on_explore), and if it advanced anything,
// push a QUEST_PROGRESS for each changed objective (the QST-01 apply-then-emit
// pattern all four sources share). No-op without a quest log. Returns whether
// anything advanced.
bool apply_and_emit_progress(net::Session& sess, ConnCtx& ctx,
                             const std::function<bool()>& mutate) {
    if (!ctx.quests) return false;
    const std::map<QuestId, std::vector<std::uint16_t>> before =
        snapshot_quest_haves(*ctx.quests);
    if (!mutate()) return false;  // nothing advanced
    emit_quest_progress_deltas(sess, ctx, before);
    // Objective progress can flip a turn-in NPC's marker from greyed `?` to lit `?`
    // (last objective just completed) — re-push the overhead markers (#844/#849).
    push_quest_markers(sess, ctx);
    return true;
}

// Re-sync every active COLLECT objective against the character's current inventory
// and push a QUEST_PROGRESS for each objective whose progress advanced (the QST-01
// collect source, driven by an inventory change — e.g. a loot pull). No-op without a
// quest log or when nothing changed. Called with the session's post-change inventory.
void sync_collect_and_emit(net::Session& sess, ConnCtx& ctx, const itm::Inventory& inv) {
    apply_and_emit_progress(sess, ctx, [&]() { return ctx.quests->sync_collect(inv); });
}

// Drain + apply this session's pending MapTick kill credits (QST-01 event-bus, #396)
// and push a QUEST_PROGRESS for each kill objective that advanced. Runs on the
// session's OWN IO worker (polled after each handled frame, next to
// poll_completed_cast) so ctx.quests stays single-threaded — the world thread only
// ever enqueues into the shared registry. Cheap no-op when nothing is pending.
void poll_quest_credits(net::Session& sess, ConnCtx& ctx) {
    if (ctx.quest_credit == nullptr || ctx.credit_guid == 0 || !ctx.quests) return;
    const std::vector<std::uint32_t> kills = ctx.quest_credit->drain_kills(ctx.credit_guid);
    if (kills.empty()) return;
    apply_and_emit_progress(sess, ctx, [&]() {
        bool changed = false;
        for (std::uint32_t npc_template_id : kills)
            changed |= ctx.quests->on_kill(npc_template_id);
        return changed;
    });
}

// Drain + apply this session's pending MapTick VITALS change (UI-01 event-bus, #437)
// and push a VITALS_UPDATE to the leveler + its AoI observers. Runs on the session's
// OWN IO worker (next to poll_quest_credits) so the WorldState-unit mutation stays on
// the single thread that owns this session's unit (the M1 per-connection model — the
// SAME worker the live combat path mutates + broadcasts on, so no new cross-thread
// race). The snapshot is the POST-change authoritative state (a level-up tops the
// player off to the new max health/power): we mirror it onto the world-owned Unit,
// then broadcast_vitals reads it straight back off that Unit. Cheap no-op when nothing
// is pending.
void poll_vitals_egress(net::Session& /*sess*/, ConnCtx& ctx) {
    if (ctx.vitals_egress == nullptr || ctx.credit_guid == 0 || ctx.world == nullptr ||
        !ctx.entered)
        return;
    std::optional<VitalsSnapshot> snap = ctx.vitals_egress->drain_vitals(ctx.credit_guid);
    if (!snap) return;

    Unit* unit = ctx.world->unit_for_slot(ctx.slot);
    if (unit == nullptr) return;

    // Mirror the new authoritative vitals onto the world-owned Unit: raise the caps
    // (a level-up only grows them, so no down-clamp), then top up current health/power
    // to the snapshot (for a level-up snap.health == snap.max_health and snap.power ==
    // snap.max_power — the leveler is healed to full). apply_healing/restore_resource
    // clamp to the new caps and never overshoot; both are no-ops when already at target.
    unit->set_level(snap->level);
    unit->set_max_health(snap->max_health);
    if (snap->health == 0) {
        unit->kill();
    } else if (unit->is_dead()) {
        unit->resurrect(snap->health);
    } else if (snap->health > unit->health()) {
        unit->apply_healing(snap->health - unit->health());
    } else if (snap->health < unit->health()) {
        unit->apply_damage(unit->health() - snap->health);
    }
    unit->set_max_resource(snap->max_power);
    if (snap->power > unit->resource()) unit->restore_resource(snap->power - unit->resource());
    // Keep the session's gate-level in step with the world Unit (mirrors GM `.setlevel`,
    // which sets both the ConnCtx gate level and the world Unit level).
    ctx.char_level = snap->level;

    // Push the VITALS_UPDATE to the subject's own client + every observer in AoI.
    ctx.world->broadcast_vitals(ctx.credit_guid);
}

// Apply a pending GM `.summon` forced move on THIS session's own IO worker (OPS-02b,
// #418). A summoning GM (on another thread) has already moved this session in the
// grid + relayed the AoI deltas, and POSTed the destination to ctx.forced_move; here
// — on the target's own worker, so ctx.movement stays single-threaded — we reset the
// authoritative position + ARM the forced-move ack barrier (#420, so the client's
// reconciling move is not flagged as a teleport), and snap the client with an
// authoritative MOVEMENT_STATE. Drained at the TOP of MOVEMENT_INTENT (barrier armed
// BEFORE that intent is validated) and after each handled frame (so an idle summoned
// player still snaps on its next action). Cheap no-op when nothing is queued.
void drain_forced_move(net::Session& sess, ConnCtx& ctx) {
    if (!ctx.forced_move || !ctx.movement) return;
    std::optional<Position> dest = ctx.forced_move->take();
    if (!dest) return;

    // The ack the client must echo before its intents resume validating. Must exceed
    // the last processed seq (force_correction's contract) — the client reconciles to
    // this MovementState and its next intent carries a seq >= it.
    const std::uint32_t ack = ctx.movement->last_seq() + 1;
    ctx.movement->force_correction(*dest, ack);
    if (ctx.simulation_enqueue) {
        WorldEvent ev;
        ev.kind = WorldEventKind::kPlayerMove;
        ev.player_guid = ctx.movement->entity_guid();
        ev.player_account_id = ctx.account_id;
        ev.player_session_token = ctx.session_token;
        ev.player_session_generation = ctx.session_generation;
        ev.player_pos = *dest;
        ctx.simulation_enqueue(std::move(ev));
    }

    const std::uint64_t server_time_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    MoveDecision snap;
    snap.accepted = true;
    snap.ack_seq = ack;
    snap.state_flags = ctx.movement->last_flags();
    snap.pos = *dest;
    send_s2c(sess, ctx,
             encode_frame(net::Opcode::MOVEMENT_STATE, 0,
                          encode_movement_state(ctx.movement->entity_guid(), snap,
                                                server_time_ms)));
    log::debug(kCat, "applied GM summon forced-move -> authoritative reset + ack barrier");
}

// Credit any explore quest objective satisfied by these area-trigger crossings
// (QST-01 explore source, #396). Fired on the session path where the mover's
// crossings are known (ENTER_WORLD spawn eval + MOVEMENT_INTENT). Only an ENTER
// crossing carrying a non-empty poi join key advances a matching explore objective
// (zone_id == area_id, poi == the crossing's poi); pushes QUEST_PROGRESS for each.
void credit_explore_triggers(net::Session& sess, ConnCtx& ctx,
                             const std::vector<TriggerEvent>& triggers) {
    if (!ctx.quests || triggers.empty()) return;
    apply_and_emit_progress(sess, ctx, [&]() {
        bool changed = false;
        for (const TriggerEvent& e : triggers) {
            if (!e.entered || e.poi.empty()) continue;
            changed |= ctx.quests->on_explore(e.area_id, e.poi);
        }
        return changed;
    });
}

// Credit any deliver quest objective satisfied by interacting with NPC `npc_id`
// (QST-01 deliver source, #396). Fired on the session path at the NPC-interact seam
// (GOSSIP_HELLO). For each active quest with a deliver objective whose to_npc == the
// interacted NPC, applies on_deliver(npc, item) — the deliver item was granted on
// accept, so the interaction completes the objective; pushes QUEST_PROGRESS for each.
void credit_deliver_at_npc(net::Session& sess, ConnCtx& ctx, std::uint32_t npc_id) {
    if (!ctx.quests) return;
    apply_and_emit_progress(sess, ctx, [&]() {
        bool changed = false;
        for (QuestId id : ctx.quests->active_quests()) {
            const QuestDef* def = quest_store().find(id);
            if (def == nullptr) continue;
            for (const QuestObjective& obj : def->objectives) {
                if (obj.type == ObjectiveType::kDeliver && obj.to_npc_id == npc_id)
                    changed |= ctx.quests->on_deliver(npc_id, obj.item_id);
            }
        }
        return changed;
    });
}

// Whether a chat body is empty or all-whitespace (#367). Not a content filter —
// just the shape check that rejects a blank line (no profanity/content filter at
// M1). ASCII whitespace is sufficient for the M1 chat surface.
bool is_blank(const std::string& s) {
    for (unsigned char c : s) {
        if (std::isspace(c) == 0) return false;
    }
    return true;
}

// A monotonic ms clock for the GCD/cast timers (steady, never wall-clock).
std::uint64_t steady_now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Resolve a cast-time ability whose timer has elapsed — the #354 deferral
// ("cast-time resolution on cast completion + resource-spend-for-casts to the tick
// loop") CLOSED by #349. The instant path (CAST_REQUEST handler) resolves inline;
// this is its cast-time twin: on completion SPEND the resource (deferred from
// begin_ability_use), roll the attack table + apply damage/heal (resolve_ability),
// and send the server-authoritative CastResult (or CastFailed if the target died /
// left / moved out of range while the cast ran). Polled on the serve loop because
// the per-connection CombatSession (ctx.combat) is owned by this IO worker; a
// client streams movement/clock frames at >= 20 Hz, so a completed cast resolves
// within ~one tick of its end. The multi-worker map manager migrates this onto the
// world thread at M3 (SAD §2.5 "recast at M3"). No-op when no cast is pending.
void poll_completed_cast(net::Session& sess, ConnCtx& ctx, std::uint64_t now_ms) {
    if (ctx.phase != SessionPhase::kInWorld || ctx.world == nullptr || !ctx.movement) return;
    std::optional<PendingCast> done = ctx.combat.take_completed(now_ms);
    if (!done) return;

    const Ability* ability = ctx.abilities ? ctx.abilities->find(done->ability_id) : nullptr;
    Unit* caster = ctx.world->unit_for_slot(ctx.slot);
    if (ability == nullptr || caster == nullptr) return;
    const std::uint64_t caster_guid = ctx.movement->entity_guid();

    ObjectGuid target_guid = done->target_guid;
    Unit* target = nullptr;
    if (ability->target == TargetKind::kSelf || target_guid == 0 ||
        target_guid == caster_guid) {
        target = caster;
        target_guid = caster_guid;
    } else {
        target = ctx.world->unit_for_guid(target_guid);
    }

    // Re-validate on completion (the target may have died / left / moved off).
    const CastReject why = validate_target(*ability, *caster, target, flat_map_los);
    if (why != CastReject::kNone || target == nullptr) {
        send_s2c(sess, ctx,
                 encode_frame(net::Opcode::CAST_FAILED, 0,
                              encode_cast_failed(done->ability_id, to_wire_reason(why), 0)));
        return;
    }

    // Resource is spent AT RESOLUTION (begin_ability_use deferred it — #354).
    if (ability->resource_amount > 0) caster->spend_resource(ability->resource_amount);
    const ResolveResult rr =
        resolve_ability(*ability, *caster, *target, ctx.world->combat_rng());
    send_s2c(sess, ctx,
             encode_frame(net::Opcode::CAST_RESULT, 0,
                          encode_cast_result(done->ability_id, caster_guid, target_guid,
                                             to_wire_outcome(rr.outcome), rr.amount,
                                             rr.is_heal, rr.target_health, rr.target_died,
                                             now_ms)));
    // VITALS_UPDATE (#430, UI-01): the resolution moved the TARGET's health (damage
    // or heal) and the CASTER's power (resource spent at resolution above). Broadcast
    // both to every client in AoI so the unit frames update without a re-enter — the
    // server-authoritative HUD delta (the client never predicts these, Principle 1).
    ctx.world->broadcast_vitals(target_guid);
    if (caster_guid != target_guid) ctx.world->broadcast_vitals(caster_guid);
    log::debug(kCat, "cast completed ability=" + std::to_string(done->ability_id) +
                         " outcome=" + std::to_string(static_cast<int>(rr.outcome)) +
                         " amount=" + std::to_string(rr.amount));
}

}  // namespace

// ---------------------------------------------------------------------------
// Content-store installation seam (#390)
// ---------------------------------------------------------------------------
// Swap the seams the QUEST/GOSSIP/VENDOR handlers read from the placeholder set to
// the world-DB-backed stores. Called ONCE at boot (main()), AFTER the IF-4 manifest
// check succeeds, before any connection is served. The referenced stores must
// outlive every served connection (main() owns the WorldContent bundle for the
// process lifetime). A nullptr leaves that seam on its placeholder default. Not
// thread-safe — a boot-time, single-threaded set (read-only afterwards).
void install_content_stores(const items::TemplateStore* item_store,
                            const vendor::VendorCatalog* vendor,
                            const QuestStore* quests,
                            const npc::NpcStore* npcs) {
    g_db_item_templates = item_store;
    g_db_vendor_catalog = vendor;
    g_db_quest_store = quests;
    g_db_npc_store = npcs;
}

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

    // OPS-03b (#421): per-opcode RATE CLASS. Every client opcode belongs to a rate
    // class (session/move/chat/action) with a per-session sliding-window ceiling;
    // a frame over its class ceiling is a FLOOD — DROP it (do NOT disconnect;
    // throttling is not a protocol error) and flag it on the anti-cheat audit
    // stream (kRateLimited) + the Errors-dashboard drop counter (reason="rate_class",
    // distinct from the finer per-feature "rate_limit" ChatIntake/MovementIntake
    // emit). A dropped frame keeps the connection open (return kHandled: nothing to
    // close over), so the serve loop simply reads the next frame.
    //
    // The MOVE class DEFERS to MovementIntake (#86/#420) — that gate is the
    // authoritative movement rate limiter, and it is keyed on the intent's CLIENT
    // time + state-change semantics (≤ kMovementIntentMaxHz per client clock, mode
    // toggles always admitted). Enforcing a SECOND, wall-clock ceiling on
    // MOVEMENT_INTENT here would double-reject a legitimately-paced burst (a client
    // paces intents by its own clock, which need not match the server's wall time),
    // so the dispatcher leaves movement throttling entirely to MovementIntake and
    // this backstop covers the session/chat/action opcodes (see rate_class.h).
    const RateClass rc = rate_class_for(f.opcode);
    if (rc != RateClass::kMove && !ctx.rate.admit(rc, steady_now_ms())) {
        metrics::opcode_dropped_total()
            .with(ctx.labels.rzs_opcode_reason(op, "rate_class"))
            .inc();
        audit::emit(build_rate_limited_audit(ctx.account_id, ctx.grant_id, f.opcode, rc));
        log::warn(kCat, "opcode " + op + " dropped (rate class " +
                            std::string(rate_class_name(rc)) + " flood)");
        return DispatchOutcome::kHandled;
    }

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
           // The session learns its GM level here (OPS-02a, #417) — the grant
           // consume's JOIN to account carried account.gm_level. Authoritative
           // for the whole session; the chat-path GM command gate reads it.
           ctx.gm_level = consumed->gm_level;
           audit::emit(audit::Record{
               .action = audit::Action::kGrantConsumed,
               .outcome = audit::Outcome::kSuccess,
               .account_id = consumed->account_id,
               .target = "realm:" + std::to_string(consumed->realm_id),
               .correlation_id = grant_id,
           });

           // OPS-02c (#419): DROP a banned account's session at the handshake, even
           // though authd already refuses a banned login — a ban can land AFTER the
           // grant was issued but before this WorldHello, and worldd is the authority
           // that owns the live session (SAD §5.3 "gatewayd/worldd refuses a banned
           // grant"). Also re-check the source IP (defense in depth). The grant is
           // already spent (single-use); a banned actor simply gets no session. A
           // moderation-lookup DB fault fails OPEN (a transient hiccup must not drop a
           // legitimate session — the account/IP were already vetted at authd login).
           {
               std::optional<bans::Active> ban;
               const char* ban_reason = nullptr;
               std::string ban_target;
               try {
                   if ((ban = bans::account_ban(*ctx.db, consumed->account_id))) {
                       ban_reason = "account_banned";
                       ban_target = "account:" + std::to_string(consumed->account_id);
                   } else {
                       const std::string ip = bans::ip_of_peer(sess.peer());
                       if (!ip.empty() && (ban = bans::ip_ban(*ctx.db, ip))) {
                           ban_reason = "ip_banned";
                           ban_target = "ip:" + ip;
                       }
                   }
               } catch (const db::DbError& e) {
                   log::warn(kCat, "WORLD_HELLO ban lookup failed (fail-open)",
                             {log::field("error", e.what())});
               }
               if (ban_reason != nullptr) {
                   audit::emit(audit::Record{
                       .action = audit::Action::kBanRejected,
                       .outcome = audit::Outcome::kFailure,
                       .account_id = consumed->account_id,
                       .target = ban_target,
                       .reason = ban_reason,
                       .correlation_id = grant_id,
                       .peer = sess.peer(),
                   });
                   log::warn(kCat, "WORLD_HELLO refused — banned",
                             {log::field("account_id",
                                         static_cast<std::int64_t>(consumed->account_id)),
                              log::field("reason", ban_reason)});
                   ctx.disconnect = true;
                   ctx.disconnect_reason = net::DisconnectReason::KICKED;
                   ctx.disconnect_message = "you are banned";
                   return;
               }
           }

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

        // GM `.summon` (OPS-02b, #418): apply any pending forced move BEFORE this
        // intent is validated, so the ack barrier is armed and the authoritative
        // position is the summoned point — this incoming intent (likely a stale
        // pre-summon packet) is then correctly rejected/reconciled against the new
        // position instead of tripping the anti-cheat teleport check.
        drain_forced_move(sess, ctx);

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

        // R2..R9 — validate against the shared constants (the full OPS-03a envelope:
        // speed windows, teleport/ack, bounds, z, flag legality), then commit the
        // decision to the authoritative state (advance on accept, snap-back on
        // reject). At M0 the per-connection movement state is single-threaded by
        // construction (one IO worker per connection); the seam onto the shared
        // world thread is the WorldServer queue (#87). `in_liquid` is false at M0 —
        // the flat bootstrap map (D-19) has NO liquid volume, so a Swim-mode intent
        // is always the "swim on dry land" illegal-flag reject; the M1 water-volume
        // sampler fills this seam unchanged (SAD §5.5 "swim only in liquid volumes").
        MoveDecision decision =
            ctx.movement->validate_move(pod, pod.client_time_ms, /*in_liquid=*/false);
        ctx.movement->apply(decision, pod, pod.client_time_ms);

        // OPS-05 + OPS-03a: a REJECTED intent is a movement-check violation (by kind
        // — meridian_movement_violations_total{...,kind}), a snap-back correction to
        // the client (meridian_movement_corrections_total{...}), AND an append-only
        // ANTI-CHEAT AUDIT flag on the OPS-05 audit stream (server PRD §6 / SAD §5.5
        // policy ladder). The metrics feed the Player-experience dashboard panels;
        // the audit record is the durable, GM-reviewable anti-cheat trail.
        if (!decision.accepted) {
            metrics::movement_violations_total()
                .with({ctx.labels.realm, ctx.labels.zone, ctx.labels.shard,
                       move_reject_kind(decision.reject)})
                .inc();
            metrics::movement_corrections_total().with(ctx.labels.rzs()).inc();
            // Anti-cheat audit (#420): attribute the flag to the session's account +
            // grant, name the offending entity + reject cause + snapped-back position.
            // No secret material (see movement_audit.h / core::audit no-secrets rule).
            audit::emit(build_movement_reject_audit(
                ctx.account_id, ctx.grant_id, ctx.movement->entity_guid(),
                decision.reject, decision.pos, decision.ack_seq));
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
            const std::vector<TriggerEvent> crossings = ctx.world->on_movement(
                ctx.slot, auth, decision.ack_seq, decision.state_flags, server_time_ms);
            if (ctx.simulation_enqueue) {
                WorldEvent ev;
                ev.kind = WorldEventKind::kPlayerMove;
                ev.player_guid = ctx.movement->entity_guid();
                ev.player_account_id = ctx.account_id;
                ev.player_session_token = ctx.session_token;
                ev.player_session_generation = ctx.session_generation;
                ev.player_pos = auth;
                ctx.simulation_enqueue(std::move(ev));
            }
            // Explore (QST-01, #396): credit any explore objective the mover's
            // area-trigger crossings satisfy (QUEST_PROGRESS follows if it advanced).
            credit_explore_triggers(sess, ctx, crossings);
            // ON-SIGHT overhead markers (#844/#849): moving may bring new NPCs into
            // the interest set — push their `!`/`?` marker (diffed, so NPCs already
            // shown re-emit nothing). credit_explore_triggers above may have pushed
            // too; the diff cache makes a second call here idempotent.
            push_quest_markers(sess, ctx);
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
           // OK only if the roster was actually READ. A DB fault (or no char_db)
           // must surface as INTERNAL, NOT a silent empty roster (#479): an empty
           // roster under OK means "zero characters", which contradicts a create
           // that still counts the existing rows and refuses with the limit.
           mn::CharListStatus status = mn::CharListStatus::OK;
           if (ctx.char_db != nullptr) {
               try {
                   roster = chr::list_characters(*ctx.char_db, ctx.account_id);
               } catch (const db::DbError& e) {
                   // Typed error channel (world.fbs CharListStatus): report the DB
                   // fault to the client so char-select renders a load-error state
                   // instead of "no characters". Production always has char_db.
                   status = mn::CharListStatus::INTERNAL;
                   log::warn(kCat, "CHAR_LIST_REQUEST DB error — INTERNAL status",
                             {log::field("account_id",
                                         static_cast<std::int64_t>(ctx.account_id)),
                              log::field("error", e.what())});
               }
           } else {
               status = mn::CharListStatus::INTERNAL;
               log::warn(kCat, "CHAR_LIST_REQUEST with no characters DB — INTERNAL status");
           }
           log::debug(kCat, "CHAR_LIST_REQUEST -> " + std::to_string(roster.size()) +
                                " character(s) (status=" +
                                std::to_string(static_cast<std::uint16_t>(status)) +
                                ") for account " + std::to_string(ctx.account_id));
           send_s2c(sess, ctx,
                    encode_frame(net::Opcode::CHAR_LIST_RESPONSE, f.seq,
                                 encode_char_list(roster, status)));
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
               // §5.2 appearance: pass the client's chosen record through opaquely
               // (absent ⇒ nullopt ⇒ the CRUD stores the server default). It is
               // bounded, never gameplay-authoritative — create_character clamps
               // it and NEVER rejects on it (no new CharCreateStatus).
               if (const auto* ap = req->appearance()) {
                   chr::AppearanceRecord rec;
                   rec.version = ap->version();
                   rec.hair = ap->hair();
                   rec.face = ap->face();
                   rec.skin = ap->skin();
                   cr.appearance = rec;
               }
               try {
                   // Validate against the boot-loaded roster (pack `race`/`class`
                   // rows merged with the compiled fallback, #695); fall back to the
                   // full offline M0 set for a directly-built ConnCtx (dispatch smoke).
                   const chr::Roster& roster =
                       ctx.roster != nullptr ? *ctx.roster : chr::Roster::offline_full();
                   minted =
                       chr::create_character(*ctx.char_db, cr, roster).character_id;
               } catch (const chr::CharacterLimitReached&) {
                   status = mn::CharCreateStatus::LIMIT_REACHED;
               } catch (const chr::DuplicateName&) {
                   status = mn::CharCreateStatus::DUPLICATE_NAME;
               } catch (const chr::InvalidRace&) {
                   status = mn::CharCreateStatus::INVALID_RACE;
               } catch (const chr::InvalidRaceForClass&) {
                   // Race valid but not permitted for the chosen class (class
                   // race_limits gate, #696). Surfaced to the client as
                   // INVALID_RACE — the chosen race cannot be used with this
                   // class — reusing the existing wire status rather than
                   // expanding the CharCreateStatus enum (no protocol/client
                   // change): the C++ exception stays distinct for the server.
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

           // OPS-02c (#419): refuse ENTER_WORLD for a BANNED character. The account
           // owns it (proven above) but this specific character is barred — the
           // account may still play its others, so this is a per-character gate, not
           // an account drop. Checked against the auth DB (bans live there, §4.1). A
           // moderation-lookup fault fails OPEN (do not block a legitimate spawn on a
           // transient DB hiccup). On a ban: audit + disconnect KICKED (a ban is a
           // kick; there is no BANNED enter-world status and world.fbs is unchanged).
           if (ctx.db != nullptr) {
               try {
                   if (std::optional<bans::Active> cb =
                           bans::character_ban(*ctx.db, character_id)) {
                       audit::emit(audit::Record{
                           .action = audit::Action::kBanRejected,
                           .outcome = audit::Outcome::kFailure,
                           .account_id = ctx.account_id,
                           .target = "character:" + std::to_string(character_id),
                           .reason = "character_banned",
                           .correlation_id = ctx.grant_id,
                       });
                       log::warn(kCat, "ENTER_WORLD refused — character banned",
                                 {log::field("account_id",
                                             static_cast<std::int64_t>(ctx.account_id)),
                                  log::field("character_id",
                                             static_cast<std::int64_t>(character_id))});
                       ctx.disconnect = true;
                       ctx.disconnect_reason = net::DisconnectReason::KICKED;
                       ctx.disconnect_message = "this character is banned";
                       return;
                   }
               } catch (const db::DbError& e) {
                   log::warn(kCat, "ENTER_WORLD character-ban lookup failed (fail-open)",
                             {log::field("error", e.what())});
               }
           }

           // --- OK: spawn the REAL owned character in-world ----------------------
           const LoadedCharacter& pc = *loaded;

           // Seed the authoritative movement state (#86) at the character's spawn.
           // C8 enter-as-chibi (#761): spawn at the realm's START ZONE first graveyard
           // (zone.schema.yaml "start_zone: spawn point = first graveyard"), resolved
           // from the loaded pack at boot (load_start_zone_spawn) and borrowed here by
           // address (ctx.enter_spawn) already in the worldd Z-up runtime frame + facing.
           // Pack-driven: the chibi realm drops the character into Sprout Meadow
           // (graveyard at origin); any theme works the same way. When no start zone was
           // loaded (DB-less smoke path / degraded or start-zone-less world DB) the
           // D-11 PLACEHOLDER stands in — Zone-01 flat-ground play-area centre (#562:
           // kZoneSpawnXY = -320 m) so the first legal moves stay in bounds. The guid
           // lets the world thread stamp MovementState; spawn_time_ms = 0 seeds Δt.
           Position spawn;
           if (ctx.enter_spawn != nullptr) {
               spawn = *ctx.enter_spawn;        // start-zone graveyard (server frame + facing)
           } else {
               spawn.x = movement::kZoneSpawnXY;   // -320 m — Zone-01 play-area centre (placeholder)
               spawn.y = movement::kZoneSpawnXY;
               spawn.z = movement::kFlatGroundZ;   // flat ground (D-19)
           }
           ctx.movement.emplace(spawn, /*spawn_time_ms=*/0);
           ctx.movement->set_entity_guid(pc.char_guid);  // may be refined below (AoI)

           // ECO-01 (#370): capture the spawned character's id (== character.id,
           // the currency/inventory key) so the vendor buy/sell/buyback handlers
           // act on the session's OWN character (server-authoritative).
           ctx.char_id = pc.char_guid;

           // QST-01 / NPC-02 (#371/#372/#388): capture the character's class + level
           // (the server-authoritative accept/learn gates — read from the DB-loaded
           // character, never a client field) and stand up this session's quest state
           // machine over the shared placeholder quest store. Quest progress is
           // in-memory at M1 (durable character_quest is a later story).
           ctx.char_class = pc.class_id;
           ctx.char_level = pc.level;
           ctx.quests.emplace(quest_store());

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

           // NOTE (#388): the character's quest log is NOT pushed unsolicited on
           // enter-world — that would inject a frame into the strict enter-world reply
           // sequence other flows (char-mgmt, AoI) depend on. The client fetches its
           // initial log with an explicit QUEST_LOG request (world.fbs QuestLog "sent
           // on … resync"); the server also resends it after each accept / turn-in.

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
               id.name = pc.name;            // #367: the whisper name key + ChatDeliver sender_name
               // ②/T1 (#538): resolve this PLAYER's visual-assembly block — race +
               // §5.2 appearance (from the loaded row) + the visible equipped set.
               // Relayed on every EntityEnter for this session so observers can
               // assemble the character; NPCs never carry it. Degrades gracefully
               // if the equipment load faults (see build_character_visual).
               id.visual = build_character_visual(pc, *ctx.char_db);
               EnterResult er = ctx.world->enter(
                   id, spawn,
                   [egress](net::Opcode op, const Bytes& payload) {
                       return egress->emit(op, payload);
                   });
               ctx.slot = er.slot;
               ctx.entered = true;
               if (Unit* world_unit = ctx.world->unit_for_slot(ctx.slot))
                   world_unit->set_level(pc.level);
               // Stamp the EFFECTIVE guid the relay assigned onto the movement state
               // so the mover's own MovementState echo and its relayed
               // EntityEnter/Update all carry the same entity id.
               ctx.movement->set_entity_guid(er.entity_guid);
               metrics::aoi_entities()
                   .with(ctx.labels.rzsm())
                   .set(static_cast<double>(ctx.world->session_count()));

               // SELF VITALS SNAPSHOT AT SPAWN (#439, UI-01 HUD contract; part of
               // #24). enter() sends the OTHER in-range sessions an EntityEnter for
               // this character (carrying its vitals), but sends the OWNING session
               // NO self EntityEnter and NO vitals — so the client HUD player frame
               // had nothing to show until the first combat/heal delta (#438 worked
               // around it by seeding name/level from char-select). Fix: push the
               // owning session an authoritative VITALS_UPDATE for its OWN character
               // now, at spawn. broadcast_vitals(guid) always sends the subject's own
               // client the VITALS_UPDATE (recipient #1), reading health/max, power/
               // max + type, and level straight off the authoritative Unit — so the
               // HUD populates immediately. We choose a self VITALS_UPDATE over a self
               // EntityEnter because the client already handles VITALS_UPDATE for its
               // player frame (#438) and treats EntityEnter as REMOTE entities to
               // spawn (a self EntityEnter would risk a duplicate-of-self capsule). No
               // new opcode — VITALS_UPDATE already exists (#430). Any observers that
               // JUST received this character's EntityEnter get an idempotent repeat
               // of the same snapshot (a harmless no-op on identical vitals).
               ctx.world->broadcast_vitals(er.entity_guid);

               // SELF INVENTORY SNAPSHOT AT SPAWN (#453, ITM-01; part of #24).
               // Alongside the self-vitals snapshot above, push the character's
               // backpack contents + money so the client HUD bags window (#452) +
               // the slot-driven vendor SELL control render REAL items the instant
               // the player is in-world (money balance was "unknown" until the first
               // transaction; contents were a copper-only greybox). Server-
               // authoritative snapshot, re-sent after every inventory change below.
               push_inventory_snapshot(sess, ctx);

               // SELF KNOWN-ABILITIES SNAPSHOT AT SPAWN (#457, CMB-01; part of #24).
               // Push the character's KNOWN ability set + per-ability cast/GCD/resource
               // metadata so the action bar (#456) seeds from the REAL learned set (#372)
               // instead of a documented greybox one — and knows each ability's cast_ms
               // + triggers_gcd so it never over-predicts a GCD. At M1 a fresh character
               // knows nothing (empty set) and the set grows on TrainerLearn (re-pushed
               // there). Server-authoritative DISPLAY projection (Principle 1).
               push_known_abilities(sess, ctx);

               // Explore (QST-01, #396): a character that spawns already standing in
               // a discovery volume fires it on enter — credit any explore objective
               // it satisfies (QUEST_PROGRESS follows if it advanced).
               credit_explore_triggers(sess, ctx, er.triggers);

               // GM COMMAND CONTROLS (OPS-02b, #418): register this session so a
               // `.summon`/`.kick` from ANOTHER session can reach it by name. The
               // summon mailbox lives in this ConnCtx (drained on this worker); the
               // .kick teardown closure mirrors the #326 kick-old KickFn — it signals
               // this session's serve loop (the shared `kicked` flag), drops it from
               // the AoI world, and writes Disconnect{KICKED} through its serialized
               // egress (the same cross-thread-safe path the relay uses).
               ctx.forced_move = std::make_shared<ForcedMoveMailbox>();
               {
                   std::shared_ptr<SessionEgress> kick_egress = ctx.egress;
                   std::shared_ptr<std::atomic<bool>> kick_flag = ctx.kicked;
                   WorldState* kick_world = ctx.world;
                   const SessionSlot kick_slot = ctx.slot;
                   const std::string realm = ctx.labels.realm;
                   ctx.world->set_session_control(
                       ctx.slot, ctx.forced_move.get(),
                       [kick_egress, kick_flag, kick_world, kick_slot, realm]() {
                           if (kick_flag) kick_flag->store(true);
                           if (kick_world != nullptr) kick_world->leave(kick_slot);
                           if (kick_egress) {
                               kick_egress->emit_frame(
                                   make_disconnect(net::DisconnectReason::KICKED,
                                                   "kicked by a game master", 0));
                               kick_egress->mark_closed();
                           }
                           metrics::disconnects_total()
                               .with({realm,
                                      disconnect_reason_label(net::DisconnectReason::KICKED)})
                               .inc();
                       });
               }
           }

           // QUEST-KILL credit bus (QST-01 event-bus, #396): register this session's
           // entity guid so creature kills the world thread routes to it are retained
           // until this session drains them (poll_quest_credits). Uses the effective
           // guid the AoI relay assigned (== char guid at M1). The serve loop
           // unregisters on teardown (guarded by credit_guid).
           if (ctx.quest_credit != nullptr) {
               ctx.credit_guid = ctx.movement->entity_guid();
               ctx.credit_token = ctx.quest_credit->register_session(ctx.credit_guid);
           }

           // VITALS egress bus (UI-01 event-bus, #437): register this session's entity
           // guid so a level-up the world thread routes to it is retained until this
           // session drains + broadcasts it (poll_vitals_egress). Same effective guid
           // as the quest bus (`credit_guid`, the AoI-assigned entity guid). The serve
           // loop unregisters on teardown (guarded by credit_guid + vitals_token).
           if (ctx.vitals_egress != nullptr && ctx.movement) {
               if (ctx.credit_guid == 0) ctx.credit_guid = ctx.movement->entity_guid();
               ctx.vitals_token = ctx.vitals_egress->register_session(ctx.credit_guid);
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
               ctx.session_generation = admitted.generation;
               ctx.admitted = true;
               if (admitted.kicked_previous) {
                   log::info(kCat,
                             "single-session takeover: kicked prior in-world session",
                             {log::field("account_id",
                                         static_cast<std::int64_t>(ctx.account_id)),
                              log::field("grant_id", ctx.grant_id)});
               }
           }

           // #784 live-map synchronization: only the world thread may mutate
           // MapTick. Queue this fully-authoritative enter after the session buses
           // are registered, so a first mob hit cannot outrun vitals delivery.
           if (ctx.simulation_enqueue && ctx.movement) {
               UnitStats stats = placeholder_player_stats(pc.class_id);
               stats.level = pc.level;
               WorldEvent ev;
               ev.kind = WorldEventKind::kPlayerEnter;
               ev.player_guid = ctx.movement->entity_guid();
               ev.player_account_id = ctx.account_id;
               ev.player_session_token = ctx.session_token;
               ev.player_session_generation = ctx.session_generation;
               ev.player_pos = ctx.movement->authoritative();
               ev.player_stats = stats;
               ev.char_class = pc.class_id;
               ctx.simulation_enqueue(std::move(ev));
           }

           // ON-SIGHT overhead markers at spawn (#844/#849): enter() already computed
           // this session's initial interest set, so push the `!`/`?` marker for every
           // NPC it can already see — the client shows quest indicators the instant it
           // is in-world, without interacting (the bug #844 fixes). Proactive + diffed.
           push_quest_markers(sess, ctx);
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
            // VITALS_UPDATE (#430, UI-01): an instant resolution moved the TARGET's
            // health and the CASTER's power — broadcast both to every client in AoI
            // so the unit frames update without a re-enter (server-authoritative HUD
            // delta; the client never predicts these, Principle 1).
            ctx.world->broadcast_vitals(target_guid);
            if (caster_guid != target_guid) ctx.world->broadcast_vitals(caster_guid);
            log::debug(kCat, "CAST_REQUEST resolved ability=" + std::to_string(ability_id) +
                                 " outcome=" + std::to_string(static_cast<int>(rr.outcome)) +
                                 " amount=" + std::to_string(rr.amount));
        }
    });

    // --- 0x6xxx CHAT: server-authoritative chat router (SOC-01 #367) -----------
    // ONE client opcode (CHAT_MESSAGE) whose `channel` selects the routing (server
    // SAD §2.5 "spatial chat stays here", §3.8):
    //   • SAY / YELL — spatial, delivered by the AoI grid (#87) to the sessions
    //     within a radius (say = local, yell = wider);
    //   • WHISPER    — routed cross-session to the named target (v0.1 "via the bus
    //     to a world-thread manager" — the in-process registry at M1; #88 over the
    //     real bus at M3);
    //   • ZONE       — the zone/general channel broadcast to all in-world sessions.
    // The server validates sender state (must be spawned) + applies the chat rate
    // class (OPS-03, ctx.chat_intake) and refuses empty/over-length/over-rate
    // sends with a typed ChatRejected. No profanity/content filter at M1.
    on(net::Opcode::CHAT_MESSAGE, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        // A chat frame before the handshake is a protocol error (mirrors CAST /
        // MOVEMENT_INTENT) — ask the serve loop to close.
        if (!ctx.authenticated) {
            log::warn(kCat, "CHAT_MESSAGE before handshake — rejecting");
            ctx.disconnect = true;
            ctx.disconnect_reason = net::DisconnectReason::PROTOCOL_MISMATCH;
            ctx.disconnect_message = "chat before handshake";
            return;
        }

        const auto* msg = fb::GetRoot<mn::ChatMessage>(f.payload);
        if (msg == nullptr) return;  // verified upstream; defensive
        const mn::ChatChannel channel = msg->channel();
        const std::string target = msg->target() ? msg->target()->str() : std::string();
        const std::string text = msg->text() ? msg->text()->str() : std::string();

        auto reject = [&](mn::ChatRejectReason reason) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::CHAT_REJECTED, f.seq,
                                  encode_chat_rejected(channel, reason, target)));
        };

        // GM COMMAND interception (OPS-02a, #417). A chat line beginning with the
        // GM command prefix ('.') is a GM COMMAND, not a chat message — handled
        // here BEFORE the normal say/yell/whisper/zone routing so it never reaches
        // the chat switch. Commands ride the EXISTING CHAT_MESSAGE opcode (no new
        // wire opcode, no world.fbs change); the server parses + permission-gates
        // on the session's gm_level (learnt at WORLD_HELLO) and records EVERY
        // attempt — allowed AND denied — on the append-only GM audit stream. A GM
        // command is legal at character-select too (a GM need not be spawned), so
        // this precedes the in-world requirement below.
        if (gm::is_command(text)) {
            // Rate-gate like any chat line (commands ride the chat opcode): an
            // over-rate command is dropped with the shared drop counter — no
            // parse, no dispatch, no audit.
            if (!ctx.chat_intake.admit(steady_now_ms())) {
                metrics::opcode_dropped_total()
                    .with(ctx.labels.rzs_opcode_reason(
                        opcode_label(static_cast<std::uint16_t>(net::Opcode::CHAT_MESSAGE)),
                        "rate_limit"))
                    .inc();
                reject(mn::ChatRejectReason::RATE_LIMITED);
                return;
            }
            // EFFECT SEAMS (OPS-02b, #418). Wire each GM command's server-side effect
            // to THIS session's authoritative state / the shared world / the char DB.
            // The framework validates args + permission-gates + audits; these lambdas
            // perform the (server-authoritative) mutation. Each captures sess/f/ctx.
            gm::GmEffects fx;

            // .tele — reposition the CALLER (own thread; ctx.movement is this
            // session's, single-threaded here). force_correction arms the ack barrier
            // so the client's reconciling move is not flagged (#420); then relay AoI.
            fx.teleport = [&](const Position& dest) -> gm::EffectStatus {
                if (!ctx.movement || !ctx.entered || ctx.world == nullptr)
                    return gm::EffectStatus::kNotInWorld;
                const std::uint32_t ack = ctx.movement->last_seq() + 1;
                ctx.movement->force_correction(dest, ack);
                const std::uint64_t now = steady_now_ms();
                MoveDecision d;
                d.accepted = true;
                d.ack_seq = ack;
                d.state_flags = ctx.movement->last_flags();
                d.pos = dest;
                send_s2c(sess, ctx,
                         encode_frame(net::Opcode::MOVEMENT_STATE, f.seq,
                                      encode_movement_state(ctx.movement->entity_guid(),
                                                            d, now)));
                ctx.world->on_movement(ctx.slot, dest, ack, d.state_flags, now);
                if (ctx.simulation_enqueue) {
                    WorldEvent ev;
                    ev.kind = WorldEventKind::kPlayerMove;
                    ev.player_guid = ctx.movement->entity_guid();
                    ev.player_account_id = ctx.account_id;
                    ev.player_session_token = ctx.session_token;
                    ev.player_session_generation = ctx.session_generation;
                    ev.player_pos = dest;
                    ctx.simulation_enqueue(std::move(ev));
                }
                return gm::EffectStatus::kApplied;
            };

            // .summon — bring a named player to the CALLER's position. WorldState
            // moves the target (grid + AoI) under its lock and posts the destination
            // to the target's mailbox; the target arms its ack barrier + snaps its own
            // client on its worker (drain_forced_move). The caller must be in-world.
            fx.summon = [&](const std::string& name) -> gm::EffectStatus {
                if (!ctx.movement || !ctx.entered || ctx.world == nullptr)
                    return gm::EffectStatus::kNotInWorld;
                const Position dest = ctx.movement->authoritative();
                const WorldState::TargetOutcome oc = ctx.world->summon_to(
                    name, dest, /*ack_seq=*/0, ctx.movement->last_flags(), steady_now_ms());
                return oc == WorldState::TargetOutcome::kApplied
                           ? gm::EffectStatus::kApplied
                           : gm::EffectStatus::kTargetOffline;
            };

            // .additem — mint + DB-persist an item into the caller's inventory (reuses
            // the loot/quest mint+place path). Server checks the template exists +
            // there is room; nothing beyond the args is trusted.
            fx.add_item = [&](std::uint32_t template_id,
                              std::uint32_t count) -> gm::AddItemResult {
                gm::AddItemResult r;
                if (ctx.char_db == nullptr || ctx.char_id == 0) {
                    r.status = gm::EffectStatus::kNotInWorld;
                    return r;
                }
                const itm::ItemTemplate* tmpl = item_templates().find(template_id);
                if (tmpl == nullptr) {
                    r.status = gm::EffectStatus::kUnknownItem;
                    return r;
                }
                std::uint32_t stack = count;
                if (tmpl->max_stack > 0 && stack > tmpl->max_stack) stack = tmpl->max_stack;
                try {
                    itm::Inventory place =
                        itm::load_inventory(*ctx.char_db, ctx.char_id, item_templates());
                    const std::optional<std::uint16_t> slot = first_free_slot(place);
                    if (!slot) {
                        r.status = gm::EffectStatus::kNoSpace;
                        return r;
                    }
                    const itm::ItemInstance minted =
                        itm::mint_instance(*ctx.char_db, template_id, stack);
                    itm::place_item(*ctx.char_db, ctx.char_id, /*bag=*/0,
                                    itm::backpack_placement_slot(*slot), minted.item_guid);
                    r.status = gm::EffectStatus::kApplied;
                    r.item_name = tmpl->name;
                    r.count = stack;
                    push_inventory_snapshot(sess, ctx);  // #453: bags gained the item
                } catch (const std::exception& e) {
                    log::warn(kCat, "GM .additem persist failed",
                              {log::field("error", e.what())});
                    r.status = gm::EffectStatus::kInternalError;
                }
                return r;
            };

            // .setlevel — set the caller's server-authoritative gate level (+ the
            // world-owned Unit level for combat scaling). Already clamped by the
            // handler; requires a spawned character.
            fx.set_level = [&](std::uint32_t level) -> gm::SetLevelResult {
                gm::SetLevelResult r;
                if (ctx.phase != SessionPhase::kInWorld) {
                    r.status = gm::EffectStatus::kNotInWorld;
                    return r;
                }
                const std::uint16_t lvl = static_cast<std::uint16_t>(level);
                ctx.char_level = lvl;
                if (ctx.entered && ctx.world != nullptr) {
                    ctx.world->set_unit_level(ctx.slot, lvl);
                    // VITALS_UPDATE (#430): push the new level to the HUD (self + AoI
                    // observers), so the unit frames reflect the GM level change.
                    if (ctx.movement) ctx.world->broadcast_vitals(ctx.movement->entity_guid());
                }
                r.status = gm::EffectStatus::kApplied;
                r.applied_level = lvl;
                return r;
            };

            // .kick — disconnect a named player's session (cross-thread teardown via
            // the target's registered closure). No caller in-world requirement.
            fx.kick = [&](const std::string& name) -> gm::EffectStatus {
                if (ctx.world == nullptr) return gm::EffectStatus::kTargetOffline;
                const WorldState::TargetOutcome oc = ctx.world->disconnect_by_name(name);
                return oc == WorldState::TargetOutcome::kApplied
                           ? gm::EffectStatus::kApplied
                           : gm::EffectStatus::kTargetOffline;
            };

            // .ban (OPS-02c, #419) — resolve the subject + write the durable ban
            // (bans lib, auth DB) attributed to the issuing GM, then audit the
            // issuance. account/char names are resolved to ids server-side (the auth
            // DB for accounts, the char DB for characters); an unknown name is
            // kTargetOffline. Bans live in the auth DB, so ctx.db is required.
            fx.ban = [&](gm::BanSubject subject, const std::string& target,
                         std::optional<std::uint64_t> dur,
                         const std::string& reason) -> gm::BanResult {
                gm::BanResult r;
                if (ctx.db == nullptr) { r.status = gm::EffectStatus::kUnavailable; return r; }
                std::string audit_target;
                try {
                    switch (subject) {
                        case gm::BanSubject::kAccount: {
                            const std::optional<std::uint64_t> id =
                                bans::account_id_for(*ctx.db, target);
                            if (!id) { r.status = gm::EffectStatus::kTargetOffline; return r; }
                            bans::ban_account(*ctx.db, *id, reason, ctx.account_id, dur);
                            audit_target = "account:" + std::to_string(*id);
                            r.subject_desc = "account '" + target + "'";
                            break;
                        }
                        case gm::BanSubject::kCharacter: {
                            if (ctx.char_db == nullptr) {
                                r.status = gm::EffectStatus::kUnavailable;
                                return r;
                            }
                            const std::optional<std::uint64_t> id =
                                bans::character_id_for(*ctx.char_db, target);
                            if (!id) { r.status = gm::EffectStatus::kTargetOffline; return r; }
                            bans::ban_character(*ctx.db, *id, reason, ctx.account_id, dur);
                            audit_target = "character:" + std::to_string(*id);
                            r.subject_desc = "character '" + target + "'";
                            break;
                        }
                        case gm::BanSubject::kIp: {
                            bans::ban_ip(*ctx.db, target, reason, ctx.account_id, dur);
                            audit_target = "ip:" + target;
                            r.subject_desc = "IP " + target;
                            break;
                        }
                    }
                } catch (const std::exception& e) {
                    log::warn(kCat, "GM .ban persist failed", {log::field("error", e.what())});
                    r.status = gm::EffectStatus::kInternalError;
                    return r;
                }
                audit::emit(audit::Record{
                    .action = audit::Action::kBanIssued,
                    .outcome = audit::Outcome::kSuccess,
                    .account_id = ctx.account_id,
                    .target = audit_target,
                    .reason = dur ? "temporary" : "permanent",
                });
                r.status = gm::EffectStatus::kApplied;
                return r;
            };

            // .mute (OPS-02c, #419) — resolve the character name → id (char DB) +
            // write the durable mute (char DB) attributed to the issuing GM, then
            // audit. An unknown name is kTargetOffline. Requires the characters DB.
            fx.mute = [&](const std::string& name, std::optional<std::uint64_t> dur,
                          const std::string& reason) -> gm::MuteResult {
                gm::MuteResult r;
                if (ctx.char_db == nullptr) { r.status = gm::EffectStatus::kUnavailable; return r; }
                try {
                    const std::optional<std::uint64_t> id =
                        bans::character_id_for(*ctx.char_db, name);
                    if (!id) { r.status = gm::EffectStatus::kTargetOffline; return r; }
                    bans::mute_character(*ctx.char_db, *id, reason, ctx.account_id, dur);
                    audit::emit(audit::Record{
                        .action = audit::Action::kMuteIssued,
                        .outcome = audit::Outcome::kSuccess,
                        .account_id = ctx.account_id,
                        .target = "character:" + std::to_string(*id),
                        .reason = dur ? "temporary" : "permanent",
                    });
                    r.status = gm::EffectStatus::kApplied;
                    r.subject_desc = name;
                } catch (const std::exception& e) {
                    log::warn(kCat, "GM .mute persist failed", {log::field("error", e.what())});
                    r.status = gm::EffectStatus::kInternalError;
                }
                return r;
            };

            gm::dispatch_command(
                gm::Registry::builtin(), text, ctx.account_id, ctx.gm_level, fx,
                // Reply sink: a server SYSTEM line back to the SENDER only. Rides
                // the existing CHAT_DELIVER opcode (no new wire opcode) with a 0
                // sender_guid + "System" sender_name so the client renders it as a
                // system message.
                [&](const std::string& line) {
                    send_s2c(sess, ctx,
                             encode_frame(net::Opcode::CHAT_DELIVER, f.seq,
                                          encode_chat_deliver_payload(
                                              mn::ChatChannel::WHISPER,
                                              /*sender_guid=*/0, "System", line)));
                },
                // Audit sink: the append-only GM audit stream (OPS-05 #92).
                [](const audit::Record& rec) { audit::emit(rec); });
            return;
        }

        // Must be SPAWNED in-world (char-select chat is a client bug, not hostile —
        // REJECT, do not disconnect; mirrors the CAST_REQUEST not-in-world path).
        if (ctx.phase != SessionPhase::kInWorld || ctx.world == nullptr || !ctx.entered) {
            reject(mn::ChatRejectReason::NOT_IN_WORLD);
            return;
        }

        // OPS-02c (#419): DROP a MUTED character's chat before it routes anywhere.
        // The mute is character-scoped + durable (characters DB), so it is queried
        // fresh here — a mute issued by any GM applies to the next line immediately,
        // and an elapsed mute (expires_at past) stops matching (the query evaluates
        // expiry in SQL). world.fbs is unchanged: the "you are muted" notice rides
        // the existing CHAT_DELIVER opcode as a System line (like a GM-command reply),
        // not a new reject reason. A mute-lookup DB fault fails OPEN (a transient
        // hiccup must not silence a legitimate player; the table is also absent on the
        // DB-less dispatch smoke path, where char_db is null and this is skipped).
        if (ctx.char_db != nullptr && ctx.char_id != 0) {
            std::optional<bans::Active> mute;
            try {
                mute = bans::character_mute(*ctx.char_db, ctx.char_id);
            } catch (const db::DbError& e) {
                log::warn(kCat, "chat mute lookup failed (fail-open)",
                          {log::field("error", e.what())});
            }
            if (mute) {
                audit::emit(audit::Record{
                    .action = audit::Action::kChatMuted,
                    .outcome = audit::Outcome::kFailure,
                    .account_id = ctx.account_id,
                    .target = "character:" + std::to_string(ctx.char_id),
                    .reason = "muted",
                    .correlation_id = ctx.grant_id,
                });
                const std::string notice =
                    mute->permanent
                        ? std::string("You are muted and cannot chat.")
                        : std::string("You are muted until ") + mute->expires_at +
                              " UTC and cannot chat.";
                send_s2c(sess, ctx,
                         encode_frame(net::Opcode::CHAT_DELIVER, f.seq,
                                      encode_chat_deliver_payload(
                                          mn::ChatChannel::WHISPER,
                                          /*sender_guid=*/0, "System", notice)));
                return;
            }
        }

        // Rate class (OPS-03): drop an over-rate send with a typed reject + the
        // Errors-dashboard drop counter (mirrors MOVEMENT_INTENT's rate drop).
        if (!ctx.chat_intake.admit(steady_now_ms())) {
            metrics::opcode_dropped_total()
                .with(ctx.labels.rzs_opcode_reason(
                    opcode_label(static_cast<std::uint16_t>(net::Opcode::CHAT_MESSAGE)),
                    "rate_limit"))
                .inc();
            reject(mn::ChatRejectReason::RATE_LIMITED);
            log::debug(kCat, "CHAT_MESSAGE dropped (rate class)");
            return;
        }

        // Shape checks (NOT a content filter — none at M1): reject a blank or an
        // over-length body. Server is authoritative — no silent truncation.
        if (is_blank(text)) {
            reject(mn::ChatRejectReason::EMPTY);
            return;
        }
        if (text.size() > kChatMaxTextBytes) {
            reject(mn::ChatRejectReason::TOO_LONG);
            return;
        }

        switch (channel) {
            case mn::ChatChannel::SAY:
            case mn::ChatChannel::YELL:
                // Spatial: delivered by the #87 grid to the sessions in range.
                ctx.world->deliver_spatial(ctx.slot, channel, text);
                break;
            case mn::ChatChannel::WHISPER: {
                // Directed cross-session by target name.
                const ChatWhisperOutcome outcome =
                    ctx.world->whisper(ctx.slot, target, text);
                if (outcome == ChatWhisperOutcome::kNoTarget) {
                    reject(mn::ChatRejectReason::NO_TARGET);
                } else if (outcome == ChatWhisperOutcome::kTargetOffline) {
                    reject(mn::ChatRejectReason::TARGET_OFFLINE);
                }
                // kDelivered: the target got it; the sender's client local-echoes
                // "To X: …" (no S→C ack needed at M1).
                break;
            }
            case mn::ChatChannel::ZONE:
                // Channel: broadcast to every in-world session on this shard.
                ctx.world->deliver_channel(ctx.slot, text);
                break;
            default:
                // An out-of-range channel enum — malformed input. Drop + log; the
                // wire verifier accepted the table but not the enum domain.
                log::warn(kCat, "CHAT_MESSAGE unknown channel=" +
                                    std::to_string(static_cast<int>(channel)) + " — dropped");
                break;
        }
    });

    // --- 0x50xx EQUIPMENT: authoritative paperdoll mutations (#802) ------------
    // The client names only one of its backpack/paperdoll slots. The server reloads
    // ownership, validates class/level/hand rules, and commits the placement swap in
    // one transaction. Every typed result is followed by a complete authoritative
    // inventory snapshot, including rejects, so a stale UI always reconciles.
    on(net::Opcode::EQUIPMENT_CHANGE_REQUEST,
       [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "EQUIPMENT_CHANGE_REQUEST")) return;
        const auto* req = fb::GetRoot<mn::EquipmentChangeRequest>(f.payload);
        if (req == nullptr) return;
        const mn::EquipmentChangeAction action = req->action();
        const std::uint16_t slot = req->slot();

        auto reject = [&](mn::EquipmentChangeStatus status) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::EQUIPMENT_CHANGE_RESULT, f.seq,
                                  encode_equipment_change_result(status, action, slot,
                                                                 255)));
            push_inventory_snapshot(sess, ctx);
        };

        if (ctx.phase != SessionPhase::kInWorld) {
            reject(mn::EquipmentChangeStatus::NOT_IN_WORLD);
            return;
        }
        if (ctx.char_db == nullptr || ctx.char_id == 0 ||
            ctx.class_catalog == nullptr || ctx.equip_type_catalog == nullptr) {
            reject(mn::EquipmentChangeStatus::INTERNAL);
            return;
        }
        const ClassRecord* cls = ctx.class_catalog->find(ctx.char_class);
        if (cls == nullptr) {
            reject(mn::EquipmentChangeStatus::UNKNOWN_CLASS);
            return;
        }

        try {
            // Read the unchanged money balance before mutation. Once COMMIT succeeds,
            // reconciliation uses the transaction's own post-mutation Inventory so a
            // transient follow-up read cannot strand this live session in stale state.
            const std::int64_t money =
                static_cast<std::int64_t>(itm::get_money(*ctx.char_db, ctx.char_id));
            const EquipmentMutationResult changed = [&]() -> EquipmentMutationResult {
                if (action == mn::EquipmentChangeAction::EQUIP) {
                    return equip_owned_item(*ctx.char_db, ctx.char_id, slot, ctx.char_level,
                                            *cls, *ctx.equip_type_catalog,
                                            item_templates());
                }
                if (action == mn::EquipmentChangeAction::UNEQUIP) {
                    if (slot >= itm::kEquipSlotCount)
                        throw itm::InvalidSlot("paperdoll slot");
                    return unequip_owned_item(*ctx.char_db, ctx.char_id,
                                              static_cast<itm::EquipSlot>(slot),
                                              item_templates());
                }
                throw itm::InvalidSlot("equipment action");
            }();

            const auto wire_slot = static_cast<std::uint8_t>(changed.equipped_slot);
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::EQUIPMENT_CHANGE_RESULT, f.seq,
                                  encode_equipment_change_result(
                                      mn::EquipmentChangeStatus::OK, action, slot,
                                      wire_slot)));
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::INVENTORY_SNAPSHOT, 0,
                                  encode_inventory_snapshot(money, changed.inventory)));

            // A successful mutation replaces the visible equipment set for self and
            // all current observers. WorldState also stores it for future EntityEnter.
            if (ctx.world != nullptr && ctx.credit_guid != 0) {
                ctx.world->update_equipment_visuals(
                    ctx.credit_guid, visible_equipment_visuals(changed.inventory));
            }
        } catch (const itm::InvalidSlot&) {
            reject(mn::EquipmentChangeStatus::INVALID_SLOT);
        } catch (const itm::SlotEmpty&) {
            reject(mn::EquipmentChangeStatus::SLOT_EMPTY);
        } catch (const itm::NotEquippable&) {
            reject(mn::EquipmentChangeStatus::NOT_EQUIPPABLE);
        } catch (const itm::LevelTooLow&) {
            reject(mn::EquipmentChangeStatus::LEVEL_TOO_LOW);
        } catch (const EquipGateRejected& e) {
            switch (e.gate()) {
                case EquipGate::kNotProficient:
                    reject(mn::EquipmentChangeStatus::NOT_PROFICIENT); break;
                case EquipGate::kCategoryMismatch:
                    reject(mn::EquipmentChangeStatus::CATEGORY_MISMATCH); break;
                case EquipGate::kUnknownEquipType:
                    reject(mn::EquipmentChangeStatus::UNKNOWN_EQUIP_TYPE); break;
                case EquipGate::kAllowed:
                    reject(mn::EquipmentChangeStatus::INTERNAL); break;
            }
        } catch (const itm::TwoHandNeedsOffHandEmpty&) {
            reject(mn::EquipmentChangeStatus::TWO_HAND_CONFLICT);
        } catch (const itm::OffHandBlockedByTwoHand&) {
            reject(mn::EquipmentChangeStatus::TWO_HAND_CONFLICT);
        } catch (const itm::InventoryFull&) {
            reject(mn::EquipmentChangeStatus::INVENTORY_FULL);
        } catch (const std::exception& e) {
            log::warn(kCat, "EQUIPMENT_CHANGE_REQUEST failed",
                      {log::field("error", e.what())});
            reject(mn::EquipmentChangeStatus::INTERNAL);
        }
    });

    // --- 0x51xx VENDOR: server-authoritative buy / sell / buyback (ECO-01 #370) -
    // Each request is validated + applied entirely on the server (does the vendor
    // sell it / does the player own it / can they afford it / is there space)
    // against the placeholder vendor catalog + item templates, moving int64 copper
    // and item instances durably through meridian-vendor's DB path. A request NEVER
    // carries a price. Prerequisites mirror the other in-world handlers:
    //   * pre-handshake  -> protocol error (Disconnect), like CHAT/CAST;
    //   * authenticated but not spawned -> typed NOT_IN_WORLD reject (no close);
    //   * no characters DB / no char_id -> INTERNAL (cannot touch durable state).
    // A rejected transaction changes nothing; the reply carries the unchanged
    // balance (best-effort) so the client can reconcile.

    on(net::Opcode::VENDOR_BUY_REQUEST, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "VENDOR_BUY_REQUEST")) return;
        const auto* req = fb::GetRoot<mn::VendorBuyRequest>(f.payload);
        if (req == nullptr) return;  // verified upstream; defensive
        const std::uint32_t vendor_id = req->vendor_id();
        const std::uint32_t template_id = req->item_template_id();
        const std::uint32_t quantity = req->quantity();

        auto reply = [&](mn::VendorBuyStatus st, std::uint64_t guid,
                         std::int64_t total, std::int64_t balance) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::VENDOR_BUY_RESULT, f.seq,
                                  encode_vendor_buy_result(st, vendor_id, template_id,
                                                           quantity, guid, total, balance)));
        };

        if (ctx.phase != SessionPhase::kInWorld) { reply(mn::VendorBuyStatus::NOT_IN_WORLD, 0, 0, 0); return; }
        if (ctx.char_db == nullptr || ctx.char_id == 0) { reply(mn::VendorBuyStatus::INTERNAL, 0, 0, 0); return; }

        // OPS-03b (#421): defense-in-depth economy sanity — reject an impossible
        // client-supplied quantity (zero or absurdly large) at the wire boundary,
        // BEFORE any catalog lookup / price arithmetic (which could overflow on a
        // 32-bit quantity). Audited on the economy stream; the wire reply is the
        // feature's existing BAD_QUANTITY (no new client oracle). The per-template
        // max_stack check inside buy_db remains the finer, authoritative gate.
        if (const EconomyReject er = check_quantity(quantity); er != EconomyReject::kNone) {
            audit::emit(build_economy_reject_audit(ctx.account_id, ctx.grant_id, ctx.char_id,
                                                   "vendor_buy", er));
            reply(mn::VendorBuyStatus::BAD_QUANTITY, 0, 0, safe_balance(ctx));
            return;
        }

        try {
            vend::BuyDbResult r = vend::buy_db(*ctx.char_db, ctx.char_id, vendor_catalog(),
                                               item_templates(), vendor_id, template_id, quantity);
            reply(mn::VendorBuyStatus::OK, r.item_guid,
                  static_cast<std::int64_t>(r.total_price),
                  static_cast<std::int64_t>(r.balance_after));
            push_inventory_snapshot(sess, ctx);  // #453: bags + money changed
        } catch (const vend::UnknownVendor&) {
            reply(mn::VendorBuyStatus::UNKNOWN_VENDOR, 0, 0, safe_balance(ctx));
        } catch (const vend::ItemNotSold&) {
            reply(mn::VendorBuyStatus::NOT_SOLD, 0, 0, safe_balance(ctx));
        } catch (const itm::UnknownTemplate&) {
            reply(mn::VendorBuyStatus::NOT_SOLD, 0, 0, safe_balance(ctx));
        } catch (const itm::BadStackCount&) {
            reply(mn::VendorBuyStatus::BAD_QUANTITY, 0, 0, safe_balance(ctx));
        } catch (const itm::InsufficientFunds&) {
            reply(mn::VendorBuyStatus::INSUFFICIENT_FUNDS, 0, 0, safe_balance(ctx));
        } catch (const itm::InventoryFull&) {
            reply(mn::VendorBuyStatus::INVENTORY_FULL, 0, 0, safe_balance(ctx));
        } catch (const std::exception& e) {
            log::warn(kCat, "VENDOR_BUY_REQUEST failed", {log::field("error", e.what())});
            reply(mn::VendorBuyStatus::INTERNAL, 0, 0, safe_balance(ctx));
        }
    });

    on(net::Opcode::VENDOR_SELL_REQUEST, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "VENDOR_SELL_REQUEST")) return;
        const auto* req = fb::GetRoot<mn::VendorSellRequest>(f.payload);
        if (req == nullptr) return;
        const std::uint16_t backpack_slot = req->backpack_slot();
        const std::uint32_t quantity = req->quantity();

        auto reply = [&](mn::VendorSellStatus st, std::uint32_t template_id,
                         std::uint32_t qty, std::int64_t credit, std::int64_t balance,
                         std::uint16_t buyback_slot) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::VENDOR_SELL_RESULT, f.seq,
                                  encode_vendor_sell_result(st, backpack_slot, template_id,
                                                            qty, credit, balance, buyback_slot)));
        };

        if (ctx.phase != SessionPhase::kInWorld) { reply(mn::VendorSellStatus::NOT_IN_WORLD, 0, 0, 0, 0, 0); return; }
        if (ctx.char_db == nullptr || ctx.char_id == 0) { reply(mn::VendorSellStatus::INTERNAL, 0, 0, 0, 0, 0); return; }

        // OPS-03b (#421): defense-in-depth economy sanity — reject an impossible
        // client-supplied sell quantity before touching durable state. Audited;
        // wire reply is the existing BAD_QUANTITY. sell_db's stack-clamp + credit
        // overflow guard remain the finer authoritative checks.
        if (const EconomyReject er = check_quantity(quantity); er != EconomyReject::kNone) {
            audit::emit(build_economy_reject_audit(ctx.account_id, ctx.grant_id, ctx.char_id,
                                                   "vendor_sell", er));
            reply(mn::VendorSellStatus::BAD_QUANTITY, 0, 0, 0, safe_balance(ctx), 0);
            return;
        }

        try {
            vend::SellDbResult r = vend::sell_db(*ctx.char_db, ctx.char_id, ctx.buyback,
                                                 item_templates(), backpack_slot, quantity);
            reply(mn::VendorSellStatus::OK, r.item_template_id, r.quantity,
                  static_cast<std::int64_t>(r.total_credit),
                  static_cast<std::int64_t>(r.balance_after),
                  static_cast<std::uint16_t>(r.buyback_slot));
            push_inventory_snapshot(sess, ctx);  // #453: bags + money changed
        } catch (const itm::SlotEmpty&) {
            reply(mn::VendorSellStatus::SLOT_EMPTY, 0, 0, 0, safe_balance(ctx), 0);
        } catch (const vend::NotSellable&) {
            reply(mn::VendorSellStatus::NOT_SELLABLE, 0, 0, 0, safe_balance(ctx), 0);
        } catch (const itm::BadStackCount&) {
            reply(mn::VendorSellStatus::BAD_QUANTITY, 0, 0, 0, safe_balance(ctx), 0);
        } catch (const std::exception& e) {
            log::warn(kCat, "VENDOR_SELL_REQUEST failed", {log::field("error", e.what())});
            reply(mn::VendorSellStatus::INTERNAL, 0, 0, 0, safe_balance(ctx), 0);
        }
    });

    on(net::Opcode::VENDOR_BUYBACK_REQUEST, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "VENDOR_BUYBACK_REQUEST")) return;
        const auto* req = fb::GetRoot<mn::VendorBuybackRequest>(f.payload);
        if (req == nullptr) return;
        const std::uint16_t buyback_slot = req->buyback_slot();

        // buyback_slot ECHOES the request's slot (#453) so the client can drop that
        // buyback row without correlating against its own request.
        auto reply = [&](mn::VendorBuybackStatus st, std::uint32_t template_id,
                         std::uint32_t qty, std::uint64_t guid, std::int64_t price,
                         std::int64_t balance) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::VENDOR_BUYBACK_RESULT, f.seq,
                                  encode_vendor_buyback_result(st, template_id, qty, guid,
                                                               price, balance, buyback_slot)));
        };

        if (ctx.phase != SessionPhase::kInWorld) { reply(mn::VendorBuybackStatus::NOT_IN_WORLD, 0, 0, 0, 0, 0); return; }
        if (ctx.char_db == nullptr || ctx.char_id == 0) { reply(mn::VendorBuybackStatus::INTERNAL, 0, 0, 0, 0, 0); return; }

        try {
            vend::BuybackDbResult r = vend::buyback_db(*ctx.char_db, ctx.char_id, ctx.buyback,
                                                       item_templates(), buyback_slot);
            reply(mn::VendorBuybackStatus::OK, r.item_template_id, r.quantity, r.item_guid,
                  static_cast<std::int64_t>(r.price),
                  static_cast<std::int64_t>(r.balance_after));
            push_inventory_snapshot(sess, ctx);  // #453: bags + money changed
        } catch (const vend::BuybackSlotEmpty&) {
            reply(mn::VendorBuybackStatus::EMPTY_SLOT, 0, 0, 0, 0, safe_balance(ctx));
        } catch (const itm::InsufficientFunds&) {
            reply(mn::VendorBuybackStatus::INSUFFICIENT_FUNDS, 0, 0, 0, 0, safe_balance(ctx));
        } catch (const itm::InventoryFull&) {
            reply(mn::VendorBuybackStatus::INVENTORY_FULL, 0, 0, 0, 0, safe_balance(ctx));
        } catch (const std::exception& e) {
            log::warn(kCat, "VENDOR_BUYBACK_REQUEST failed", {log::field("error", e.what())});
            reply(mn::VendorBuybackStatus::INTERNAL, 0, 0, 0, 0, safe_balance(ctx));
        }
    });

    // --- 0x4xxx QUEST: accept / turn-in / log (QST-01 #371) --------------------
    // The session's quest state machine (ctx.quests) is emplaced at ENTER_WORLD over
    // the shared placeholder quest store. Accept gates on the character's server-
    // authoritative level; turn-in grants durable rewards (reward items minted +
    // reward copper credited to char_db). Progress + log snapshots are pushed S→C.
    // Prerequisites mirror the other in-world handlers: pre-handshake -> protocol
    // close (require_authenticated); authenticated-but-not-spawned -> a typed reject
    // (the quest enums carry no NOT_IN_WORLD, so the generic "no such quest / not on
    // the quest" reject is used out-of-phase — no character context exists yet).

    on(net::Opcode::QUEST_ACCEPT, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "QUEST_ACCEPT")) return;
        const auto* req = fb::GetRoot<mn::QuestAccept>(f.payload);
        if (req == nullptr) return;  // verified upstream; defensive
        const QuestId quest_id = req->quest_id();
        const std::uint64_t giver_guid = req->giver_guid();

        auto reply = [&](mn::QuestAcceptStatus st) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::QUEST_ACCEPT_RESULT, f.seq,
                                  encode_quest_accept_result(quest_id, st)));
        };

        if (ctx.phase != SessionPhase::kInWorld || !ctx.quests) {
            reply(mn::QuestAcceptStatus::UNKNOWN_QUEST);  // out-of-phase: no character
            return;
        }

        const QuestDef* def = quest_store().find(quest_id);
        if (def == nullptr) { reply(mn::QuestAcceptStatus::UNKNOWN_QUEST); return; }
        // WRONG_GIVER (wire-only, dispatch-added). giver_guid == 0 is "unspecified /
        // self-serve" and skips the check. A SPAWNED giver is addressed by its world-
        // entity wire guid, so resolve it to its npc_template id first (#838) — the same
        // mapping GOSSIP_HELLO uses; a raw template id (pre-spawn) resolves to itself.
        if (giver_guid != 0 &&
            resolve_npc_template_id(ctx, giver_guid) != def->giver_npc_id) {
            reply(mn::QuestAcceptStatus::WRONG_GIVER);
            return;
        }

        const AcceptStatus st = ctx.quests->accept(quest_id, ctx.char_level);
        reply(to_wire(st));
        if (st != AcceptStatus::kOk) return;

        // The deliver item of a deliver objective is granted on accept (quest.schema
        // note) — mint it durably so the deliver flow is real. Best-effort: a DB fault
        // does not undo the (in-memory) accept; it is logged.
        if (ctx.char_db != nullptr && ctx.char_id != 0) {
            try {
                itm::Inventory place =
                    itm::load_inventory(*ctx.char_db, ctx.char_id, item_templates());
                for (const QuestObjective& o : def->objectives) {
                    if (o.type != ObjectiveType::kDeliver) continue;
                    std::optional<std::uint16_t> slot = first_free_slot(place);
                    if (!slot) break;
                    itm::ItemInstance minted = itm::mint_instance(*ctx.char_db, o.item_id, 1);
                    itm::place_item(*ctx.char_db, ctx.char_id, /*bag=*/0,
                                    itm::backpack_placement_slot(*slot), minted.item_guid);
                    place.add(minted);
                }
            } catch (const std::exception& e) {
                log::warn(kCat, "QUEST_ACCEPT deliver-item grant failed",
                          {log::field("error", e.what())});
            }
        }

        // Resync the client's quest log (a QUEST_PROGRESS/QuestLog follows an accept).
        send_s2c(sess, ctx, encode_frame(net::Opcode::QUEST_LOG, f.seq,
                                         encode_quest_log(*ctx.quests)));
        // Accepting flips the giver's marker `!` -> greyed `?` (now on the quest) and
        // may light a turn-in NPC — re-push the overhead markers (#844/#849).
        push_quest_markers(sess, ctx);
    });

    on(net::Opcode::QUEST_TURN_IN, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "QUEST_TURN_IN")) return;
        const auto* req = fb::GetRoot<mn::QuestTurnIn>(f.payload);
        if (req == nullptr) return;
        const QuestId quest_id = req->quest_id();
        // Resolve a SPAWNED turn-in NPC's world-entity guid to its npc_template id, the
        // same way QUEST_ACCEPT / GOSSIP_HELLO do (#838) — else a spawned turn-in giver
        // is mis-identified and the turn-in is wrongly rejected.
        const std::uint32_t npc_id = resolve_npc_template_id(ctx, req->turn_in_guid());
        const int choice_index = req->choice_index();

        auto reply = [&](mn::QuestTurnInStatus st, std::uint32_t xp, std::int64_t money,
                         const std::vector<QuestRewardItem>& items) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::QUEST_TURN_IN_RESULT, f.seq,
                                  encode_quest_turn_in_result(quest_id, st, xp, money, items,
                                                              ctx.char_level)));
        };

        if (ctx.phase != SessionPhase::kInWorld || !ctx.quests) {
            reply(mn::QuestTurnInStatus::NOT_ACTIVE, 0, 0, {});  // out-of-phase
            return;
        }
        if (ctx.char_db == nullptr || ctx.char_id == 0) {  // defensive (unreachable in-world)
            reply(mn::QuestTurnInStatus::UNKNOWN_QUEST, 0, 0, {});
            return;
        }

        try {
            itm::Inventory inv =
                itm::load_inventory(*ctx.char_db, ctx.char_id, item_templates());
            RewardGrant grant;
            const TurnInStatus st =
                ctx.quests->turn_in(quest_id, npc_id, inv, choice_index, grant);
            if (st != TurnInStatus::kOk) {
                reply(to_wire(st), 0, 0, {});
                return;
            }

            // Persist the CONSUMED objective items durably FIRST (collect items handed
            // over + the deliver item): destroy or decrement the matching backpack rows
            // so the reward mint below sees the freed slots. turn_in's in-memory `inv`
            // already reflects the removal; mirror it in the characters DB. Best-effort
            // per item (turn_in reported only what was actually held).
            {
                itm::Inventory held =
                    itm::load_inventory(*ctx.char_db, ctx.char_id, item_templates());
                for (const QuestRewardItem& cx : grant.consumed) {
                    std::uint32_t remaining = cx.count;
                    for (std::uint16_t i = 0;
                         i < held.backpack_capacity() && remaining > 0; ++i) {
                        const itm::ItemInstance* inst = held.backpack_at(i);
                        if (inst == nullptr || inst->template_id != cx.item_id) continue;
                        if (inst->stack <= remaining) {
                            itm::destroy_instance(*ctx.char_db, inst->item_guid);
                            remaining -= inst->stack;
                        } else {
                            itm::set_instance_stack(*ctx.char_db, inst->item_guid,
                                                    inst->stack - remaining);
                            remaining = 0;
                        }
                    }
                }
            }

            // Persist the reward bundle durably: mint + place each reward stack at a
            // free backpack slot (turn_in verified room after consumption), then credit
            // reward copper. Reload so the free-slot search reflects the consumption.
            itm::Inventory place =
                itm::load_inventory(*ctx.char_db, ctx.char_id, item_templates());
            for (const QuestRewardItem& ri : grant.items) {
                std::optional<std::uint16_t> slot = first_free_slot(place);
                if (!slot) break;  // defensive: turn_in reserved room
                itm::ItemInstance minted =
                    itm::mint_instance(*ctx.char_db, ri.item_id, ri.count);
                itm::place_item(*ctx.char_db, ctx.char_id, /*bag=*/0,
                                itm::backpack_placement_slot(*slot), minted.item_guid);
                place.add(minted);
            }
            // OPS-03b (#421): defense-in-depth — a reward-money credit is server-
            // derived (quest content), but guard it anyway: a negative reward from a
            // corrupt content row is an impossible delta — audit + skip the credit
            // rather than silently dropping it. A non-negative amount credits through
            // add_money, whose checked_add is the authoritative overflow guard.
            if (const EconomyReject er = check_amount_nonnegative(grant.money);
                er != EconomyReject::kNone) {
                audit::emit(build_economy_reject_audit(ctx.account_id, ctx.grant_id,
                                                       ctx.char_id, "quest_reward", er));
            } else if (grant.money > 0) {
                itm::add_money(*ctx.char_db, ctx.char_id, grant.money);
            }

            // new_level is reported as the current level: reward XP is not persisted at
            // M1 (durable XP/level is CHR-03/mcc #28) — the amount is still surfaced.
            reply(mn::QuestTurnInStatus::OK, grant.xp, static_cast<std::int64_t>(grant.money),
                  grant.items);
            // #453: the reward consumed objective items + minted reward items +
            // credited reward copper — re-snapshot the bags window's real state.
            push_inventory_snapshot(sess, ctx);
            send_s2c(sess, ctx, encode_frame(net::Opcode::QUEST_LOG, f.seq,
                                             encode_quest_log(*ctx.quests)));
            // Turning in clears this NPC's lit `?` (quest done) — re-push the overhead
            // markers so the icon drops (#844/#849).
            push_quest_markers(sess, ctx);
        } catch (const std::exception& e) {
            log::warn(kCat, "QUEST_TURN_IN failed", {log::field("error", e.what())});
            reply(mn::QuestTurnInStatus::UNKNOWN_QUEST, 0, 0, {});  // defensive (no INTERNAL enum)
        }
    });

    // QUEST_LOG (C→S resync request): the client asks for its authoritative quest log
    // snapshot (world.fbs QuestLog "sent on enter-world and on resync"). Replies with
    // the same S→C QuestLog table.
    on(net::Opcode::QUEST_LOG, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "QUEST_LOG")) return;
        if (ctx.quests) {
            send_s2c(sess, ctx, encode_frame(net::Opcode::QUEST_LOG, f.seq,
                                             encode_quest_log(*ctx.quests)));
            return;
        }
        // Not spawned yet — an empty log.
        fb::FlatBufferBuilder b;
        std::vector<fb::Offset<mn::QuestLogEntry>> none;
        b.Finish(mn::CreateQuestLog(b, b.CreateVector(none)));
        send_s2c(sess, ctx,
                 encode_frame(net::Opcode::QUEST_LOG, f.seq,
                              Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize())));
    });

    // --- 0x50xx LOOT: open / take / release a corpse loot window (ITM-02 #369) --
    // The corpse loot lives in the shared, thread-safe LootRegistry (ctx.loot); each
    // request re-runs the loot session's server-side gates (ownership / in-range /
    // not-already-looted / quest gate). A take moves the stack (or the copper) into
    // the character's DURABLE inventory (mint + place / add_money on char_db). The
    // looter is the session's OWN char_id + authoritative position — never a client
    // field. The quest gate reads the session's live quest log.

    on(net::Opcode::LOOT_REQUEST, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "LOOT_REQUEST")) return;
        const auto* req = fb::GetRoot<mn::LootRequest>(f.payload);
        if (req == nullptr) return;
        const std::uint64_t corpse_guid = req->corpse_guid();

        auto reply = [&](mn::LootStatus st, std::int64_t copper,
                         const std::vector<lo::LootSlotView>& slots) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::LOOT_RESPONSE, f.seq,
                                  encode_loot_response(corpse_guid, st, copper, slots)));
        };

        if (ctx.phase != SessionPhase::kInWorld || ctx.loot == nullptr || !ctx.movement) {
            reply(mn::LootStatus::NO_SUCH_CORPSE, 0, {});  // out-of-phase / no registry
            return;
        }
        const Position& p = ctx.movement->authoritative();
        const lo::LootPoint looter_pos{p.x, p.y, p.z};
        const lo::QuestPredicate has_quest = [&ctx](std::uint32_t q) {
            return ctx.quests && ctx.quests->is_active(q);
        };
        const LootWindow w = ctx.loot->open(corpse_guid, ctx.char_id, looter_pos, has_quest);
        reply(to_wire(w.status), static_cast<std::int64_t>(w.copper), w.slots);
        if (w.status == LootOpenStatus::kOk) ctx.open_corpse = corpse_guid;
    });

    on(net::Opcode::LOOT_TAKE, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "LOOT_TAKE")) return;
        const auto* req = fb::GetRoot<mn::LootTake>(f.payload);
        if (req == nullptr) return;
        const std::uint64_t corpse_guid = req->corpse_guid();
        const std::uint32_t slot = req->slot();
        const bool money = req->money();

        auto reply = [&](mn::LootTakeStatus st, std::uint32_t template_id,
                         std::uint32_t count, std::int64_t copper) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::LOOT_RESULT, f.seq,
                                  encode_loot_result(corpse_guid, slot, st, template_id, count,
                                                     copper)));
        };
        auto close_if_looted = [&](bool fully_looted) {
            if (!fully_looted) return;
            send_s2c(sess, ctx, encode_frame(net::Opcode::LOOT_CLOSED, f.seq,
                                             encode_loot_closed(corpse_guid)));
            if (ctx.open_corpse && *ctx.open_corpse == corpse_guid) ctx.open_corpse.reset();
        };

        if (ctx.phase != SessionPhase::kInWorld || ctx.loot == nullptr || !ctx.movement ||
            ctx.char_db == nullptr || ctx.char_id == 0) {
            reply(mn::LootTakeStatus::NO_SUCH_CORPSE, 0, 0, 0);  // out-of-phase / no registry
            return;
        }
        const Position& p = ctx.movement->authoritative();
        const lo::LootPoint looter_pos{p.x, p.y, p.z};
        const lo::QuestPredicate has_quest = [&ctx](std::uint32_t q) {
            return ctx.quests && ctx.quests->is_active(q);
        };

        if (money) {
            const TakeMoneyOutcome mo = ctx.loot->take_money(corpse_guid, ctx.char_id, looter_pos);
            if (mo.status != LootTakeResult::kOk) {
                reply(to_wire(mo.status), 0, 0, 0);
                return;
            }
            // OPS-03b (#421): defense-in-depth — the loot copper pile is server-
            // derived (the corpse's rolled loot), but guard the credit: a negative
            // copper amount is an impossible delta — audit + skip. add_money's
            // checked_add remains the authoritative overflow guard for the balance.
            if (const EconomyReject er = check_amount_nonnegative(mo.copper);
                er != EconomyReject::kNone) {
                audit::emit(build_economy_reject_audit(ctx.account_id, ctx.grant_id,
                                                       ctx.char_id, "loot_money", er));
            } else {
                try {
                    itm::add_money(*ctx.char_db, ctx.char_id, mo.copper);
                } catch (const std::exception& e) {
                    // No INTERNAL status on the loot wire; the copper left the corpse.
                    // Log loudly and still report the take (best-effort, degraded — M1).
                    log::warn(kCat, "LOOT_TAKE money credit failed",
                              {log::field("error", e.what())});
                }
            }
            reply(mn::LootTakeStatus::OK, 0, 0, static_cast<std::int64_t>(mo.copper));
            push_inventory_snapshot(sess, ctx);  // #453: money balance changed
            close_if_looted(mo.fully_looted);
            return;
        }

        // Item take: load the real inventory (capacity), note the free slot the loot
        // will land in, run the validated take, then persist the minted stack durably.
        try {
            itm::Inventory inv =
                itm::load_inventory(*ctx.char_db, ctx.char_id, item_templates());
            const std::optional<std::uint16_t> target = first_free_slot(inv);
            const TakeItemOutcome io = ctx.loot->take_item(corpse_guid, ctx.char_id, looter_pos,
                                                           slot, has_quest, inv);
            if (io.status != LootTakeResult::kOk) {
                reply(to_wire(io.status), 0, 0, 0);
                return;
            }
            if (target) {  // kOk implies room (else kInventoryFull) so target is set
                itm::ItemInstance minted = itm::mint_instance(
                    *ctx.char_db, io.stack.item_template_id, io.stack.count);
                itm::place_item(*ctx.char_db, ctx.char_id, /*bag=*/0,
                                itm::backpack_placement_slot(*target), minted.item_guid);
            }
            reply(mn::LootTakeStatus::OK, io.stack.item_template_id, io.stack.count, 0);
            push_inventory_snapshot(sess, ctx);  // #453: backpack gained the looted stack
            // Advance any collect objective the pull satisfied (QUEST_PROGRESS S→C).
            sync_collect_and_emit(sess, ctx, inv);
            close_if_looted(io.fully_looted);
        } catch (const std::exception& e) {
            log::warn(kCat, "LOOT_TAKE item persist failed", {log::field("error", e.what())});
            reply(mn::LootTakeStatus::NO_SUCH_CORPSE, 0, 0, 0);  // defensive (no INTERNAL enum)
        }
    });

    on(net::Opcode::LOOT_RELEASE, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "LOOT_RELEASE")) return;
        const auto* req = fb::GetRoot<mn::LootRelease>(f.payload);
        if (req == nullptr) return;
        const std::uint64_t corpse_guid = req->corpse_guid();
        // Close THIS session's loot window (per-connection state). The corpse's shared
        // loot is untouched — a release just stops this looter viewing it.
        if (ctx.open_corpse && *ctx.open_corpse == corpse_guid) ctx.open_corpse.reset();
        send_s2c(sess, ctx, encode_frame(net::Opcode::LOOT_CLOSED, f.seq,
                                         encode_loot_closed(corpse_guid)));
    });

    // --- 0x52xx NPC: gossip menu + trainer list / learn (NPC-01/02 #372) -------
    // The gossip menu + the trainer eligibility/cost are computed on the SERVER from
    // the placeholder NPC store, gated by the session's live quest state + class/level.
    // At M1 the wire npc_guid maps 1:1 to its npc_template id (no NPC entities spawn
    // yet — the mcc #28 spawn seam). A learn debits copper from character.money over
    // the wire (server-authoritative price; the client never supplies it).

    on(net::Opcode::GOSSIP_HELLO, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "GOSSIP_HELLO")) return;
        const auto* req = fb::GetRoot<mn::GossipHello>(f.payload);
        if (req == nullptr) return;
        const std::uint64_t npc_guid = req->npc_guid();

        auto empty_menu = [&]() {
            const npc::GossipMenu none;
            send_s2c(sess, ctx, encode_frame(net::Opcode::GOSSIP_MENU, f.seq,
                                             encode_gossip_menu(npc_guid, none)));
        };
        if (ctx.phase != SessionPhase::kInWorld || !ctx.quests) { empty_menu(); return; }
        // Resolve the clicked target to an npc_template id. When the client targets a
        // spawned world entity (#486), its wire guid is in the kWorldEntityGuidBase band
        // (not a template id) — map it back to the npc content it represents. Absent a
        // spawn (or no world registry), fall back to treating npc_guid AS the template id
        // (the pre-spawn 1:1 mapping the placeholder path relies on).
        npc::NpcId npc_id = static_cast<npc::NpcId>(npc_guid);
        if (ctx.world != nullptr) {
            if (auto tmpl = ctx.world->npc_template_for_guid(npc_guid))
                npc_id = static_cast<npc::NpcId>(*tmpl);
        }
        const npc::NpcDef* def = npc_store().find(npc_id);
        if (def == nullptr) {
            // Unknown NPC: no gossip content, but interacting still delivers (below).
            empty_menu();
        } else {
            const QuestLogView view(*ctx.quests, ctx.char_level);
            const npc::GossipMenu menu = npc::build_gossip_menu(*def, view);
            send_s2c(sess, ctx, encode_frame(net::Opcode::GOSSIP_MENU, f.seq,
                                             encode_gossip_menu(npc_guid, menu)));

            // A trainer NPC also gets its per-player ability list pushed (TrainerList S→C).
            if (def->is_trainer) {
                const std::int64_t balance = safe_balance(ctx);
                std::vector<TrainerRow> rows;
                rows.reserve(def->trainer_abilities.size());
                for (const npc::TrainerAbility& ta : def->trainer_abilities) {
                    const bool known = ctx.learned.knows(ta.ability_id);
                    const npc::TrainPlan plan = npc::plan_learn(
                        *def, ta.ability_id, ctx.char_class, ctx.char_level, balance, known);
                    rows.push_back(TrainerRow{ta.ability_id, static_cast<std::int64_t>(ta.cost),
                                              ta.required_class, ta.required_level,
                                              trainable_state(plan.status)});
                }
                send_s2c(sess, ctx, encode_frame(net::Opcode::TRAINER_LIST, f.seq,
                                                 encode_trainer_list(npc_guid, rows)));
            }

            // A vendor NPC also gets its for-sale catalog pushed (VendorList S→C,
            // #453) — mirroring the trainer-list push. Server-computed prices from
            // the vendor catalog (#390 DbVendorCatalog / M1 placeholder); the client
            // renders the buy window from it instead of a template-id greybox. The
            // buy path re-validates every price (Principle 1). An empty/unknown
            // catalog yields an empty list (the window is simply empty).
            if (def->is_vendor && def->vendor_id != 0) {
                const std::vector<vend::VendorListing>* listings =
                    vendor_catalog().listings(def->vendor_id);
                static const std::vector<vend::VendorListing> kEmpty;
                send_s2c(sess, ctx,
                         encode_frame(net::Opcode::VENDOR_LIST, f.seq,
                                      encode_vendor_list(def->vendor_id,
                                                         listings != nullptr ? *listings : kEmpty)));
            }
        }

        // Deliver (QST-01, #396): talking to an NPC is the deliver interaction — hand
        // any quest item this NPC is the deliver target for, completing the objective
        // (the item was granted on accept). QUEST_PROGRESS follows for whatever
        // advanced. Runs AFTER the gossip reply so a non-deliver interaction's reply
        // ordering (GOSSIP_MENU / TRAINER_LIST) is unchanged. Uses the resolved template
        // id so a deliver target clicked as a spawned entity (#486) still credits.
        credit_deliver_at_npc(sess, ctx, static_cast<std::uint32_t>(npc_id));
    });

    on(net::Opcode::TRAINER_LEARN, [](net::Session& sess, const Frame& f, ConnCtx& ctx) {
        if (!require_authenticated(ctx, "TRAINER_LEARN")) return;
        const auto* req = fb::GetRoot<mn::TrainerLearn>(f.payload);
        if (req == nullptr) return;
        const std::uint64_t npc_guid = req->npc_guid();
        const std::uint32_t ability_id = req->ability_id();

        auto reply = [&](mn::TrainerLearnStatus st, std::int64_t cost, std::int64_t balance) {
            send_s2c(sess, ctx,
                     encode_frame(net::Opcode::TRAINER_LEARN_RESULT, f.seq,
                                  encode_trainer_learn_result(npc_guid, ability_id, st, cost,
                                                              balance)));
        };

        if (ctx.phase != SessionPhase::kInWorld) {
            reply(mn::TrainerLearnStatus::NOT_TRAINER, 0, 0);  // out-of-phase
            return;
        }
        if (ctx.char_db == nullptr || ctx.char_id == 0) {  // defensive (unreachable in-world)
            reply(mn::TrainerLearnStatus::NOT_TRAINER, 0, 0);
            return;
        }
        const npc::NpcDef* def = npc_store().find(static_cast<npc::NpcId>(npc_guid));
        if (def == nullptr) { reply(mn::TrainerLearnStatus::NOT_TRAINER, 0, safe_balance(ctx)); return; }

        try {
            const npc::LearnResult r = npc::learn_ability(
                *ctx.char_db, ctx.char_id, *def, ability_id, ctx.char_class, ctx.char_level,
                ctx.learned);
            const std::int64_t cost =
                (r.status == npc::TrainStatus::kOk) ? static_cast<std::int64_t>(r.cost) : 0;
            reply(to_wire(r.status), cost, static_cast<std::int64_t>(r.new_balance));
            // On a SUCCESSFUL learn the character's known set grew — re-push
            // KNOWN_ABILITIES (#457) so the action bar (#456) picks up the newly
            // learned ability + its metadata without a re-enter. Sent AFTER the
            // TrainerLearnResult so the reply stays first in the stream. Rejections
            // leave the set unchanged (no push).
            if (r.status == npc::TrainStatus::kOk) push_known_abilities(sess, ctx);
        } catch (const std::exception& e) {
            log::warn(kCat, "TRAINER_LEARN failed", {log::field("error", e.what())});
            reply(mn::TrainerLearnStatus::NOT_TRAINER, 0, safe_balance(ctx));  // defensive
        }
    });

    // No handler is registered for server→client opcodes (HANDSHAKE_OK,
    // MOVEMENT_STATE, ENTITY_ENTER/UPDATE/LEAVE, CHAR_*_RESPONSE, CHAT_DELIVER/
    // CHAT_REJECTED, VENDOR_*_RESULT): a client sending one is out-of-direction and
    // is treated as an unknown opcode (Disconnect).
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
    std::atomic<std::size_t> simulated_player_count{0};

    // Shared world state + AoI grid (#87). Thread-safe internally; shared across
    // every serve_connection so a mover relays to the OTHER sessions in range.
    WorldState world;

    // Single active session per account (#326). Thread-safe internally; shared
    // across every serve_connection so a second login for an account kicks the
    // first (kick-old) — an account holds at most one in-world session.
    ActiveSessionRegistry active_sessions;

    // Shared corpse-loot registry (ITM-02 wire; #388). Thread-safe; shared across
    // every serve_connection so eligible looters open/take the same corpse session.
    // Seeded by the world-thread creature-death hook (#369) — or a test.
    LootRegistry loot;

    // Shared MapTick→session quest-kill credit registry (QST-01 event-bus, #396).
    // The world thread pushes creature-kill credits (route_tick_events, drained from
    // the map tick); each in-world session registers its guid + drains its own credits
    // on its IO worker. Thread-safe; the seam that credits a live kill to the killer's
    // accepted quests without MapTick owning any quest state.
    QuestCreditRegistry quest_credit;

    // Shared MapTick→session VITALS egress registry (UI-01 event-bus, #437). The world
    // thread pushes a level-up's new authoritative vitals (route_vitals_events, drained
    // from the map tick); each in-world session registers its guid + drains its own
    // snapshot and broadcasts a VITALS_UPDATE on its IO worker. Thread-safe; the seam
    // that pushes a MapTick-driven level-up to the HUD without MapTick owning egress.
    VitalsEgressRegistry vitals_egress;

    // The read-only ability template store (#343), loaded once at construction
    // with the M1 placeholder set (epic #28 swaps the source behind the store).
    // Shared read-only across every serve_connection — the CAST_REQUEST handler
    // looks abilities up here with no lock (O(1), single load — client SAD §2.4).
    AbilityStore abilities = load_placeholder_ability_store();

    // The runtime playable roster CHAR_CREATE validates against (SP2.5 #695).
    // Defaults to the full offline M0 set so a DB-less run still creates characters;
    // set_roster() swaps in the pack-loaded roster (the `race`/`class` world-DB rows
    // merged with the compiled fallback) at boot. Shared read-only across every
    // serve_connection by address (ctx.roster), so the move-assign in set_roster
    // keeps every ConnCtx pointer valid.
    chr::Roster roster = chr::Roster::offline_full();

    // The per-class equip-gating + role catalog (SP2.7 #697). Empty until
    // set_class_catalog() installs the pack-loaded classes at boot; the MapTick role
    // threat hook borrows it by address (set below). Empty -> threat_multiplier is
    // 1.0 for every class, so a DB-less run's threat is unscaled.
    ClassCatalog classes;
    EquipTypeCatalog equip_types;

    // The enter-world spawn (C8 enter-as-chibi, #761): the realm's START ZONE first
    // graveyard position (load_start_zone_spawn), in the worldd Z-up runtime frame.
    // std::nullopt until set_enter_spawn() installs it at boot; every ConnCtx borrows
    // it by address (ctx.enter_spawn = &*enter_spawn), so the move-assign in
    // set_enter_spawn keeps that pointer valid. nullopt (DB-less / no start zone) ->
    // the ENTER_WORLD handler keeps the movement::kZoneSpawnXY D-11 placeholder.
    std::optional<Position> enter_spawn;

    // The per-map tick orchestrator (SAD §2.5 phase order; #349). Owned by + run
    // ONLY on the world thread. Each tick it runs the AI -> combat/auras ->
    // spawns/respawns passes for the map's server-controlled creatures + auras +
    // cast timers (the #354 cast-completion seam). At M1 the bootstrap map has no
    // creatures spawned (content wiring is #28), so has_simulation() is false and
    // the pass is skipped — the seam is real, the population lands with the content
    // pipeline. A fixed per-map seed keeps a single-worker boot deterministic
    // (mirrors WorldState::combat_rng_; the MapKey-derived seed is a map-manager
    // concern). Declared AFTER `abilities` so its ctor can borrow it.
    MapTick map{abilities, 0x9E3779B97F4A7C15ULL, kTickDtMs};
    struct MapPlayerOwner {
        std::uint64_t account_id = 0;
        SessionToken token = 0;
        SessionGeneration generation = 0;
    };
    // World-thread-only current owner and persistent admission high-water. The
    // high-water is deliberately retained after leave: a delayed old enter must
    // never resurrect a session generation that has already been superseded.
    std::unordered_map<ObjectGuid, MapPlayerOwner> map_player_owners;
    std::unordered_map<ObjectGuid, SessionGeneration> map_player_high_water;
    mutable std::mutex map_player_diag_mtx;
    std::unordered_map<ObjectGuid, Position> map_player_positions;
};

WorldServer::WorldServer(const Dispatcher& dispatcher, WorldServerConfig cfg)
    : dispatcher_(dispatcher), cfg_(cfg), impl_(std::make_unique<Impl>()) {
    // QST-01 event-bus (#396): the live map REPORTS creature kills as typed events
    // the world thread routes to the killer's session (route_tick_events → the
    // quest_credit bus). MapTick owns no quest state itself.
    impl_->map.set_report_kills(true);
    // UI-01 event-bus (#437): the live map tags a level-up as kVitalsChanged carrying
    // the new authoritative vitals the world thread routes to the leveler's session
    // (route_vitals_events → the vitals_egress bus), which mirrors them onto its unit
    // and pushes a VITALS_UPDATE. MapTick owns no egress itself.
    impl_->map.set_report_vitals(true);
    // SP2.7 #697 role hook: the map borrows the class catalog by address so the
    // resolver->AI threat seam can scale a Tank caster's threat. The catalog starts
    // empty (multiplier 1.0 for all) and is filled by set_class_catalog at boot; the
    // move-assign there preserves this address.
    impl_->map.set_class_catalog(&impl_->classes);
}

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

LootRegistry& WorldServer::loot_registry() { return impl_->loot; }

QuestCreditRegistry& WorldServer::quest_credit() { return impl_->quest_credit; }

VitalsEgressRegistry& WorldServer::vitals_egress() { return impl_->vitals_egress; }

// The world thread's per-tick kill-credit routing (QST-01 event-bus, #396). Kept a
// free function (declared in world_dispatch.h) so the integration test drives the
// EXACT routing the world thread runs, with a real MapTick's deltas.
void route_tick_events(const std::vector<TickEvent>& deltas, QuestCreditRegistry& reg) {
    for (const TickEvent& ev : deltas) {
        if (ev.kind == TickEventKind::kCreatureKill)
            reg.push_kill(ev.killer_guid, ev.npc_template_id);
    }
}

// The world thread's per-tick VITALS-egress routing (UI-01 event-bus, #437). Kept a
// free function (declared in world_dispatch.h) so the integration test drives the
// EXACT routing the world thread runs, with a real MapTick's deltas.
void route_vitals_events(const std::vector<TickEvent>& deltas, VitalsEgressRegistry& reg) {
    for (const TickEvent& ev : deltas) {
        if (ev.kind == TickEventKind::kVitalsChanged)
            reg.push_vitals(ev.vitals);
    }
}

void WorldServer::set_loot_tables(const loot::LootTableStore& store) {
    impl_->map.set_loot_tables(store);
}

void WorldServer::install_spawns(const std::vector<SpawnPlacement>& spawns) {
    // Boot-time, single-threaded (before start()): populate the live world from the
    // authored spawn_point content (#486). Each placement lands in TWO systems:
    //   1. the per-map TICK (impl_->map) — MapTick::add_creature makes the creature
    //      EXIST in the simulation (AI phase / respawn seam / the kill-objective
    //      creature the world thread routes via route_tick_events). Stats come from a
    //      CreatureSpawnDef built from the resolved placement.
    //   2. the AoI RELAY (impl_->world) — add_world_entity makes it VISIBLE: a player
    //      entering range gets an ENTITY_ENTER carrying its #430 vitals + name, and a
    //      gossip NPC is targetable by its wire guid (npc_template_for_guid resolves it
    //      for GOSSIP_HELLO). The wire guid is assigned in the kWorldEntityGuidBase band.
    // The two representations are independent (the M1 live combat path resolves targets
    // through the AoI unit; the MapTick creature is the simulation/kill substrate) — the
    // shared truth is the authored placement.
    AoiId next_entity_guid = kWorldEntityGuidBase;
    std::size_t n = 0;
    for (const SpawnPlacement& sp : spawns) {
        // (1) MapTick existence. A minimal CreatureSpawnDef — the M1 goal is EXISTENCE
        // + visibility; leash/aggro/patrol tuning is the AI stories (#346-#348). The
        // respawn delay is carried from content (seconds -> ms); a wandering spawn uses
        // its wander radius as the leash, else a generous default so it never evades.
        CreatureSpawnDef def;
        def.template_id = sp.npc_id;
        def.level = sp.stats.level;
        def.faction = sp.stats.faction;
        def.authored_stats = sp.stats;
        def.behavior = sp.behavior;
        def.damage_min = sp.damage_min;
        def.damage_max = sp.damage_max;
        def.attack_speed_ms = sp.attack_speed_ms;
        def.home = sp.pos;
        def.aggro_base_radius = sp.aggro_radius_m;
        def.leash_radius = sp.leash_radius_m;
        def.respawn_ms = sp.respawn_min * 1000u;
        def.move_speed = sp.run_speed_mps;
        impl_->map.add_creature(def);

        // (2) AoI visibility + interactability. The wire projection: a unique guid, the
        // npc id as the type kind, and the display name (#430 nameplate + vitals). The
        // resolved combat stats give real health/level/faction on EntityEnter.
        EntityIdentity id;
        id.entity_guid = next_entity_guid++;
        id.type_id = sp.npc_id;   // the client maps the npc id to its model/appearance
        id.char_class = 0;        // not a player — no class coloring
        id.name = sp.name;
        // npc@2 (contract ①/§7, #821): an NPC that carries an appearance_catalog
        // relays the SAME visual-assembly block a player does — the encoder emits it
        // whenever identity.visual is set (not gated on player-vs-NPC), so the client
        // assembles + recolors the body via the proven player path. A model-only NPC
        // leaves visual unset (nullopt) and stays on the monolithic visual.model path.
        if (sp.appearance) id.visual = *sp.appearance;
        impl_->world.add_world_entity(id, sp.stats, sp.npc_id, sp.pos);
        ++n;
    }
    log::info(kCat, "installed " + std::to_string(n) + " content spawn(s) into the live world");
}

std::size_t WorldServer::map_creature_count() const {
    return impl_->map.ai().size();
}

std::size_t WorldServer::map_player_count() const {
    return impl_->simulated_player_count.load();
}

std::optional<Position> WorldServer::map_player_position(ObjectGuid guid) const {
    std::lock_guard<std::mutex> lk(impl_->map_player_diag_mtx);
    auto it = impl_->map_player_positions.find(guid);
    if (it == impl_->map_player_positions.end()) return std::nullopt;
    return it->second;
}

void WorldServer::set_abilities(AbilityStore store) {
    // Move-assign into the store MapTick + every ConnCtx already reference by address
    // (impl_->abilities). The object's address is unchanged by a move-assign, so the
    // MapTick reference (map_tick.h abilities_) and ctx.abilities pointers stay valid.
    // Boot-time only (before start()), so no concurrent reader races the swap.
    impl_->abilities = std::move(store);
}

void WorldServer::set_roster(chr::Roster roster) {
    // Move-assign into the roster every ConnCtx borrows by address (impl_->roster).
    // The object's address is unchanged by a move-assign, so ctx.roster pointers stay
    // valid. Boot-time only (before start()), so no concurrent reader races the swap.
    impl_->roster = std::move(roster);
}

void WorldServer::set_class_catalog(ClassCatalog classes) {
    // Move-assign into the catalog the MapTick borrows by address (impl_->classes,
    // wired in the ctor). The address is unchanged by a move-assign, so the MapTick
    // pointer stays valid. Boot-time only (before start()), so no reader races it.
    impl_->classes = std::move(classes);
}

void WorldServer::set_equip_type_catalog(EquipTypeCatalog equip_types) {
    impl_->equip_types = std::move(equip_types);
}

void WorldServer::set_enter_spawn(std::optional<Position> spawn) {
    // Move-assign into the optional every ConnCtx borrows by address (ctx.enter_spawn
    // = &*impl_->enter_spawn). The optional's storage address is unchanged by an
    // assignment, so those pointers stay valid. Boot-time only (before start()), so no
    // reader races the swap. std::nullopt keeps the movement::kZoneSpawnXY placeholder.
    impl_->enter_spawn = std::move(spawn);
}

void WorldServer::world_thread_main() {
    // The per-map map tick (SAD §2.5 / §3.2), 20 Hz with a 40 ms soft budget
    // (SAD §8.1). Each wake runs ONE tick in the SAD §2.5 PHASE ORDER:
    //   drain inbound -> movement/commands -> AI -> combat/auras ->
    //   spawns/respawns -> AoI delta build -> flush.
    // The inbound queue drain (many IO workers -> this one world thread) is the
    // "drain inbound" phase; the AI / combat/auras / spawns-respawns passes run in
    // MapTick (impl_->map) — the single-threaded orchestrator that owns the map's
    // creatures + auras + cast timers, and that resolves completed casts + spends
    // their resource (the #354 cast-completion seam). Player combat currently
    // resolves on the owning IO worker (the M1 per-connection model —
    // poll_completed_cast); it migrates onto this thread with the multi-worker map
    // manager (SAD §2.5 "recast at M3"). This is the ONE thread that owns
    // simulation work (game state), so the IO/accept path never does.
    using namespace std::chrono;
    const auto interval = milliseconds(cfg_.tick_interval_ms);
    const auto soft_budget = milliseconds(kTickSoftBudgetMs);

    // OPS-05: the tick-duration histogram (meridian_tick_duration_seconds
    // {realm,zone,shard,map}; p99 = tick health, the Realm-health headline). We
    // time the TICK BODY (drain + phases), NOT the wait_for idle — the metric is
    // "how long the tick's work took", so a truly idle tick is not measured (it
    // would report ~0 and drown the real work in the histogram).
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

        // A truly idle tick — nothing inbound AND no map to simulate (SAD §2.5
        // "inactive grids do not tick") — is skipped so the histogram measures
        // real work only.
        if (batch.empty() && !impl_->map.has_simulation()) continue;

        const auto tick_t0 = steady_clock::now();

        // PHASE 1 — drain inbound. Player lifecycle and movement are applied only
        // here, on the world thread, before AI snapshots targets this tick. Opaque
        // scaffold events remain accounting-only.
        for (const WorldEvent& ev : batch) {
            switch (ev.kind) {
                case WorldEventKind::kPlayerEnter: {
                    const SessionGeneration high =
                        impl_->map_player_high_water[ev.player_guid];
                    if (ev.player_session_generation <= high ||
                        !impl_->active_sessions.is_current(
                            ev.player_account_id, ev.player_session_token,
                            ev.player_session_generation)) {
                        break;
                    }
                    impl_->map_player_high_water[ev.player_guid] =
                        ev.player_session_generation;
                    impl_->map.remove_player(ev.player_guid);
                    impl_->map.add_player(ev.player_guid, ev.player_pos,
                                          ev.player_stats, ev.char_class);
                    impl_->map_player_owners[ev.player_guid] = {
                        ev.player_account_id, ev.player_session_token,
                        ev.player_session_generation};
                    {
                        std::lock_guard<std::mutex> lk(impl_->map_player_diag_mtx);
                        impl_->map_player_positions[ev.player_guid] = ev.player_pos;
                    }
                    impl_->simulated_player_count.store(
                        impl_->map_player_owners.size());
                    break;
                }
                case WorldEventKind::kPlayerMove: {
                    auto it = impl_->map_player_owners.find(ev.player_guid);
                    if (it != impl_->map_player_owners.end() &&
                        it->second.account_id == ev.player_account_id &&
                        it->second.token == ev.player_session_token &&
                        it->second.generation == ev.player_session_generation) {
                        impl_->map.set_player_position(ev.player_guid, ev.player_pos);
                        std::lock_guard<std::mutex> lk(impl_->map_player_diag_mtx);
                        impl_->map_player_positions[ev.player_guid] = ev.player_pos;
                    }
                    break;
                }
                case WorldEventKind::kPlayerLeave: {
                    auto it = impl_->map_player_owners.find(ev.player_guid);
                    if (it != impl_->map_player_owners.end() &&
                        it->second.account_id == ev.player_account_id &&
                        it->second.token == ev.player_session_token &&
                        it->second.generation == ev.player_session_generation) {
                        impl_->map.remove_player(ev.player_guid);
                        impl_->map_player_owners.erase(it);
                        {
                            std::lock_guard<std::mutex> lk(impl_->map_player_diag_mtx);
                            impl_->map_player_positions.erase(ev.player_guid);
                        }
                        impl_->simulated_player_count.store(
                            impl_->map_player_owners.size());
                    }
                    break;
                }
                case WorldEventKind::kOpaque:
                    break;
            }
            impl_->drained.fetch_add(1);
        }

        // PHASES 2-6 — movement/commands -> AI (threat/aggro/leash/evade/patrol) ->
        // combat/auras (casts complete + resource spend, periodic DoT/HoT, deaths)
        // -> spawns/respawns. MapTick runs them in exactly this order; the returned
        // event stream is the per-tick AoI delta the FLUSH phase (7) serialises to
        // observers (wired to the AoI relay with the #28 content spawns).
        if (impl_->map.has_simulation()) {
            const std::vector<TickEvent> deltas = impl_->map.advance();
            // QST-01 event-bus (#396): route creature-kill deltas to the killer's
            // session via the shared credit registry (the session drains + applies
            // them on its own IO worker). Other deltas are the AoI/flush stream.
            route_tick_events(deltas, impl_->quest_credit);
            // UI-01 event-bus (#437): route level-up vitals deltas to the leveler's
            // session via the shared vitals-egress registry (the session drains +
            // mirrors + broadcasts a VITALS_UPDATE on its own IO worker).
            route_vitals_events(deltas, impl_->vitals_egress);
        }

        // Record the tick + log a soft-budget overrun (SAD §8.1: 50 ms hard / 40 ms
        // soft per map). Sustained overruns are the first signal a map is too hot.
        const auto elapsed = steady_clock::now() - tick_t0;
        tick_hist.observe(duration<double>(elapsed).count());
        if (elapsed > soft_budget) {
            log::warn(kCat,
                      "tick overrun: " +
                          std::to_string(duration_cast<milliseconds>(elapsed).count()) +
                          " ms > " + std::to_string(kTickSoftBudgetMs) +
                          " ms soft budget (map=" + cfg_.labels.map + ")");
        }
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
    ctx.roster = &impl_->roster;  // runtime playable roster CHAR_CREATE validates against (#695)
    ctx.class_catalog = &impl_->classes;
    ctx.equip_type_catalog = &impl_->equip_types;
    // Enter-world spawn (C8 #761): borrow the boot-loaded start-zone graveyard position
    // by address when a start zone was loaded; nullptr keeps the D-11 placeholder.
    ctx.enter_spawn = impl_->enter_spawn ? &*impl_->enter_spawn : nullptr;
    ctx.active_sessions = &impl_->active_sessions;  // single-session registry (#326)
    ctx.loot = &impl_->loot;  // shared corpse loot registry (ITM-02 wire; #388)
    ctx.quest_credit = &impl_->quest_credit;  // MapTick→session kill credit bus (#396)
    ctx.vitals_egress = &impl_->vitals_egress;  // MapTick→session vitals egress bus (#437)
    ctx.simulation_enqueue = [this](WorldEvent ev) { enqueue(std::move(ev)); };
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

            if (outcome == DispatchOutcome::kHandled) {
                // #349: resolve any cast-time ability whose timer elapsed (the
                // #354 cast-completion + resource-spend seam). Cheap no-op unless a
                // cast is pending and has reached its end time.
                poll_completed_cast(sess, ctx, steady_now_ms());
                // #396: drain + apply this session's pending MapTick kill credits and
                // push QUEST_PROGRESS for any kill objective that advanced (QST-01
                // event-bus). Cheap no-op unless a kill is pending for this guid.
                poll_quest_credits(sess, ctx);
                // #437: drain + apply this session's pending MapTick level-up vitals and
                // push a VITALS_UPDATE (UI-01 HUD). Cheap no-op unless a level-up is
                // pending for this guid.
                poll_vitals_egress(sess, ctx);
                // #418: apply a pending GM `.summon` forced move so an idle summoned
                // player snaps on its next action (any frame), not only on a move.
                drain_forced_move(sess, ctx);
                continue;
            }

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
    if (ctx.entered && ctx.movement && ctx.simulation_enqueue) {
        WorldEvent ev;
        ev.kind = WorldEventKind::kPlayerLeave;
        ev.player_guid = ctx.movement->entity_guid();
        ev.player_account_id = ctx.account_id;
        ev.player_session_token = ctx.session_token;
        ev.player_session_generation = ctx.session_generation;
        ctx.simulation_enqueue(std::move(ev));
    }
    // QST-01 event-bus (#396): drop this session from the quest-kill credit bus so no
    // further kill is retained for its guid (guarded by credit_guid — set only if it
    // ever registered at ENTER_WORLD).
    if (ctx.quest_credit != nullptr && ctx.credit_guid != 0) {
        ctx.quest_credit->unregister_session(ctx.credit_guid, ctx.credit_token);
    }
    // UI-01 event-bus (#437): drop this session from the vitals egress bus so no
    // further level-up snapshot is retained for its guid (guarded by credit_guid +
    // vitals_token).
    if (ctx.vitals_egress != nullptr && ctx.credit_guid != 0) {
        ctx.vitals_egress->unregister_session(ctx.credit_guid, ctx.vitals_token);
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
