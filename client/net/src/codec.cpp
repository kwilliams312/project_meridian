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

// ---- IF-2 PROGRESSION: XpGained / LevelUp (CHR-03, #531) -------------------

Bytes encode_xp_gained(const XpGained& in) {
    fb::FlatBufferBuilder b;
    auto x = mn::CreateXpGained(b, in.player_guid, in.xp_gained, in.level, in.xp_total,
                                in.xp_to_next);
    b.Finish(x);
    return to_bytes(b);
}

std::optional<XpGained> decode_xp_gained(const Bytes& buf) {
    const mn::XpGained* t = verify_and_get<mn::XpGained>(buf);
    if (t == nullptr) return std::nullopt;
    XpGained out;
    out.player_guid = t->player_guid();
    out.xp_gained = t->xp_gained();
    out.level = t->level();
    out.xp_total = t->xp_total();
    out.xp_to_next = t->xp_to_next();
    return out;
}

Bytes encode_level_up(const LevelUp& in) {
    fb::FlatBufferBuilder b;
    auto l = mn::CreateLevelUp(b, in.player_guid, in.old_level, in.new_level, in.max_health,
                               in.max_resource);
    b.Finish(l);
    return to_bytes(b);
}

std::optional<LevelUp> decode_level_up(const Bytes& buf) {
    const mn::LevelUp* t = verify_and_get<mn::LevelUp>(buf);
    if (t == nullptr) return std::nullopt;
    LevelUp out;
    out.player_guid = t->player_guid();
    out.old_level = t->old_level();
    out.new_level = t->new_level();
    out.max_health = t->max_health();
    out.max_resource = t->max_resource();
    return out;
}

// ---- IF-2 COMBAT: CastRequest / CastStart / CastFailed / CastResult (CMB-01, D-10, #432) ----

Bytes encode_cast_request(const CastRequest& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCastRequest(b, in.ability_id, in.target_guid, in.client_time_ms));
    return to_bytes(b);
}

std::optional<CastRequest> decode_cast_request(const Bytes& buf) {
    const mn::CastRequest* t = verify_and_get<mn::CastRequest>(buf);
    if (t == nullptr) return std::nullopt;
    CastRequest out;
    out.ability_id = t->ability_id();
    out.target_guid = t->target_guid();
    out.client_time_ms = t->client_time_ms();
    return out;
}

Bytes encode_cast_start(const CastStart& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCastStart(b, in.ability_id, in.cast_ms, in.server_time_ms));
    return to_bytes(b);
}

std::optional<CastStart> decode_cast_start(const Bytes& buf) {
    const mn::CastStart* t = verify_and_get<mn::CastStart>(buf);
    if (t == nullptr) return std::nullopt;
    CastStart out;
    out.ability_id = t->ability_id();
    out.cast_ms = t->cast_ms();
    out.server_time_ms = t->server_time_ms();
    return out;
}

Bytes encode_cast_failed(const CastFailed& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCastFailed(b, in.ability_id,
                                  static_cast<mn::CastFailReason>(in.reason),
                                  in.gcd_remaining_ms));
    return to_bytes(b);
}

std::optional<CastFailed> decode_cast_failed(const Bytes& buf) {
    const mn::CastFailed* t = verify_and_get<mn::CastFailed>(buf);
    if (t == nullptr) return std::nullopt;
    CastFailed out;
    out.ability_id = t->ability_id();
    out.reason = static_cast<std::uint16_t>(t->reason());
    out.gcd_remaining_ms = t->gcd_remaining_ms();
    return out;
}

Bytes encode_cast_result(const CastResult& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateCastResult(b, in.ability_id, in.caster_guid, in.target_guid,
                                  static_cast<mn::AttackOutcome>(in.outcome), in.amount,
                                  in.is_heal, in.target_health, in.target_dead,
                                  in.server_time_ms));
    return to_bytes(b);
}

