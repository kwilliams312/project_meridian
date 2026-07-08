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
    auto e = mn::CreateEntityEnter(b, in.entity_guid, in.type_id, in.x, in.y, in.z,
                                   in.orientation, attrs, in.char_class);
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

}  // namespace meridian::clientnet::codec
