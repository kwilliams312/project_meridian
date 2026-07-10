// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — FlatBuffers codec helpers (issue #95). Uses the flatc-generated
// auth.fbs / world.fbs tables (the same codegen authd/worldd/bot use). Applies the
// verify-before-GetRoot discipline on all untrusted input. No GPL source consulted
// (CONTRIBUTING.md).

#include "meridian/clientnet/codec.h"

#include "auth_generated.h"
#include "world_generated.h"

namespace meridian::clientnet::codec {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;

Bytes to_bytes(const fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// Verify the FlatBuffer root table `T` over untrusted bytes, then GetRoot. Returns
// nullptr on a failed verify (never GetRoot on unverified bytes).
template <typename T>
const T* verify_and_get(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

}  // namespace

// ---- IF-1 ClientHello ------------------------------------------------------

Bytes encode_client_hello(const ClientHello& in) {
    fb::FlatBufferBuilder b;
    auto root = mn::CreateClientHello(b, in.build, in.proto_ver);
    b.Finish(root);
    return to_bytes(b);
}

std::optional<ClientHello> decode_client_hello(const Bytes& buf) {
    const mn::ClientHello* t = verify_and_get<mn::ClientHello>(buf);
    if (t == nullptr) return std::nullopt;
    ClientHello out;
    out.build = t->build();
    out.proto_ver = t->proto_ver();
    return out;
}

// ---- IF-2 MovementIntent ---------------------------------------------------

Bytes encode_movement_intent(const MovementIntent& in) {
    fb::FlatBufferBuilder b;
    auto root = mn::CreateMovementIntent(b, in.seq, in.state_flags, in.x, in.y, in.z,
                                         in.orientation, in.client_time_ms);
    b.Finish(root);
    return to_bytes(b);
}

std::optional<MovementIntent> decode_movement_intent(const Bytes& buf) {
    const mn::MovementIntent* t = verify_and_get<mn::MovementIntent>(buf);
    if (t == nullptr) return std::nullopt;
    MovementIntent out;
    out.seq = t->seq();
    out.state_flags = t->state_flags();
    out.x = t->x();
    out.y = t->y();
    out.z = t->z();
    out.orientation = t->orientation();
    out.client_time_ms = t->client_time_ms();
    return out;
}

// ---- IF-2 MovementState ----------------------------------------------------

Bytes encode_movement_state(const MovementState& in) {
    fb::FlatBufferBuilder b;
    auto root = mn::CreateMovementState(b, in.entity_guid, in.ack_seq, in.state_flags,
                                        in.x, in.y, in.z, in.orientation,
                                        in.server_time_ms);
    b.Finish(root);
    return to_bytes(b);
}

std::optional<MovementState> decode_movement_state(const Bytes& buf) {
    const mn::MovementState* t = verify_and_get<mn::MovementState>(buf);
    if (t == nullptr) return std::nullopt;
    MovementState out;
    out.entity_guid = t->entity_guid();
    out.ack_seq = t->ack_seq();
    out.state_flags = t->state_flags();
    out.x = t->x();
    out.y = t->y();
    out.z = t->z();
    out.orientation = t->orientation();
    out.server_time_ms = t->server_time_ms();
    return out;
}

// ---- IF-2 Disconnect -------------------------------------------------------

Bytes encode_disconnect(const Disconnect& in) {
    fb::FlatBufferBuilder b;
    auto msg = in.message.empty() ? 0 : b.CreateString(in.message);
    auto root = mn::CreateDisconnect(
        b, static_cast<mn::DisconnectReason>(in.reason), msg);
    b.Finish(root);
    return to_bytes(b);
}

std::optional<Disconnect> decode_disconnect(const Bytes& buf) {
    const mn::Disconnect* t = verify_and_get<mn::Disconnect>(buf);
    if (t == nullptr) return std::nullopt;
    Disconnect out;
    out.reason = static_cast<std::uint16_t>(t->reason());
    out.message = t->message() ? t->message()->str() : std::string();
    return out;
}

// ---- IF-2 EntityEnter (#87 AoI relay) --------------------------------------

Bytes encode_entity_enter(const EntityEnter& in) {
    fb::FlatBufferBuilder b;
    // attrs empty at M0 — created so the table shape is complete for the verifier
    // (mirrors worldd's encode_entity_enter_payload).
    auto attrs = b.CreateVector(std::vector<fb::Offset<mn::AttrDelta>>{});
    // name (#430): the character/creature display name for the unit frame. Empty
    // string round-trips as an empty name (worldd sends "" for the D-11 placeholder).
    auto name = b.CreateString(in.name);
    auto e = mn::CreateEntityEnter(b, in.entity_guid, in.type_id, in.x, in.y, in.z,
                                   in.orientation, attrs, in.char_class, in.health,
                                   in.max_health, in.power, in.max_power,
                                   static_cast<mn::PowerType>(in.power_type), in.level,
                                   name);
    b.Finish(e);
    return to_bytes(b);
}

std::optional<EntityEnter> decode_entity_enter(const Bytes& buf) {
    const mn::EntityEnter* t = verify_and_get<mn::EntityEnter>(buf);
    if (t == nullptr) return std::nullopt;
    EntityEnter out;
    out.entity_guid = t->entity_guid();
    out.type_id = t->type_id();
    out.x = t->x();
    out.y = t->y();
    out.z = t->z();
    out.orientation = t->orientation();
    out.char_class = t->char_class();  // #328: class id for the client capsule color
    // Vitals (#430 HUD contract): additive fields — a pre-#430 producer defaults them.
    out.health = t->health();
    out.max_health = t->max_health();
    out.power = t->power();
    out.max_power = t->max_power();
    out.power_type = static_cast<std::uint8_t>(t->power_type());
    out.level = t->level();
    out.name = t->name() ? t->name()->str() : std::string();
    return out;
}

// ---- IF-2 VitalsUpdate (#430/#431 HUD delta) -------------------------------

Bytes encode_vitals_update(const VitalsUpdate& in) {
    fb::FlatBufferBuilder b;
    auto v = mn::CreateVitalsUpdate(b, in.entity_guid, in.health, in.max_health,
                                    in.power, in.max_power,
                                    static_cast<mn::PowerType>(in.power_type), in.level);
    b.Finish(v);
    return to_bytes(b);
}

std::optional<VitalsUpdate> decode_vitals_update(const Bytes& buf) {
    const mn::VitalsUpdate* t = verify_and_get<mn::VitalsUpdate>(buf);
    if (t == nullptr) return std::nullopt;
    VitalsUpdate out;
    out.entity_guid = t->entity_guid();
    out.health = t->health();
    out.max_health = t->max_health();
    out.power = t->power();
    out.max_power = t->max_power();
    out.power_type = static_cast<std::uint8_t>(t->power_type());
    out.level = t->level();
    return out;
}

// ---- IF-2 EntityUpdate (#87 AoI relay) -------------------------------------

Bytes encode_entity_update(const EntityUpdate& in) {
    fb::FlatBufferBuilder b;
    auto attrs = b.CreateVector(std::vector<fb::Offset<mn::AttrDelta>>{});
    // world.fbs marks x/y/z/orientation optional; a move delta carries them all
    // (mirrors worldd's encode_entity_update_payload, which always sends position).
    auto u = mn::CreateEntityUpdate(b, in.entity_guid, in.x, in.y, in.z, in.orientation,
                                    attrs);
    b.Finish(u);
    return to_bytes(b);
}

std::optional<EntityUpdate> decode_entity_update(const Bytes& buf) {
    const mn::EntityUpdate* t = verify_and_get<mn::EntityUpdate>(buf);
    if (t == nullptr) return std::nullopt;
    EntityUpdate out;
    out.entity_guid = t->entity_guid();
    // Each position field is optional (default null) — record whichever the delta
    // carried so a movement-only vs attribute-only delta are distinguishable.
    const auto ox = t->x();
    const auto oy = t->y();
    const auto oz = t->z();
    const auto oo = t->orientation();
    out.has_x = ox.has_value();
    out.has_y = oy.has_value();
    out.has_z = oz.has_value();
    out.has_orientation = oo.has_value();
    out.x = ox.has_value() ? *ox : 0.0f;
    out.y = oy.has_value() ? *oy : 0.0f;
    out.z = oz.has_value() ? *oz : 0.0f;
    out.orientation = oo.has_value() ? *oo : 0.0f;
    return out;
}

// ---- IF-2 EntityLeave (#87 AoI relay) --------------------------------------

Bytes encode_entity_leave(const EntityLeave& in) {
    fb::FlatBufferBuilder b;
    auto l = mn::CreateEntityLeave(b, in.entity_guid,
                                   static_cast<mn::LeaveReason>(in.reason));
    b.Finish(l);
    return to_bytes(b);
}

std::optional<EntityLeave> decode_entity_leave(const Bytes& buf) {
    const mn::EntityLeave* t = verify_and_get<mn::EntityLeave>(buf);
    if (t == nullptr) return std::nullopt;
    EntityLeave out;
    out.entity_guid = t->entity_guid();
    out.reason = static_cast<std::uint16_t>(t->reason());
    return out;
}

// ---- IF-2 character management (D-35 / #286) -------------------------------

Bytes encode_char_list_request() {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCharListRequest(b));
    return to_bytes(b);
}

std::optional<CharListResponse> decode_char_list_response(const Bytes& buf) {
    const mn::CharListResponse* t = verify_and_get<mn::CharListResponse>(buf);
    if (t == nullptr) return std::nullopt;
    CharListResponse out;
    if (t->characters() != nullptr) {
        out.characters.reserve(t->characters()->size());
        for (const auto* e : *t->characters()) {
            if (e == nullptr) continue;
            CharSummary c;
            c.character_id = e->character_id();
            if (e->name() != nullptr) c.name = e->name()->str();
            c.race = e->race();
            c.char_class = e->char_class();
            c.level = e->level();
            out.characters.push_back(std::move(c));
        }
    }
    return out;
}

Bytes encode_char_create_request(const CharCreateRequest& in) {
    fb::FlatBufferBuilder b;
    auto name = b.CreateString(in.name);
    b.Finish(mn::CreateCharCreateRequest(b, name, in.race, in.char_class));
    return to_bytes(b);
}

std::optional<CharCreateResponse> decode_char_create_response(const Bytes& buf) {
    const mn::CharCreateResponse* t = verify_and_get<mn::CharCreateResponse>(buf);
    if (t == nullptr) return std::nullopt;
    CharCreateResponse out;
    out.status = static_cast<std::uint16_t>(t->status());
    out.character_id = t->character_id();
    return out;
}

Bytes encode_char_delete_request(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCharDeleteRequest(b, character_id));
    return to_bytes(b);
}

