// SPDX-License-Identifier: Apache-2.0
//
// worldd — shared world state + AoI movement relay implementation (issue #87).
// See world_state.h for the clean-room provenance, the relay design, the egress
// seam, and the threading model.

#include "world_state.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "meridian/core/log.hpp"
#include "meridian/net/tls_listener.h"

#include "world_dispatch.h"  // encode_frame

namespace meridian::worldd {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace log = meridian::core::log;

constexpr const char* kCat = "worldd.aoi";

// ASCII lower-case a name for the case-insensitive whisper index (#367). Names
// are the M0-frozen roster's ASCII set (server/characters roster), so a byte-wise
// tolower is sufficient — no locale/Unicode folding is in scope at M1.
std::string to_lower_ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// SessionEgress — serialized, WorldSession-sealed s2c writes.
// ---------------------------------------------------------------------------

bool SessionEgress::emit(net::Opcode opcode, const std::vector<std::uint8_t>& payload) {
    std::lock_guard<std::mutex> lk(write_mtx_);
    if (!alive_ || sess_ == nullptr || session_ == nullptr) return false;

    // Route through the established WorldSession s2c channel (SAD §5.2): seal()
    // advances the s2c nonce counter and returns the seq this frame is bound
    // under. The seq goes in the IF-2 header. At M0 the frame BODY is the
    // plaintext FlatBuffer (TLS gives confidentiality; the AEAD wrap is the
    // documented seam — swap `payload` for `sealed` here and flip the client to
    // open() to activate it). We still call seal() so the s2c counter + the seam
    // are already per-subscriber correct.
    std::uint64_t s2c_seq = 0;
    try {
        // aad binds the opcode header into the tag when the wrap activates; empty
        // at M0 (no wrap on the wire yet). Discard the sealed bytes for now — the
        // call's purpose today is to allocate + advance the authoritative s2c seq.
        const std::vector<std::uint8_t> aad;
        (void)session_->seal(Direction::kServerToClient, payload, aad, s2c_seq);
    } catch (const net::TlsError& e) {
        log::warn(kCat, std::string("s2c seal failed: ") + e.what());
        return false;
    }

    try {
        sess_->write_frame(encode_frame(opcode, s2c_seq, payload));
    } catch (const net::TlsError&) {
        return false;  // peer gone
    }
    return true;
}

bool SessionEgress::emit_frame(const std::vector<std::uint8_t>& frame) {
    std::lock_guard<std::mutex> lk(write_mtx_);
    if (!alive_ || sess_ == nullptr) return false;
    try {
        sess_->write_frame(frame);
    } catch (const net::TlsError&) {
        return false;
    }
    return true;
}

void SessionEgress::mark_closed() {
    std::lock_guard<std::mutex> lk(write_mtx_);
    alive_ = false;
}

// ---------------------------------------------------------------------------
// Egress payload builders (world.fbs tables)
// ---------------------------------------------------------------------------

// Project the server unit model's ResourceType (combat_unit.h) onto the wire
// PowerType (world.fbs). The two enums MIRROR each other 1:1 (kNone/kMana/kEnergy/
// kRage == NONE/MANA/ENERGY/RAGE), so this is a straight value map — kept explicit
// (not a raw cast) so a future divergence fails loudly at the switch.
mn::PowerType wire_power_type(ResourceType rt) {
    switch (rt) {
        case ResourceType::kMana: return mn::PowerType::MANA;
        case ResourceType::kEnergy: return mn::PowerType::ENERGY;
        case ResourceType::kRage: return mn::PowerType::RAGE;
        case ResourceType::kNone: break;
    }
    return mn::PowerType::NONE;
}

std::vector<std::uint8_t> encode_entity_enter_payload(const EntityIdentity& subject,
                                                      const Unit& unit) {
    fb::FlatBufferBuilder b;
    const Position& pos = unit.position();
    // attrs empty at M0 (D-11 placeholder carries no wire attributes yet); the
    // vector is created so the table shape is complete for the client verifier.
    auto attrs = b.CreateVector(std::vector<fb::Offset<mn::AttrDelta>>{});
    // name (#430): the character/creature display name for the client unit frame.
    // Empty for the D-11 placeholder (no characters DB).
    auto name = b.CreateString(subject.name);
    // char_class (#328): the mover's M0-frozen class id, so every client renders the
    // placeholder capsule in a class-derived color. Authoritative here on the server.
    // vitals (#430): health/max, power/max + type, level — the HUD contract, read
    // straight from the authoritative Unit (SAD §2.5). Server-authoritative.
    auto e = mn::CreateEntityEnter(b, subject.entity_guid, subject.type_id, pos.x, pos.y,
                                   pos.z, pos.orientation, attrs, subject.char_class,
                                   unit.health(), unit.max_health(), unit.resource(),
                                   unit.max_resource(), wire_power_type(unit.resource_type()),
                                   unit.level(), name);
    b.Finish(e);
    return std::vector<std::uint8_t>(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

std::vector<std::uint8_t> encode_vitals_update_payload(AoiId subject_guid, const Unit& unit) {
    fb::FlatBufferBuilder b;
    auto v = mn::CreateVitalsUpdate(b, subject_guid, unit.health(), unit.max_health(),
                                    unit.resource(), unit.max_resource(),
                                    wire_power_type(unit.resource_type()), unit.level());
    b.Finish(v);
    return std::vector<std::uint8_t>(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

std::vector<std::uint8_t> encode_entity_update_payload(AoiId subject_guid,
                                                       const Position& pos) {
    fb::FlatBufferBuilder b;
    auto attrs = b.CreateVector(std::vector<fb::Offset<mn::AttrDelta>>{});
    // Position fields present (this is a movement delta). world.fbs makes them
    // optional (default null); we always send them for a move so the observer
    // gets the new position.
    auto u = mn::CreateEntityUpdate(b, subject_guid, pos.x, pos.y, pos.z, pos.orientation,
                                    attrs);
    b.Finish(u);
    return std::vector<std::uint8_t>(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

std::vector<std::uint8_t> encode_entity_leave_payload(AoiId subject_guid,
                                                      net::LeaveReason reason) {
    fb::FlatBufferBuilder b;
    auto l = mn::CreateEntityLeave(b, subject_guid, reason);
    b.Finish(l);
    return std::vector<std::uint8_t>(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

std::vector<std::uint8_t> encode_poi_discovered_payload(TriggerId trigger_id,
                                                        std::uint32_t area_id,
                                                        std::uint32_t name_id) {
    fb::FlatBufferBuilder b;
    auto d = mn::CreatePoiDiscovered(b, trigger_id, area_id, name_id);
    b.Finish(d);
    return std::vector<std::uint8_t>(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

std::vector<std::uint8_t> encode_chat_deliver_payload(net::ChatChannel channel,
                                                      AoiId sender_guid,
                                                      const std::string& sender_name,
                                                      const std::string& text) {
    fb::FlatBufferBuilder b;
    auto name = b.CreateString(sender_name);
    auto body = b.CreateString(text);
    auto d = mn::CreateChatDeliver(b, channel, sender_guid, name, body);
    b.Finish(d);
    return std::vector<std::uint8_t>(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// ---------------------------------------------------------------------------
// ChatIntake — per-connection chat rate gate (OPS-03; #367)
// ---------------------------------------------------------------------------

bool ChatIntake::admit(std::uint64_t now_ms) {
    // Expire stamps older than the trailing 1000 ms window (front is oldest).
    while (!window_.empty() && now_ms - window_.front() >= 1000) {
        window_.pop_front();
    }
    if (window_.size() >= static_cast<std::size_t>(kChatMaxPerSecond)) {
        ++dropped_;
        return false;
    }
    window_.push_back(now_ms);
    ++admitted_;
    return true;
}

// ---------------------------------------------------------------------------
// WorldState
// ---------------------------------------------------------------------------

std::optional<SessionSlot> WorldState::slot_of_guid(AoiId guid) const {
    auto it = slot_by_guid_.find(guid);
    if (it == slot_by_guid_.end()) return std::nullopt;
    return it->second;
}

void WorldState::send_enter(SessionRec& to, const SessionRec& subject) {
    if (!to.egress) return;
    to.egress(mn::Opcode::ENTITY_ENTER,
              encode_entity_enter_payload(subject.identity, subject.unit));
}

void WorldState::send_update(SessionRec& to, const SessionRec& subject,
                             std::uint32_t /*ack_seq*/, std::uint64_t /*server_time_ms*/) {
    if (!to.egress) return;
    to.egress(mn::Opcode::ENTITY_UPDATE,
              encode_entity_update_payload(subject.identity.entity_guid,
                                           subject.unit.position()));
}

void WorldState::send_leave(SessionRec& to, const SessionRec& subject,
                            net::LeaveReason reason) {
    if (!to.egress) return;
    to.egress(mn::Opcode::ENTITY_LEAVE,
              encode_entity_leave_payload(subject.identity.entity_guid, reason));
}

// --- static world entity relay (content spawns, #486) ------------------------

void WorldState::send_enter_entity(SessionRec& to, const WorldEntityRec& subject) {
    if (!to.egress) return;
    // Full spawn state (position + #430 vitals + name), read from the entity's
    // authoritative Unit — the SAME EntityEnter shape a session presents, so the
    // client renders a spawned NPC/creature exactly like a remote player.
    to.egress(mn::Opcode::ENTITY_ENTER,
              encode_entity_enter_payload(subject.identity, subject.unit));
}

void WorldState::send_leave_entity(SessionRec& to, const WorldEntityRec& subject,
                                   net::LeaveReason reason) {
    if (!to.egress) return;
    to.egress(mn::Opcode::ENTITY_LEAVE,
              encode_entity_leave_payload(subject.identity.entity_guid, reason));
}

void WorldState::relay_visible_entities(SessionRec& self,
                                        const std::unordered_set<AoiId>& now_guids) {
    // ENTERS: entity guids now in `self`'s interest set it did not already see. (A
    // guid in now_guids is EITHER a session or an entity; entities_ discriminates.)
    for (AoiId g : now_guids) {
        auto eit = entities_.find(g);
        if (eit == entities_.end()) continue;             // a session, handled elsewhere
        if (self.visible_entities.count(g) != 0) continue;  // already visible
        send_enter_entity(self, eit->second);
        self.visible_entities.insert(g);
    }
    // LEAVES: entity guids `self` used to see but no longer does.
    std::vector<AoiId> gone;
    for (AoiId g : self.visible_entities) {
        if (now_guids.find(g) == now_guids.end()) gone.push_back(g);
    }
    for (AoiId g : gone) {
        self.visible_entities.erase(g);
        auto eit = entities_.find(g);
        if (eit == entities_.end()) continue;  // entity dropped (not at M1) — defensive
        send_leave_entity(self, eit->second, net::LeaveReason::OUT_OF_RANGE);
    }
}

AoiId WorldState::add_world_entity(const EntityIdentity& identity, const UnitStats& stats,
                                   std::uint32_t npc_template_id, const Position& pos) {
    std::lock_guard<std::mutex> lk(mtx_);
    // The authoritative Unit (spawned alive at full health by the Unit ctor). It owns
    // the entity's position; the grid mirrors it. Faction/level/health come from the
    // resolved spawn stats (npc_template) so the #430 vitals on EntityEnter are real.
    // Aggregate-initialized (Creature has no default ctor), then moved into the map.
    WorldEntityRec rec{identity, Creature(identity.entity_guid, pos, stats, npc_template_id),
                       npc_template_id};
    entities_.emplace(identity.entity_guid, std::move(rec));
    grid_.upsert(identity.entity_guid, pos);
    log::info(kCat, "world entity spawned guid=" + std::to_string(identity.entity_guid) +
                        " npc=" + std::to_string(npc_template_id) + " at (" +
                        std::to_string(pos.x) + "," + std::to_string(pos.y) + ")");
    return identity.entity_guid;
}

std::optional<std::uint32_t> WorldState::npc_template_for_guid(AoiId guid) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = entities_.find(guid);
    if (it == entities_.end()) return std::nullopt;
    return it->second.npc_template_id;
}

std::size_t WorldState::world_entity_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return entities_.size();
}

// ---------------------------------------------------------------------------
// Area triggers + POI discovery (#368; WLD-01/03). Evaluated against the mover's
// authoritative position (the map tick's movement phase; SAD §2.5).
// ---------------------------------------------------------------------------

void WorldState::load_area_triggers(std::vector<TriggerVolume> volumes) {
    std::lock_guard<std::mutex> lk(mtx_);
    const std::size_t n = volumes.size();
    triggers_.load(std::move(volumes));
    log::info(kCat, "loaded " + std::to_string(n) + " area-trigger volume(s)");
}

void WorldState::set_area_trigger_hook(AreaTriggerHook hook) {
    std::lock_guard<std::mutex> lk(mtx_);
    area_trigger_hook_ = std::move(hook);
}

std::vector<TriggerEvent> WorldState::fire_area_triggers(SessionRec& self) {
    // Caller holds mtx_. Diff the mover's position against the volume set; dispatch
    // each crossing. Cheap no-op when no volumes are loaded (the DB-less dispatch
    // and relay tests never load a set, so their behaviour is unchanged).
    const AoiId guid = self.identity.entity_guid;
    // The Unit owns the authoritative position (#342); evaluate against it.
    std::vector<TriggerEvent> events = triggers_.evaluate(guid, self.unit.position());
    for (const TriggerEvent& e : events) {
        // Discovery: the server has marked the POI discovered on the character
        // (in-memory at M1; persisting to the characters DB is a later concern) —
        // NOTIFY the client exactly once (re-entry never re-fires; enforced by the
        // AreaTriggerSet discovered bookkeeping).
        if (e.kind == TriggerKind::kDiscovery && e.discovered_now) {
            if (self.egress) {
                self.egress(mn::Opcode::POI_DISCOVERED,
                            encode_poi_discovered_payload(e.trigger_id, e.area_id, e.name_id));
            }
            log::info(kCat, "POI discovered guid=" + std::to_string(guid) + " trigger=" +
                                std::to_string(e.trigger_id) + " area=" +
                                std::to_string(e.area_id));
        }

        // Server-side OnAreaTrigger hook (SAD §2.5 script-hook seam): fired for
        // every enter/leave crossing of every kind (discovery, quest-objective,
        // graveyard, generic). At M1 the default is a log line; a typed hook
        // registry keyed by content id is a later concern.
        if (area_trigger_hook_) {
            area_trigger_hook_(guid, e);
        } else {
            log::debug(kCat, std::string("area-trigger ") + (e.entered ? "ENTER" : "LEAVE") +
                                 " guid=" + std::to_string(guid) + " trigger=" +
                                 std::to_string(e.trigger_id) + " kind=" +
                                 std::to_string(static_cast<int>(e.kind)));
        }
    }
    return events;
}

EnterResult WorldState::enter(const EntityIdentity& identity, const Position& spawn,
                              EgressFn egress) {
    std::lock_guard<std::mutex> lk(mtx_);

    const SessionSlot slot = next_slot_++;

    // Assign a unique synthetic entity guid when the placeholder guid is 0 (the
    // D-11 stub), so two placeholder sessions do not collide in the grid /
    // slot_by_guid_ or present the same id on the wire (which would make two
    // clients render as one entity). A real character guid (M1) passes through.
    EntityIdentity id = identity;
    if (id.entity_guid == 0) id.entity_guid = kSyntheticGuidBase + slot;

    // Build the session's Unit (#342): a Player spawned at `spawn` with clean-room
    // placeholder stats picked by the M0-frozen class id (the D-11 placeholder
    // pattern — no content pipeline yet). The Unit OWNS the authoritative position
    // that the grid mirrors. The character name (#367) flows through so the Player
    // carries it and the whisper name index below can address this session; it is
    // empty for the D-11 placeholder (no characters DB). Player() ctor spawns it
    // alive at full health.
    SessionRec rec;
    rec.identity = id;
    rec.unit = Player(id.entity_guid, spawn, placeholder_player_stats(id.char_class),
                      /*account_id=*/0, id.char_class, id.name);
    rec.egress = std::move(egress);
    sessions_.emplace(slot, std::move(rec));
    slot_by_guid_[id.entity_guid] = slot;
    // Whisper name index (#367): a NON-empty name makes this session addressable by
    // whisper. Case-insensitive key. A duplicate name (should not happen — names
    // are unique per the characters CRUD) resolves to the most recent entrant.
    if (!id.name.empty()) slot_by_name_ci_[to_lower_ascii(id.name)] = slot;
    grid_.upsert(id.entity_guid, spawn);

    // Initial interest set: who this newcomer can already see. Compute against an
    // empty "previous" (nothing was visible yet) so the enter radius applies.
    SessionRec& self = sessions_.at(slot);
    std::unordered_set<AoiId> visible_guids =
        grid_.interest_set(id.entity_guid, /*previous=*/{});

    for (AoiId guid : visible_guids) {
        std::optional<SessionSlot> other = slot_of_guid(guid);
        if (!other) continue;
        auto oit = sessions_.find(*other);
        if (oit == sessions_.end()) continue;
        SessionRec& other_rec = oit->second;

        // Bidirectional: newcomer sees the other, and the other sees the newcomer.
        send_enter(self, other_rec);
        self.visible.insert(*other);

        send_enter(other_rec, self);
        other_rec.visible.insert(slot);
    }

    // Static world entities (#486): the newcomer also sees any content spawn
    // (creature / NPC) already in range — send it an EntityEnter for each. One-way
    // (an entity has no egress), so there is no reciprocal enter.
    relay_visible_entities(self, visible_guids);

    // Area triggers (#368): evaluate the spawn position so a character that logs
    // in already standing inside a volume fires its enter/discovery immediately
    // (a POI at the spawn point, a graveyard you resurrect into, etc.).
    std::vector<TriggerEvent> triggers = fire_area_triggers(self);

    log::info(kCat, "session entered slot=" + std::to_string(slot) + " guid=" +
                        std::to_string(id.entity_guid) + " at (" +
                        std::to_string(spawn.x) + "," + std::to_string(spawn.y) +
                        ") sees " + std::to_string(self.visible.size()) + " other(s)");
    return EnterResult{slot, id.entity_guid, std::move(triggers)};
}

std::vector<TriggerEvent> WorldState::on_movement(SessionSlot slot, const Position& pos,
                                                  std::uint32_t ack_seq,
                                                  std::uint32_t state_flags,
                                                  std::uint64_t server_time_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    return move_session_locked(slot, pos, ack_seq, state_flags, server_time_ms);
}

std::vector<TriggerEvent> WorldState::move_session_locked(SessionSlot slot,
                                                          const Position& pos,
                                                          std::uint32_t ack_seq,
                                                          std::uint32_t state_flags,
                                                          std::uint64_t server_time_ms) {
    auto sit = sessions_.find(slot);
    if (sit == sessions_.end()) return {};  // not entered
    SessionRec& self = sit->second;
    self.unit.set_position(pos);  // the Unit owns the authoritative position (#342)
    self.state_flags = state_flags;
    grid_.upsert(self.identity.entity_guid, pos);

    // Area triggers (#368): evaluate the mover's new authoritative position for
    // enter/leave crossings + POI discovery before the AoI relay. This is the map
    // tick's post-movement trigger phase (SAD §2.5) realized at the M0 movement seam.
    std::vector<TriggerEvent> triggers = fire_area_triggers(self);

    // Translate self's PREVIOUS visible-slot set into guids for the hysteresis
    // resolution against the grid (which is keyed by guid).
    std::unordered_set<AoiId> prev_guids;
    prev_guids.reserve(self.visible.size());
    for (SessionSlot vs : self.visible) {
        auto vit = sessions_.find(vs);
        if (vit != sessions_.end()) prev_guids.insert(vit->second.identity.entity_guid);
    }

    // Recompute self's interest set WITH HYSTERESIS.
    std::unordered_set<AoiId> now_guids =
        grid_.interest_set(self.identity.entity_guid, prev_guids);

    // Convert to slots + diff against self.visible.
    std::unordered_set<SessionSlot> now_slots;
    now_slots.reserve(now_guids.size());
    for (AoiId g : now_guids) {
        std::optional<SessionSlot> s = slot_of_guid(g);
        if (s) now_slots.insert(*s);
    }

    // For each subscriber that ALREADY saw self: EntityUpdate (self moved). For
    // each NEWLY seeing self: EntityEnter (both directions). For each that no
    // longer sees self: EntityLeave (both directions).
    //
    // Note the symmetry: self's interest set == the set of others that can see
    // self (distance is symmetric, and the enter/leave radii are shared). The
    // per-observer hysteresis is resolved from EACH observer's own `visible` set,
    // so a subscriber only "enters" self when self crosses ITS enter radius. We
    // drive the mover's side from `now_slots` and each observer's side from that
    // observer's own record, keeping both books consistent.

    // (a) ENTERS + UPDATES on the mover's own view, and the reciprocal enter on
    //     the newly-seen session's view.
    for (SessionSlot other_slot : now_slots) {
        auto oit = sessions_.find(other_slot);
        if (oit == sessions_.end()) continue;
        SessionRec& other = oit->second;

        const bool self_saw_other = self.visible.find(other_slot) != self.visible.end();
        if (!self_saw_other) {
            // Newly in range → the mover now sees `other`, and `other` now sees
            // the mover. EntityEnter both ways (full spawn state), then future
            // moves are EntityUpdate.
            send_enter(self, other);
            self.visible.insert(other_slot);

            send_enter(other, self);
            other.visible.insert(slot);
        } else {
            // Already mutually visible: the mover moved, so tell `other` about
            // the mover's new position (EntityUpdate). The mover's own view of
            // `other` is unchanged (other did not move here).
            send_update(other, self, ack_seq, server_time_ms);
        }
    }

    // (b) LEAVES: anyone self used to see but no longer does. EntityLeave both
    //     ways (out of range).
    std::vector<SessionSlot> gone;
    for (SessionSlot prev_slot : self.visible) {
        if (now_slots.find(prev_slot) == now_slots.end()) gone.push_back(prev_slot);
    }
    for (SessionSlot prev_slot : gone) {
        self.visible.erase(prev_slot);
        auto oit = sessions_.find(prev_slot);
        if (oit == sessions_.end()) continue;
        SessionRec& other = oit->second;

        send_leave(self, other, net::LeaveReason::OUT_OF_RANGE);
        send_leave(other, self, net::LeaveReason::OUT_OF_RANGE);
        other.visible.erase(slot);
    }

    // Static world entities (#486): relay EntityEnter/Leave for content spawns the
    // mover came into / out of range of this move (entities never move, so they are
    // never EntityUpdate — only the observer's crossing of their range matters).
    relay_visible_entities(self, now_guids);

    log::debug(kCat, "movement slot=" + std::to_string(slot) + " ack=" +
                         std::to_string(ack_seq) + " now sees " +
                         std::to_string(self.visible.size()) + " other(s)");
    return triggers;
}

void WorldState::leave(SessionSlot slot) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto sit = sessions_.find(slot);
    if (sit == sessions_.end()) return;
    SessionRec& self = sit->second;

    // Tell everyone who currently sees self that it despawned, and drop self from
    // their visible sets.
    for (SessionSlot observer_slot : self.visible) {
        auto oit = sessions_.find(observer_slot);
        if (oit == sessions_.end()) continue;
        send_leave(oit->second, self, net::LeaveReason::DESPAWNED);
        oit->second.visible.erase(slot);
    }

    grid_.remove(self.identity.entity_guid);
    slot_by_guid_.erase(self.identity.entity_guid);
    // Drop this character's area-trigger bookkeeping (occupancy + discovered).
    triggers_.remove(self.identity.entity_guid);
    // Drop the whisper name index entry — but ONLY if it still points at THIS slot
    // (a later same-name entrant would have overwritten it; a compare-and-erase
    // keeps that newer session addressable). (#367)
    if (!self.identity.name.empty()) {
        const std::string key = to_lower_ascii(self.identity.name);
        auto nit = slot_by_name_ci_.find(key);
        if (nit != slot_by_name_ci_.end() && nit->second == slot) {
            slot_by_name_ci_.erase(nit);
        }
    }
    log::info(kCat, "session left slot=" + std::to_string(slot) + " guid=" +
                        std::to_string(self.identity.entity_guid));
    sessions_.erase(sit);
}

void WorldState::set_session_control(SessionSlot slot, ForcedMoveMailbox* forced_move,
                                     DisconnectFn disconnect) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto sit = sessions_.find(slot);
    if (sit == sessions_.end()) return;  // not entered
    sit->second.forced_move = forced_move;
    sit->second.disconnect = std::move(disconnect);
}

WorldState::TargetOutcome WorldState::summon_to(const std::string& target_name,
                                                const Position& dest,
                                                std::uint32_t ack_seq,
                                                std::uint32_t state_flags,
                                                std::uint64_t server_time_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto nit = slot_by_name_ci_.find(to_lower_ascii(target_name));
    if (nit == slot_by_name_ci_.end()) return TargetOutcome::kTargetOffline;
    const SessionSlot target_slot = nit->second;
    auto sit = sessions_.find(target_slot);
    if (sit == sessions_.end()) return TargetOutcome::kTargetOffline;  // stale index (defensive)

    // Move the target in the grid + relay the AoI deltas so every observer (and the
    // summoner, if in range) sees the target appear at the caller's position at once.
    move_session_locked(target_slot, dest, ack_seq, state_flags, server_time_ms);

    // Hand the destination to the target's own IO worker: it applies force_correction
    // (authoritative reset + ack-barrier arm + speed-window clear) + snaps its own
    // client on its next turn — keeping SessionMovementState single-threaded (#418).
    if (sit->second.forced_move != nullptr) sit->second.forced_move->post(dest);

    log::info(kCat, "GM summon: moved '" + target_name + "' (slot " +
                        std::to_string(target_slot) + ") to summoner position");
    return TargetOutcome::kApplied;
}

WorldState::TargetOutcome WorldState::disconnect_by_name(const std::string& target_name) {
    // Look the target up + COPY its disconnect closure OUT under the lock, then clear
    // it so a concurrent/second kick is a no-op. Invoke the closure AFTER releasing
    // the lock: it calls leave() (which re-takes mtx_) + writes the victim's socket,
    // so running it under the lock would deadlock / serialize a socket write (the same
    // "kick outside the registry lock" discipline as #326).
    DisconnectFn teardown;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto nit = slot_by_name_ci_.find(to_lower_ascii(target_name));
        if (nit == slot_by_name_ci_.end()) return TargetOutcome::kTargetOffline;
        auto sit = sessions_.find(nit->second);
        if (sit == sessions_.end() || !sit->second.disconnect)
            return TargetOutcome::kTargetOffline;
        teardown = std::move(sit->second.disconnect);  // consume (one-shot)
        sit->second.disconnect = nullptr;
    }
    teardown();  // Disconnect{KICKED} + AoI leave, outside the world lock
    log::info(kCat, "GM kick: disconnected '" + target_name + "'");
    return TargetOutcome::kApplied;
}

bool WorldState::set_unit_level(SessionSlot slot, std::uint16_t level) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto sit = sessions_.find(slot);
    if (sit == sessions_.end()) return false;  // not entered
    sit->second.unit.set_level(level);
    return true;
}

std::size_t WorldState::broadcast_vitals(AoiId guid) {
    std::lock_guard<std::mutex> lk(mtx_);
    const std::optional<SessionSlot> subject = slot_of_guid(guid);
    if (!subject) return 0;  // no such in-world unit
    auto sit = sessions_.find(*subject);
    if (sit == sessions_.end()) return 0;

    // Read the vitals straight off the authoritative Unit (SAD §2.5 — the unit owns
    // its combat state) and project them to a VITALS_UPDATE payload once; the same
    // bytes go to every recipient.
    const std::vector<std::uint8_t> payload =
        encode_vitals_update_payload(guid, sit->second.unit);

    std::size_t recipients = 0;
    // (1) The subject's OWN client — its player unit frame (a session always sees
    // its own vitals, even when no other session is in AoI).
    if (sit->second.egress) {
        sit->second.egress(mn::Opcode::VITALS_UPDATE, payload);
        ++recipients;
    }
    // (2) Every OTHER session that currently SEES the subject (has it in its AoI
    // interest set) — their target/nameplate frame for this unit. Driven off each
    // observer's own `visible` set so it exactly matches who was sent the EntityEnter.
    for (auto& [slot, rec] : sessions_) {
        if (slot == *subject) continue;
        if (rec.visible.find(*subject) == rec.visible.end()) continue;
        if (!rec.egress) continue;
        rec.egress(mn::Opcode::VITALS_UPDATE, payload);
        ++recipients;
    }
    log::debug(kCat, "vitals broadcast guid=" + std::to_string(guid) + " -> " +
                         std::to_string(recipients) + " recipient(s)");
    return recipients;
}

std::size_t WorldState::session_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sessions_.size();
}

Unit* WorldState::unit_for_slot(SessionSlot slot) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(slot);
    if (it == sessions_.end()) return nullptr;
    return &it->second.unit;
}

const Unit* WorldState::unit_for_slot(SessionSlot slot) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(slot);
    if (it == sessions_.end()) return nullptr;
    return &it->second.unit;
}

Unit* WorldState::unit_for_guid(AoiId guid) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto sit = slot_by_guid_.find(guid);
    if (sit != slot_by_guid_.end()) {
        auto it = sessions_.find(sit->second);
        if (it != sessions_.end()) return &it->second.unit;
    }
    // A content spawn (#486): the client targets it by guid, so combat reaches it here.
    auto eit = entities_.find(guid);
    if (eit != entities_.end()) return &eit->second.unit;
    return nullptr;
}

const Unit* WorldState::unit_for_guid(AoiId guid) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto sit = slot_by_guid_.find(guid);
    if (sit != slot_by_guid_.end()) {
        auto it = sessions_.find(sit->second);
        if (it != sessions_.end()) return &it->second.unit;
    }
    auto eit = entities_.find(guid);
    if (eit != entities_.end()) return &eit->second.unit;
    return nullptr;
}

// ---------------------------------------------------------------------------
// SOC-01 chat routing (#367)
// ---------------------------------------------------------------------------

void WorldState::send_chat(SessionRec& to, net::ChatChannel channel, AoiId sender_guid,
                           const std::string& sender_name, const std::string& text) {
    if (!to.egress) return;
    to.egress(mn::Opcode::CHAT_DELIVER,
              encode_chat_deliver_payload(channel, sender_guid, sender_name, text));
}

std::size_t WorldState::deliver_spatial(SessionSlot from, net::ChatChannel channel,
                                        const std::string& text) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto fit = sessions_.find(from);
    if (fit == sessions_.end()) return 0;  // sender not in world
    SessionRec& sender = fit->second;
    const AoiId sender_guid = sender.identity.entity_guid;
    const std::string sender_name = sender.identity.name;

    const float radius =
        (channel == mn::ChatChannel::YELL) ? kChatYellRadiusM : kChatSayRadiusM;

    std::size_t recipients = 0;

    // The sender always sees its own line (say/yell echo). Deliver it first.
    send_chat(sender, channel, sender_guid, sender_name, text);
    ++recipients;

    // Every OTHER session within the channel's radius, via the #87 grid visitor.
    const std::unordered_set<AoiId> in_range =
        grid_.within_radius(sender_guid, radius);
    for (AoiId guid : in_range) {
        std::optional<SessionSlot> other = slot_of_guid(guid);
        if (!other) continue;
        auto oit = sessions_.find(*other);
        if (oit == sessions_.end()) continue;
        send_chat(oit->second, channel, sender_guid, sender_name, text);
        ++recipients;
    }

    log::debug(kCat, "chat spatial channel=" +
                         std::to_string(static_cast<int>(channel)) + " from guid=" +
                         std::to_string(sender_guid) + " -> " +
                         std::to_string(recipients) + " recipient(s)");
    return recipients;
}

ChatWhisperOutcome WorldState::whisper(SessionSlot from, const std::string& target_name,
                                       const std::string& text) {
    std::lock_guard<std::mutex> lk(mtx_);

    if (target_name.empty()) return ChatWhisperOutcome::kNoTarget;

    auto fit = sessions_.find(from);
    if (fit == sessions_.end()) return ChatWhisperOutcome::kTargetOffline;  // sender gone
    const AoiId sender_guid = fit->second.identity.entity_guid;
    const std::string sender_name = fit->second.identity.name;

    auto nit = slot_by_name_ci_.find(to_lower_ascii(target_name));
    if (nit == slot_by_name_ci_.end()) return ChatWhisperOutcome::kTargetOffline;
    auto tit = sessions_.find(nit->second);
    if (tit == sessions_.end()) return ChatWhisperOutcome::kTargetOffline;  // defensive

    // Deliver to the target across sessions — the recipient's shard worker need
    // not be involved (SAD §3.8); at M1 both live in this worldd, so it is a
    // direct egress write to the named session's channel.
    send_chat(tit->second, mn::ChatChannel::WHISPER, sender_guid, sender_name, text);
    log::debug(kCat, "chat whisper from guid=" + std::to_string(sender_guid) +
                         " -> \"" + target_name + "\" (slot " +
                         std::to_string(nit->second) + ")");
    return ChatWhisperOutcome::kDelivered;
}

std::size_t WorldState::deliver_channel(SessionSlot from, const std::string& text) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto fit = sessions_.find(from);
    if (fit == sessions_.end()) return 0;  // sender not in world
    const AoiId sender_guid = fit->second.identity.entity_guid;
    const std::string sender_name = fit->second.identity.name;

    // Zone/general membership at M1 = every in-world session on this shard
    // (one map); realm-wide-across-shards membership via servicesd is M3.
    std::size_t recipients = 0;
    for (auto& [slot, rec] : sessions_) {
        (void)slot;
        send_chat(rec, mn::ChatChannel::ZONE, sender_guid, sender_name, text);
        ++recipients;
    }
    log::debug(kCat, "chat zone from guid=" + std::to_string(sender_guid) + " -> " +
                         std::to_string(recipients) + " recipient(s)");
    return recipients;
}

}  // namespace meridian::worldd
