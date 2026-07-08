// SPDX-License-Identifier: Apache-2.0
//
// worldd — shared world state + AoI movement relay implementation (issue #87).
// See world_state.h for the clean-room provenance, the relay design, the egress
// seam, and the threading model.

#include "world_state.h"

#include <flatbuffers/flatbuffers.h>

#include "meridian/core/log.hpp"
#include "meridian/net/tls_listener.h"

#include "world_dispatch.h"  // encode_frame

namespace meridian::worldd {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace log = meridian::core::log;

constexpr const char* kCat = "worldd.aoi";

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

std::vector<std::uint8_t> encode_entity_enter_payload(const EntityIdentity& subject,
                                                      const Position& pos) {
    fb::FlatBufferBuilder b;
    // attrs empty at M0 (D-11 placeholder carries no wire attributes yet); the
    // vector is created so the table shape is complete for the client verifier.
    auto attrs = b.CreateVector(std::vector<fb::Offset<mn::AttrDelta>>{});
    // char_class (#328): the mover's M0-frozen class id, so every client renders the
    // placeholder capsule in a class-derived color. Authoritative here on the server.
    auto e = mn::CreateEntityEnter(b, subject.entity_guid, subject.type_id, pos.x, pos.y,
                                   pos.z, pos.orientation, attrs, subject.char_class);
    b.Finish(e);
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
              encode_entity_enter_payload(subject.identity, subject.unit.position()));
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

void WorldState::fire_area_triggers(SessionRec& self) {
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
    // that the grid mirrors; account/name are not carried on the AoI enter path
    // (0 / empty here) — the dispatcher can enrich the Player later without
    // touching this relay. Player() ctor spawns it alive at full health.
    SessionRec rec;
    rec.identity = id;
    rec.unit = Player(id.entity_guid, spawn, placeholder_player_stats(id.char_class),
                      /*account_id=*/0, id.char_class, /*name=*/std::string{});
    rec.egress = std::move(egress);
    sessions_.emplace(slot, std::move(rec));
    slot_by_guid_[id.entity_guid] = slot;
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

    // Area triggers (#368): evaluate the spawn position so a character that logs
    // in already standing inside a volume fires its enter/discovery immediately
    // (a POI at the spawn point, a graveyard you resurrect into, etc.).
    fire_area_triggers(self);

    log::info(kCat, "session entered slot=" + std::to_string(slot) + " guid=" +
                        std::to_string(id.entity_guid) + " at (" +
                        std::to_string(spawn.x) + "," + std::to_string(spawn.y) +
                        ") sees " + std::to_string(self.visible.size()) + " other(s)");
    return EnterResult{slot, id.entity_guid};
}

void WorldState::on_movement(SessionSlot slot, const Position& pos, std::uint32_t ack_seq,
                             std::uint32_t state_flags, std::uint64_t server_time_ms) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto sit = sessions_.find(slot);
    if (sit == sessions_.end()) return;  // not entered
    SessionRec& self = sit->second;
    self.unit.set_position(pos);  // the Unit owns the authoritative position (#342)
    self.state_flags = state_flags;
    grid_.upsert(self.identity.entity_guid, pos);

    // Area triggers (#368): evaluate the mover's new authoritative position for
    // enter/leave crossings + POI discovery before the AoI relay. This is the map
    // tick's post-movement trigger phase (SAD §2.5) realized at the M0 movement seam.
    fire_area_triggers(self);

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

    log::debug(kCat, "movement slot=" + std::to_string(slot) + " ack=" +
                         std::to_string(ack_seq) + " now sees " +
                         std::to_string(self.visible.size()) + " other(s)");
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
    log::info(kCat, "session left slot=" + std::to_string(slot) + " guid=" +
                        std::to_string(self.identity.entity_guid));
    sessions_.erase(sit);
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
    if (sit == slot_by_guid_.end()) return nullptr;
    auto it = sessions_.find(sit->second);
    if (it == sessions_.end()) return nullptr;
    return &it->second.unit;
}

const Unit* WorldState::unit_for_guid(AoiId guid) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto sit = slot_by_guid_.find(guid);
    if (sit == slot_by_guid_.end()) return nullptr;
    auto it = sessions_.find(sit->second);
    if (it == sessions_.end()) return nullptr;
    return &it->second.unit;
}

}  // namespace meridian::worldd