std::optional<CharDeleteResponse> decode_char_delete_response(const Bytes& buf) {
    const mn::CharDeleteResponse* t = verify_and_get<mn::CharDeleteResponse>(buf);
    if (t == nullptr) return std::nullopt;
    CharDeleteResponse out;
    out.status = static_cast<std::uint16_t>(t->status());
    return out;
}

// ---- IF-2 server-authoritative enter-world (D-35 / #341) -------------------

Bytes encode_enter_world_request(std::uint64_t character_id) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldRequest(b, character_id));
    return to_bytes(b);
}

std::optional<EnterWorldResponse> decode_enter_world_response(const Bytes& buf) {
    const mn::EnterWorldResponse* t = verify_and_get<mn::EnterWorldResponse>(buf);
    if (t == nullptr) return std::nullopt;
    EnterWorldResponse out;
    out.status = static_cast<std::uint16_t>(t->status());
    return out;
}

// ---- S→C response encoders (test/mock symmetry — client never sends these) ---

Bytes encode_char_list_response(const CharListResponse& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::CharListEntry>> rows;
    rows.reserve(in.characters.size());
    for (const auto& c : in.characters) {
        auto n = b.CreateString(c.name);
        rows.push_back(mn::CreateCharListEntry(b, c.character_id, n, c.race,
                                               c.char_class, c.level));
    }
    b.Finish(mn::CreateCharListResponse(b, b.CreateVector(rows)));
    return to_bytes(b);
}