std::optional<CastResult> decode_cast_result(const Bytes& buf) {
    const mn::CastResult* t = verify_and_get<mn::CastResult>(buf);
    if (t == nullptr) return std::nullopt;
    CastResult out;
    out.ability_id = t->ability_id();
    out.caster_guid = t->caster_guid();
    out.target_guid = t->target_guid();
    out.outcome = static_cast<std::uint16_t>(t->outcome());
    out.amount = t->amount();
    out.is_heal = t->is_heal();
    out.target_health = t->target_health();
    out.target_dead = t->target_dead();
    out.server_time_ms = t->server_time_ms();
    return out;
}

// ---- IF-2 KNOWN_ABILITIES (0x3005 — CMB-01, #457/#456) ---------------------

Bytes encode_known_abilities(const KnownAbilities& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::KnownAbility>> abl;
    abl.reserve(in.abilities.size());
    for (const auto& a : in.abilities) {
        abl.push_back(mn::CreateKnownAbility(
            b, a.ability_id, a.cast_ms, a.triggers_gcd,
            static_cast<mn::AbilityResource>(a.resource_type), a.resource_cost, a.range_m));
    }
    b.Finish(mn::CreateKnownAbilities(b, b.CreateVector(abl)));
    return to_bytes(b);
}

std::optional<KnownAbilities> decode_known_abilities(const Bytes& buf) {
    const mn::KnownAbilities* t = verify_and_get<mn::KnownAbilities>(buf);
    if (t == nullptr) return std::nullopt;
    KnownAbilities out;
    if (t->abilities() != nullptr) {
        out.abilities.reserve(t->abilities()->size());
        for (const auto* a : *t->abilities()) {
            if (a == nullptr) continue;
            out.abilities.push_back(KnownAbility{
                a->ability_id(), a->cast_ms(), a->triggers_gcd(),
                static_cast<std::uint8_t>(a->resource_type()), a->resource_cost(),
                a->range_m()});
        }
    }
    return out;
}

// ---- IF-2 DEATH / GHOST / RESURRECT (0x3010-0x3014 — CMB-03, #359/#532) -----

Bytes encode_death_state(const DeathState& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateDeathState(b, in.victim_guid, in.killer_guid, in.corpse_guid,
                                  in.corpse_x, in.corpse_y, in.corpse_z, in.auto_release_ms));
    return to_bytes(b);
}

std::optional<DeathState> decode_death_state(const Bytes& buf) {
    const mn::DeathState* t = verify_and_get<mn::DeathState>(buf);
    if (t == nullptr) return std::nullopt;
    DeathState out;
    out.victim_guid = t->victim_guid();
    out.killer_guid = t->killer_guid();
    out.corpse_guid = t->corpse_guid();
    out.corpse_x = t->corpse_x();
    out.corpse_y = t->corpse_y();
    out.corpse_z = t->corpse_z();
    out.auto_release_ms = t->auto_release_ms();
    return out;
}

Bytes encode_ghost_state(const GhostState& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateGhostState(b, in.player_guid, in.graveyard_x, in.graveyard_y,
                                  in.graveyard_z, in.corpse_guid));
    return to_bytes(b);
}

std::optional<GhostState> decode_ghost_state(const Bytes& buf) {
    const mn::GhostState* t = verify_and_get<mn::GhostState>(buf);
    if (t == nullptr) return std::nullopt;
    GhostState out;
    out.player_guid = t->player_guid();
    out.graveyard_x = t->graveyard_x();
    out.graveyard_y = t->graveyard_y();
    out.graveyard_z = t->graveyard_z();
    out.corpse_guid = t->corpse_guid();
    return out;
}

Bytes encode_resurrect_result(const ResurrectResult& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateResurrectResult(b, in.player_guid,
                                       static_cast<mn::ResurrectStatus>(in.status),
                                       in.health, in.max_health));
    return to_bytes(b);
}

std::optional<ResurrectResult> decode_resurrect_result(const Bytes& buf) {
    const mn::ResurrectResult* t = verify_and_get<mn::ResurrectResult>(buf);
    if (t == nullptr) return std::nullopt;
    ResurrectResult out;
    out.player_guid = t->player_guid();
    out.status = static_cast<std::uint16_t>(t->status());
    out.health = t->health();
    out.max_health = t->max_health();
    return out;
}