Bytes encode_char_create_response(const CharCreateResponse& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCharCreateResponse(
        b, static_cast<mn::CharCreateStatus>(in.status), in.character_id));
    return to_bytes(b);
}

Bytes encode_char_delete_response(const CharDeleteResponse& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCharDeleteResponse(
        b, static_cast<mn::CharDeleteStatus>(in.status)));
    return to_bytes(b);
}

Bytes encode_enter_world_response(const EnterWorldResponse& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateEnterWorldResponse(
        b, static_cast<mn::EnterWorldStatus>(in.status)));
    return to_bytes(b);
}

// ---- IF-2 GOSSIP (0x52xx — NPC-01/02, #372/#433) ---------------------------

Bytes encode_gossip_hello(const GossipHello& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateGossipHello(b, in.npc_guid));
    return to_bytes(b);
}

std::optional<GossipHello> decode_gossip_hello(const Bytes& buf) {
    const mn::GossipHello* t = verify_and_get<mn::GossipHello>(buf);
    if (t == nullptr) return std::nullopt;
    GossipHello out;
    out.npc_guid = t->npc_guid();
    return out;
}

Bytes encode_gossip_menu(const GossipMenu& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::GossipOption>> rows;
    rows.reserve(in.options.size());
    for (const auto& o : in.options) {
        rows.push_back(mn::CreateGossipOption(
            b, static_cast<mn::GossipOptionKind>(o.kind), o.target_id));
    }
    b.Finish(mn::CreateGossipMenu(b, in.npc_guid, b.CreateVector(rows)));
    return to_bytes(b);
}