Bytes encode_release_request() {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateReleaseRequest(b));
    return to_bytes(b);
}

std::optional<bool> decode_release_request(const Bytes& buf) {
    if (verify_and_get<mn::ReleaseRequest>(buf) == nullptr) return std::nullopt;
    return true;
}

Bytes encode_resurrect_request() {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateResurrectRequest(b));
    return to_bytes(b);
}

std::optional<bool> decode_resurrect_request(const Bytes& buf) {
    if (verify_and_get<mn::ResurrectRequest>(buf) == nullptr) return std::nullopt;
    return true;
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
    out.status = static_cast<std::uint16_t>(t->status());  // OK default (#479)
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
    // The chosen appearance (CHR-01 #435). Nested table must be built before the
    // parent table is created (FlatBuffers requirement). morphs stay empty at M1.
    auto appearance = mn::CreateAppearance(
        b, in.appearance.version, in.appearance.hair, in.appearance.face,
        in.appearance.skin);
    b.Finish(mn::CreateCharCreateRequest(
        b, name, in.race, in.char_class, appearance));
    return to_bytes(b);
}

std::optional<CharCreateRequest> decode_char_create_request(const Bytes& buf) {
    const mn::CharCreateRequest* t = verify_and_get<mn::CharCreateRequest>(buf);
    if (t == nullptr) return std::nullopt;
    CharCreateRequest out;
    if (t->name() != nullptr) out.name = t->name()->str();
    out.race = t->race();
    out.char_class = t->char_class();
    if (const mn::Appearance* a = t->appearance()) {
        out.appearance.version = a->version();
        out.appearance.hair = a->hair();
        out.appearance.face = a->face();
        out.appearance.skin = a->skin();
    }
    return out;
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
    b.Finish(mn::CreateCharListResponse(b, b.CreateVector(rows),
                                        static_cast<mn::CharListStatus>(in.status)));
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
        std::vector<fb::Offset<mn::QuestRewardItem>> rewards;
        rewards.reserve(q.reward_items.size());
        for (const auto& r : q.reward_items) {
            rewards.push_back(mn::CreateQuestRewardItem(b, r.item_id, r.count));
        }
        std::vector<fb::Offset<mn::QuestRewardItem>> choices;
        choices.reserve(q.choice_items.size());
        for (const auto& c : q.choice_items) {
            choices.push_back(mn::CreateQuestRewardItem(b, c.item_id, c.count));
        }
        entries.push_back(mn::CreateQuestLogEntry(
            b, q.quest_id, q.level, q.complete, b.CreateVector(objs), q.reward_xp,
            q.reward_money, b.CreateVector(rewards), b.CreateVector(choices)));
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
            entry.reward_xp = q->reward_xp();
            entry.reward_money = q->reward_money();
            if (q->reward_items() != nullptr) {
                entry.reward_items.reserve(q->reward_items()->size());
                for (const auto* r : *q->reward_items()) {
                    if (r == nullptr) continue;
                    entry.reward_items.push_back(QuestRewardItem{r->item_id(), r->count()});
                }
            }
            if (q->choice_items() != nullptr) {
                entry.choice_items.reserve(q->choice_items()->size());
                for (const auto* c : *q->choice_items()) {
                    if (c == nullptr) continue;
                    entry.choice_items.push_back(QuestRewardItem{c->item_id(), c->count()});
                }
            }
            out.quests.push_back(std::move(entry));
        }
    }
    return out;
}

// ---- IF-2 LOOT (0x5001..0x5006 — ITM-02, #369/#441) ------------------------

Bytes encode_loot_request(const LootRequest& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootRequest(b, in.corpse_guid));
    return to_bytes(b);
}

std::optional<LootRequest> decode_loot_request(const Bytes& buf) {
    const mn::LootRequest* t = verify_and_get<mn::LootRequest>(buf);
    if (t == nullptr) return std::nullopt;
    LootRequest out;
    out.corpse_guid = t->corpse_guid();
    return out;
}