std::optional<GossipMenu> decode_gossip_menu(const Bytes& buf) {
    const mn::GossipMenu* t = verify_and_get<mn::GossipMenu>(buf);
    if (t == nullptr) return std::nullopt;
    GossipMenu out;
    out.npc_guid = t->npc_guid();
    if (t->options() != nullptr) {
        out.options.reserve(t->options()->size());
        for (const auto* o : *t->options()) {
            if (o == nullptr) continue;
            out.options.push_back(
                GossipOption{static_cast<std::uint16_t>(o->kind()), o->target_id()});
        }
    }
    return out;
}

// ---- IF-2 QUEST (0x40xx — QST-01, #371/#433) -------------------------------

Bytes encode_quest_accept(const QuestAccept& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestAccept(b, in.quest_id, in.giver_guid));
    return to_bytes(b);
}

std::optional<QuestAccept> decode_quest_accept(const Bytes& buf) {
    const mn::QuestAccept* t = verify_and_get<mn::QuestAccept>(buf);
    if (t == nullptr) return std::nullopt;
    QuestAccept out;
    out.quest_id = t->quest_id();
    out.giver_guid = t->giver_guid();
    return out;
}

Bytes encode_quest_accept_result(const QuestAcceptResult& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestAcceptResult(
        b, in.quest_id, static_cast<mn::QuestAcceptStatus>(in.status)));
    return to_bytes(b);
}

std::optional<QuestAcceptResult> decode_quest_accept_result(const Bytes& buf) {
    const mn::QuestAcceptResult* t = verify_and_get<mn::QuestAcceptResult>(buf);
    if (t == nullptr) return std::nullopt;
    QuestAcceptResult out;
    out.quest_id = t->quest_id();
    out.status = static_cast<std::uint16_t>(t->status());
    return out;
}

Bytes encode_quest_progress(const QuestProgress& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestProgress(
        b, in.quest_id, in.objective_index,
        static_cast<mn::QuestObjectiveType>(in.type), in.have, in.need, in.complete));
    return to_bytes(b);
}

std::optional<QuestProgress> decode_quest_progress(const Bytes& buf) {
    const mn::QuestProgress* t = verify_and_get<mn::QuestProgress>(buf);
    if (t == nullptr) return std::nullopt;
    QuestProgress out;
    out.quest_id = t->quest_id();
    out.objective_index = t->objective_index();
    out.type = static_cast<std::uint16_t>(t->type());
    out.have = t->have();
    out.need = t->need();
    out.complete = t->complete();
    return out;
}