Bytes encode_loot_response(const LootResponse& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::LootItem>> items;
    items.reserve(in.items.size());
    for (const auto& it : in.items) {
        items.push_back(mn::CreateLootItem(b, it.slot, it.item_template_id, it.count,
                                           it.quality, it.quest_item));
    }
    b.Finish(mn::CreateLootResponse(b, in.corpse_guid,
                                    static_cast<mn::LootStatus>(in.status), in.copper,
                                    b.CreateVector(items)));
    return to_bytes(b);
}

std::optional<LootResponse> decode_loot_response(const Bytes& buf) {
    const mn::LootResponse* t = verify_and_get<mn::LootResponse>(buf);
    if (t == nullptr) return std::nullopt;
    LootResponse out;
    out.corpse_guid = t->corpse_guid();
    out.status = static_cast<std::uint16_t>(t->status());
    out.copper = t->copper();
    if (t->items() != nullptr) {
        out.items.reserve(t->items()->size());
        for (const auto* it : *t->items()) {
            if (it == nullptr) continue;
            out.items.push_back(LootItem{it->slot(), it->item_template_id(), it->count(),
                                         it->quality(), it->quest_item()});
        }
    }
    return out;
}

Bytes encode_loot_take(const LootTake& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootTake(b, in.corpse_guid, in.slot, in.money));
    return to_bytes(b);
}

std::optional<LootTake> decode_loot_take(const Bytes& buf) {
    const mn::LootTake* t = verify_and_get<mn::LootTake>(buf);
    if (t == nullptr) return std::nullopt;
    LootTake out;
    out.corpse_guid = t->corpse_guid();
    out.slot = t->slot();
    out.money = t->money();
    return out;
}

Bytes encode_loot_result(const LootResult& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootResult(b, in.corpse_guid, in.slot,
                                  static_cast<mn::LootTakeStatus>(in.status),
                                  in.item_template_id, in.count, in.copper));
    return to_bytes(b);
}

std::optional<LootResult> decode_loot_result(const Bytes& buf) {
    const mn::LootResult* t = verify_and_get<mn::LootResult>(buf);
    if (t == nullptr) return std::nullopt;
    LootResult out;
    out.corpse_guid = t->corpse_guid();
    out.slot = t->slot();
    out.status = static_cast<std::uint16_t>(t->status());
    out.item_template_id = t->item_template_id();
    out.count = t->count();
    out.copper = t->copper();
    return out;
}

Bytes encode_loot_release(const LootRelease& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootRelease(b, in.corpse_guid));
    return to_bytes(b);
}

std::optional<LootRelease> decode_loot_release(const Bytes& buf) {
    const mn::LootRelease* t = verify_and_get<mn::LootRelease>(buf);
    if (t == nullptr) return std::nullopt;
    LootRelease out;
    out.corpse_guid = t->corpse_guid();
    return out;
}

Bytes encode_loot_closed(const LootClosed& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateLootClosed(b, in.corpse_guid));
    return to_bytes(b);
}

std::optional<LootClosed> decode_loot_closed(const Bytes& buf) {
    const mn::LootClosed* t = verify_and_get<mn::LootClosed>(buf);
    if (t == nullptr) return std::nullopt;
    LootClosed out;
    out.corpse_guid = t->corpse_guid();
    return out;
}

// ---- IF-2 INVENTORY SNAPSHOT (0x5007 — ITM-01, #453/#471) ------------------

Bytes encode_inventory_snapshot(const InventorySnapshot& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::InventoryItem>> items;
    items.reserve(in.items.size());
    for (const auto& it : in.items) {
        items.push_back(mn::CreateInventoryItem(b, it.slot, it.item_template_id, it.count,
                                                it.quality, it.binding));
    }
    b.Finish(mn::CreateInventorySnapshot(b, in.money, b.CreateVector(items),
                                         in.backpack_slots));
    return to_bytes(b);
}

std::optional<InventorySnapshot> decode_inventory_snapshot(const Bytes& buf) {
    const mn::InventorySnapshot* t = verify_and_get<mn::InventorySnapshot>(buf);
    if (t == nullptr) return std::nullopt;
    InventorySnapshot out;
    out.money = t->money();
    out.backpack_slots = t->backpack_slots();
    if (t->items() != nullptr) {
        out.items.reserve(t->items()->size());
        for (const auto* it : *t->items()) {
            if (it == nullptr) continue;
            out.items.push_back(InventoryItem{it->slot(), it->item_template_id(),
                                              it->count(), it->quality(), it->binding()});
        }
    }
    return out;
}

// ---- IF-2 VENDOR (0x5101..0x5106 — ECO-01, #370/#441) ----------------------

Bytes encode_vendor_buy_request(const VendorBuyRequest& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuyRequest(b, in.vendor_id, in.item_template_id, in.quantity));
    return to_bytes(b);
}

std::optional<VendorBuyRequest> decode_vendor_buy_request(const Bytes& buf) {
    const mn::VendorBuyRequest* t = verify_and_get<mn::VendorBuyRequest>(buf);
    if (t == nullptr) return std::nullopt;
    VendorBuyRequest out;
    out.vendor_id = t->vendor_id();
    out.item_template_id = t->item_template_id();
    out.quantity = t->quantity();
    return out;
}

Bytes encode_vendor_buy_result(const VendorBuyResult& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuyResult(b, static_cast<mn::VendorBuyStatus>(in.status),
                                       in.vendor_id, in.item_template_id, in.quantity,
                                       in.item_guid, in.total_price, in.balance));
    return to_bytes(b);
}

std::optional<VendorBuyResult> decode_vendor_buy_result(const Bytes& buf) {
    const mn::VendorBuyResult* t = verify_and_get<mn::VendorBuyResult>(buf);
    if (t == nullptr) return std::nullopt;
    VendorBuyResult out;
    out.status = static_cast<std::uint16_t>(t->status());
    out.vendor_id = t->vendor_id();
    out.item_template_id = t->item_template_id();
    out.quantity = t->quantity();
    out.item_guid = t->item_guid();
    out.total_price = t->total_price();
    out.balance = t->balance();
    return out;
}

Bytes encode_vendor_sell_request(const VendorSellRequest& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorSellRequest(b, in.vendor_id, in.backpack_slot, in.quantity));
    return to_bytes(b);
}

std::optional<VendorSellRequest> decode_vendor_sell_request(const Bytes& buf) {
    const mn::VendorSellRequest* t = verify_and_get<mn::VendorSellRequest>(buf);
    if (t == nullptr) return std::nullopt;
    VendorSellRequest out;
    out.vendor_id = t->vendor_id();
    out.backpack_slot = t->backpack_slot();
    out.quantity = t->quantity();
    return out;
}

Bytes encode_vendor_sell_result(const VendorSellResult& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorSellResult(b, static_cast<mn::VendorSellStatus>(in.status),
                                        in.backpack_slot, in.item_template_id, in.quantity,
                                        in.total_credit, in.balance, in.buyback_slot));
    return to_bytes(b);
}

std::optional<VendorSellResult> decode_vendor_sell_result(const Bytes& buf) {
    const mn::VendorSellResult* t = verify_and_get<mn::VendorSellResult>(buf);
    if (t == nullptr) return std::nullopt;
    VendorSellResult out;
    out.status = static_cast<std::uint16_t>(t->status());
    out.backpack_slot = t->backpack_slot();
    out.item_template_id = t->item_template_id();
    out.quantity = t->quantity();
    out.total_credit = t->total_credit();
    out.balance = t->balance();
    out.buyback_slot = t->buyback_slot();
    return out;
}

Bytes encode_vendor_buyback_request(const VendorBuybackRequest& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuybackRequest(b, in.buyback_slot));
    return to_bytes(b);
}