Bytes encode_quest_turn_in(const QuestTurnIn& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateQuestTurnIn(b, in.quest_id, in.turn_in_guid, in.choice_index));
    return to_bytes(b);
}

std::optional<QuestTurnIn> decode_quest_turn_in(const Bytes& buf) {
    const mn::QuestTurnIn* t = verify_and_get<mn::QuestTurnIn>(buf);
    if (t == nullptr) return std::nullopt;
    QuestTurnIn out;
    out.quest_id = t->quest_id();
    out.turn_in_guid = t->turn_in_guid();
    out.choice_index = t->choice_index();
    return out;
}

Bytes encode_quest_turn_in_result(const QuestTurnInResult& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::QuestRewardItem>> items;
    items.reserve(in.reward_items.size());
    for (const auto& it : in.reward_items) {
        items.push_back(mn::CreateQuestRewardItem(b, it.item_id, it.count));
    }
    b.Finish(mn::CreateQuestTurnInResult(
        b, in.quest_id, static_cast<mn::QuestTurnInStatus>(in.status), in.reward_xp,
        in.reward_money, b.CreateVector(items), in.new_level));
    return to_bytes(b);
}

std::optional<QuestTurnInResult> decode_quest_turn_in_result(const Bytes& buf) {
    const mn::QuestTurnInResult* t = verify_and_get<mn::QuestTurnInResult>(buf);
    if (t == nullptr) return std::nullopt;
    QuestTurnInResult out;
    out.quest_id = t->quest_id();
    out.status = static_cast<std::uint16_t>(t->status());
    out.reward_xp = t->reward_xp();
    out.reward_money = t->reward_money();
    out.new_level = t->new_level();
    if (t->reward_items() != nullptr) {
        out.reward_items.reserve(t->reward_items()->size());
        for (const auto* it : *t->reward_items()) {
            if (it == nullptr) continue;
            out.reward_items.push_back(QuestRewardItem{it->item_id(), it->count()});
        }
    }
    return out;
}

Bytes encode_quest_log(const QuestLog& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::QuestLogEntry>> entries;
    entries.reserve(in.quests.size());
    for (const auto& q : in.quests) {
        std::vector<fb::Offset<mn::QuestObjectiveState>> objs;
        objs.reserve(q.objectives.size());
        for (const auto& o : q.objectives) {
            objs.push_back(mn::CreateQuestObjectiveState(
                b, static_cast<mn::QuestObjectiveType>(o.type), o.target_id, o.have,
                o.need, o.complete));
        }
        entries.push_back(mn::CreateQuestLogEntry(b, q.quest_id, q.level, q.complete,
                                                  b.CreateVector(objs)));
    }
    b.Finish(mn::CreateQuestLog(b, b.CreateVector(entries)));
    return to_bytes(b);
}

std::optional<QuestLog> decode_quest_log(const Bytes& buf) {
    const mn::QuestLog* t = verify_and_get<mn::QuestLog>(buf);
    if (t == nullptr) return std::nullopt;
    QuestLog out;
    if (t->quests() != nullptr) {
        out.quests.reserve(t->quests()->size());
        for (const auto* q : *t->quests()) {
            if (q == nullptr) continue;
            QuestLogEntry entry;
            entry.quest_id = q->quest_id();
            entry.level = q->level();
            entry.complete = q->complete();
            if (q->objectives() != nullptr) {
                entry.objectives.reserve(q->objectives()->size());
                for (const auto* o : *q->objectives()) {
                    if (o == nullptr) continue;
                    entry.objectives.push_back(QuestObjectiveState{
                        static_cast<std::uint16_t>(o->type()), o->target_id(), o->have(),
                        o->need(), o->complete()});
                }
            }
            out.quests.push_back(std::move(entry));
        }
    }
    return out;
}

}  // namespace meridian::clientnet::codec