std::optional<VendorBuybackRequest> decode_vendor_buyback_request(const Bytes& buf) {
    const mn::VendorBuybackRequest* t = verify_and_get<mn::VendorBuybackRequest>(buf);
    if (t == nullptr) return std::nullopt;
    VendorBuybackRequest out;
    out.buyback_slot = t->buyback_slot();
    return out;
}

Bytes encode_vendor_buyback_result(const VendorBuybackResult& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateVendorBuybackResult(
        b, static_cast<mn::VendorBuybackStatus>(in.status), in.item_template_id,
        in.quantity, in.item_guid, in.price, in.balance, in.buyback_slot));
    return to_bytes(b);
}

std::optional<VendorBuybackResult> decode_vendor_buyback_result(const Bytes& buf) {
    const mn::VendorBuybackResult* t = verify_and_get<mn::VendorBuybackResult>(buf);
    if (t == nullptr) return std::nullopt;
    VendorBuybackResult out;
    out.status = static_cast<std::uint16_t>(t->status());
    out.item_template_id = t->item_template_id();
    out.quantity = t->quantity();
    out.item_guid = t->item_guid();
    out.price = t->price();
    out.balance = t->balance();
    out.buyback_slot = t->buyback_slot();
    return out;
}

// ---- IF-2 VENDOR CATALOG (0x5107 — ECO-01, #453/#471) ----------------------

Bytes encode_vendor_list(const VendorList& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::VendorItem>> items;
    items.reserve(in.items.size());
    for (const auto& it : in.items) {
        items.push_back(mn::CreateVendorItem(b, it.item_template_id, it.price, it.quality,
                                             it.stock));
    }
    b.Finish(mn::CreateVendorList(b, in.vendor_id, b.CreateVector(items)));
    return to_bytes(b);
}

std::optional<VendorList> decode_vendor_list(const Bytes& buf) {
    const mn::VendorList* t = verify_and_get<mn::VendorList>(buf);
    if (t == nullptr) return std::nullopt;
    VendorList out;
    out.vendor_id = t->vendor_id();
    if (t->items() != nullptr) {
        out.items.reserve(t->items()->size());
        for (const auto* it : *t->items()) {
            if (it == nullptr) continue;
            out.items.push_back(VendorItem{it->item_template_id(), it->price(),
                                           it->quality(), it->stock()});
        }
    }
    return out;
}

// ---- IF-2 TRAINER (0x5203..0x5205 — NPC-02, #372/#441) ---------------------

Bytes encode_trainer_list(const TrainerList& in) {
    fb::FlatBufferBuilder b;
    std::vector<fb::Offset<mn::TrainerListEntry>> entries;
    entries.reserve(in.entries.size());
    for (const auto& e : in.entries) {
        entries.push_back(mn::CreateTrainerListEntry(
            b, e.ability_id, e.cost, e.required_class, e.required_level,
            static_cast<mn::TrainableState>(e.state)));
    }
    b.Finish(mn::CreateTrainerList(b, in.npc_guid, b.CreateVector(entries)));
    return to_bytes(b);
}

std::optional<TrainerList> decode_trainer_list(const Bytes& buf) {
    const mn::TrainerList* t = verify_and_get<mn::TrainerList>(buf);
    if (t == nullptr) return std::nullopt;
    TrainerList out;
    out.npc_guid = t->npc_guid();
    if (t->entries() != nullptr) {
        out.entries.reserve(t->entries()->size());
        for (const auto* e : *t->entries()) {
            if (e == nullptr) continue;
            out.entries.push_back(TrainerListEntry{
                e->ability_id(), e->cost(), e->required_class(), e->required_level(),
                static_cast<std::uint16_t>(e->state())});
        }
    }
    return out;
}

Bytes encode_trainer_learn(const TrainerLearn& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateTrainerLearn(b, in.npc_guid, in.ability_id));
    return to_bytes(b);
}

std::optional<TrainerLearn> decode_trainer_learn(const Bytes& buf) {
    const mn::TrainerLearn* t = verify_and_get<mn::TrainerLearn>(buf);
    if (t == nullptr) return std::nullopt;
    TrainerLearn out;
    out.npc_guid = t->npc_guid();
    out.ability_id = t->ability_id();
    return out;
}

Bytes encode_trainer_learn_result(const TrainerLearnResult& in) {
    fb::FlatBufferBuilder b;
    b.Finish(mn::CreateTrainerLearnResult(
        b, in.npc_guid, in.ability_id,
        static_cast<mn::TrainerLearnStatus>(in.status), in.cost, in.new_balance));
    return to_bytes(b);
}

std::optional<TrainerLearnResult> decode_trainer_learn_result(const Bytes& buf) {
    const mn::TrainerLearnResult* t = verify_and_get<mn::TrainerLearnResult>(buf);
    if (t == nullptr) return std::nullopt;
    TrainerLearnResult out;
    out.npc_guid = t->npc_guid();
    out.ability_id = t->ability_id();
    out.status = static_cast<std::uint16_t>(t->status());
    out.cost = t->cost();
    out.new_balance = t->new_balance();
    return out;
}

// ---- IF-2 CHAT / SOCIAL (SOC-01, #367/#434) --------------------------------
// Strings are created BEFORE the table (FlatBuffers rule); an empty body/target
// round-trips as an empty string. The generated field order matches world.fbs:
// ChatMessage{channel,target,text}, ChatDeliver{channel,sender_guid,sender_name,text},
// ChatRejected{channel,reason,target}.

Bytes encode_chat_message(const ChatMessage& in) {
    fb::FlatBufferBuilder b;
    auto target = b.CreateString(in.target);
    auto text = b.CreateString(in.text);
    b.Finish(mn::CreateChatMessage(
        b, static_cast<mn::ChatChannel>(in.channel), target, text));
    return to_bytes(b);
}

std::optional<ChatMessage> decode_chat_message(const Bytes& buf) {
    const mn::ChatMessage* t = verify_and_get<mn::ChatMessage>(buf);
    if (t == nullptr) return std::nullopt;
    ChatMessage out;
    out.channel = static_cast<std::uint16_t>(t->channel());
    out.target = t->target() ? t->target()->str() : std::string();
    out.text = t->text() ? t->text()->str() : std::string();
    return out;
}

Bytes encode_chat_deliver(const ChatDeliver& in) {
    fb::FlatBufferBuilder b;
    auto name = b.CreateString(in.sender_name);
    auto text = b.CreateString(in.text);
    b.Finish(mn::CreateChatDeliver(
        b, static_cast<mn::ChatChannel>(in.channel), in.sender_guid, name, text));
    return to_bytes(b);
}

std::optional<ChatDeliver> decode_chat_deliver(const Bytes& buf) {
    const mn::ChatDeliver* t = verify_and_get<mn::ChatDeliver>(buf);
    if (t == nullptr) return std::nullopt;
    ChatDeliver out;
    out.channel = static_cast<std::uint16_t>(t->channel());
    out.sender_guid = t->sender_guid();
    out.sender_name = t->sender_name() ? t->sender_name()->str() : std::string();
    out.text = t->text() ? t->text()->str() : std::string();
    return out;
}

Bytes encode_chat_rejected(const ChatRejected& in) {
    fb::FlatBufferBuilder b;
    auto target = b.CreateString(in.target);
    b.Finish(mn::CreateChatRejected(
        b, static_cast<mn::ChatChannel>(in.channel),
        static_cast<mn::ChatRejectReason>(in.reason), target));
    return to_bytes(b);
}

std::optional<ChatRejected> decode_chat_rejected(const Bytes& buf) {
    const mn::ChatRejected* t = verify_and_get<mn::ChatRejected>(buf);
    if (t == nullptr) return std::nullopt;
    ChatRejected out;
    out.channel = static_cast<std::uint16_t>(t->channel());
    out.reason = static_cast<std::uint16_t>(t->reason());
    out.target = t->target() ? t->target()->str() : std::string();
    return out;
}

}  // namespace meridian::clientnet::codec
