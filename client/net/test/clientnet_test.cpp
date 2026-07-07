// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free unit tests for the client net core (issue #95).
// Plain-main style (no Godot, no live server), mirroring the client/server core
// tests (client/bot/test/*, server/worldd/test/*). ctest-wired via
// client/net/CMakeLists.txt.
//
// Proves the #95 deliverables WITHOUT a live server:
//   1. FRAMING — u32-LE length prefix known-answer + FrameReader deframes chunked,
//      concatenated, and partial streams; oversize length latches an error.
//   2. IF-2 WORLD-FRAME CODEC — encode/decode round-trip + known-answer byte layout
//      (u16 opcode LE ‖ u64 seq LE ‖ payload); a too-short body is rejected.
//   3. AEAD WORLD SESSION — seal/open round-trip; wrong seq / tampered byte fail
//      open; two sessions from the SAME key derive identical per-direction keys and
//      open each other's frames (the client↔worldd "crypto agrees" proof, offline).
//   4. FLATBUFFERS CODEC — ClientHello (IF-1), MovementIntent / MovementState /
//      Disconnect (IF-2) round-trip through plain structs; a garbage buffer is
//      rejected by the verifier (never GetRoot on unverified bytes).
//   5. FULL CLIENT STACK COMPOSITION — build a MovementIntent payload, wrap it in
//      the IF-2 frame, length-prefix it, deframe with FrameReader, decode the frame,
//      decode the payload — every field survives the whole client send/recv path.

#include "meridian/clientnet/codec.h"
#include "meridian/clientnet/framing.h"
#include "meridian/clientnet/tls_client.h"
#include "meridian/clientnet/transport.h"
#include "meridian/clientnet/wire_frame.h"
#include "meridian/clientnet/world_session.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace meridian::clientnet;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* what, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

Bytes bytes_of(std::initializer_list<std::uint8_t> l) { return Bytes(l); }

// ---------------------------------------------------------------------------
// 1. Framing
// ---------------------------------------------------------------------------
void test_framing() {
    std::puts("[framing] length-prefix known-answer + FrameReader");

    // Known-answer: a 3-byte payload frames to [03 00 00 00][payload] (u32 LE).
    Bytes payload = bytes_of({0xAA, 0xBB, 0xCC});
    auto framed = frame_message(payload);
    CHECK(framed.has_value());
    CHECK(framed->size() == 4 + 3);
    CHECK((*framed)[0] == 0x03 && (*framed)[1] == 0x00 && (*framed)[2] == 0x00 &&
          (*framed)[3] == 0x00);
    CHECK((*framed)[4] == 0xAA && (*framed)[5] == 0xBB && (*framed)[6] == 0xCC);

    // Empty payload frames to a bare zero length.
    auto empty = frame_message(Bytes{});
    CHECK(empty.has_value() && empty->size() == 4);
    CHECK((*empty)[0] == 0 && (*empty)[1] == 0 && (*empty)[2] == 0 && (*empty)[3] == 0);

    // Oversize payload is refused (never framed).
    Bytes huge(kMaxFrameBytes + 1, 0x00);
    CHECK(!frame_message(huge).has_value());

    // read_length_prefix decodes the u32 LE.
    CHECK(*read_length_prefix(*framed) == 3u);
    CHECK(!read_length_prefix(bytes_of({0x01, 0x02})).has_value());  // too short

    // FrameReader: feed two whole frames at once, drain both.
    FrameReader r;
    auto f1 = frame_message(bytes_of({0x01}));
    auto f2 = frame_message(bytes_of({0x02, 0x03}));
    r.feed(*f1);
    r.feed(*f2);
    auto p1 = r.next();
    auto p2 = r.next();
    CHECK(p1.has_value() && *p1 == bytes_of({0x01}));
    CHECK(p2.has_value() && *p2 == bytes_of({0x02, 0x03}));
    CHECK(!r.next().has_value());  // nothing left

    // FrameReader: byte-at-a-time (partial) feed of one frame yields nothing until
    // the last byte arrives.
    FrameReader r2;
    auto f3 = frame_message(bytes_of({0x10, 0x20, 0x30, 0x40}));
    for (std::size_t i = 0; i + 1 < f3->size(); ++i) {
        r2.feed(&(*f3)[i], 1);
        CHECK(!r2.next().has_value());  // frame not complete yet
    }
    r2.feed(&f3->back(), 1);
    auto p3 = r2.next();
    CHECK(p3.has_value() && *p3 == bytes_of({0x10, 0x20, 0x30, 0x40}));

    // FrameReader: an oversize advertised length latches a permanent error.
    FrameReader r3;
    Bytes bad_prefix = bytes_of({0x01, 0x00, 0x00, 0x01});  // 0x01000000 = 16 MiB
    r3.feed(bad_prefix);
    CHECK(!r3.next().has_value());
    CHECK(r3.error());
    r3.feed(bytes_of({0x00}));         // more bytes do not clear the error
    CHECK(!r3.next().has_value());
}

// ---------------------------------------------------------------------------
// 2. IF-2 world-frame codec
// ---------------------------------------------------------------------------
void test_wire_frame() {
    std::puts("[wire_frame] IF-2 header known-answer + round-trip");

    Bytes payload = bytes_of({0xDE, 0xAD, 0xBE, 0xEF});
    Bytes frame = encode_world_frame(kOpMovementIntent, /*seq=*/0x0102030405060708ull,
                                     payload);
    // Known-answer: u16 opcode LE (01 10) ‖ u64 seq LE (08..01) ‖ payload.
    CHECK(frame.size() == kFrameHeaderBytes + payload.size());
    CHECK(frame[0] == 0x01 && frame[1] == 0x10);  // 0x1001 LE
    CHECK(frame[2] == 0x08 && frame[3] == 0x07 && frame[4] == 0x06 && frame[5] == 0x05);
    CHECK(frame[6] == 0x04 && frame[7] == 0x03 && frame[8] == 0x02 && frame[9] == 0x01);

    auto decoded = decode_world_frame(frame);
    CHECK(decoded.has_value());
    CHECK(decoded->opcode == kOpMovementIntent);
    CHECK(decoded->seq == 0x0102030405060708ull);
    CHECK(decoded->payload == payload);

    // A body shorter than the 10-byte header is malformed.
    CHECK(!decode_world_frame(bytes_of({0x01, 0x10, 0x00})).has_value());
    // Exactly the header, empty payload, is valid.
    auto empty = decode_world_frame(encode_world_frame(kOpClockSync, 7, Bytes{}));
    CHECK(empty.has_value() && empty->opcode == kOpClockSync && empty->seq == 7 &&
          empty->payload.empty());
}

// ---------------------------------------------------------------------------
// 3. AEAD world session
// ---------------------------------------------------------------------------
void test_world_session() {
    std::puts("[world_session] AEAD seal/open + cross-instance interop");

    // A fixed, spec-faithful 32-byte key (test vector; not a secret).
    Bytes key(kAeadKeyBytes);
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i + 1);

    WorldSession a(key);
    WorldSession b(key);
    CHECK(a.ok() && b.ok());

    // A wrong-length key produces a not-ok session.
    WorldSession bad(Bytes(16, 0x00));
    CHECK(!bad.ok());

    // Both ends derive IDENTICAL per-direction keys (deterministic HKDF) and the
    // two directions differ from each other.
    CHECK(a.key(Direction::kClientToServer) == b.key(Direction::kClientToServer));
    CHECK(a.key(Direction::kServerToClient) == b.key(Direction::kServerToClient));
    CHECK(a.key(Direction::kClientToServer) != a.key(Direction::kServerToClient));

    // Seal on 'a' as client→server; open on 'b' as client→server → round-trips.
    Bytes plaintext = bytes_of({0x11, 0x22, 0x33, 0x44, 0x55});
    Bytes aad;  // empty at M0
    std::uint64_t seq = 0;
    auto sealed = a.seal(Direction::kClientToServer, plaintext, aad, seq);
    CHECK(sealed.has_value());
    CHECK(seq == 0);                                    // first seal uses seq 0
    CHECK(sealed->size() == plaintext.size() + kAeadTagBytes);
    auto opened = b.open(Direction::kClientToServer, *sealed, seq, aad);
    CHECK(opened.has_value() && *opened == plaintext);

    // The counter advanced: a second seal uses seq 1.
    std::uint64_t seq2 = 0;
    auto sealed2 = a.seal(Direction::kClientToServer, plaintext, aad, seq2);
    CHECK(sealed2.has_value() && seq2 == 1);

    // Opening under the WRONG seq fails (nonce mismatch → bad tag).
    CHECK(!b.open(Direction::kClientToServer, *sealed, seq + 99, aad).has_value());

    // Opening under the WRONG direction fails (different key).
    CHECK(!b.open(Direction::kServerToClient, *sealed, seq, aad).has_value());

    // A single tampered ciphertext byte fails authentication.
    Bytes tampered = *sealed;
    tampered[0] ^= 0x01;
    CHECK(!b.open(Direction::kClientToServer, tampered, seq, aad).has_value());

    // A tampered AAD fails authentication.
    Bytes aad2 = bytes_of({0x01});
    std::uint64_t sseq = 0;
    auto sealed_aad = a.seal(Direction::kServerToClient, plaintext, aad2, sseq);
    CHECK(sealed_aad.has_value());
    CHECK(b.open(Direction::kServerToClient, *sealed_aad, sseq, aad2).has_value());
    CHECK(!b.open(Direction::kServerToClient, *sealed_aad, sseq, Bytes{}).has_value());
}

// ---------------------------------------------------------------------------
// 4. FlatBuffers codec
// ---------------------------------------------------------------------------
void test_codec() {
    std::puts("[codec] FlatBuffers IF-1/IF-2 round-trip + verifier");

    // ClientHello (IF-1).
    codec::ClientHello ch{/*build=*/1000, /*proto_ver=*/1};
    auto ch_buf = codec::encode_client_hello(ch);
    auto ch_out = codec::decode_client_hello(ch_buf);
    CHECK(ch_out.has_value());
    CHECK(ch_out->build == 1000 && ch_out->proto_ver == 1);

    // MovementIntent (IF-2).
    codec::MovementIntent mi;
    mi.seq = 42;
    mi.state_flags = 0x5;
    mi.x = 64.0f;
    mi.y = 65.5f;
    mi.z = 0.25f;
    mi.orientation = 1.5708f;
    mi.client_time_ms = 123456789ull;
    auto mi_buf = codec::encode_movement_intent(mi);
    auto mi_out = codec::decode_movement_intent(mi_buf);
    CHECK(mi_out.has_value());
    CHECK(mi_out->seq == 42 && mi_out->state_flags == 0x5);
    CHECK(mi_out->x == 64.0f && mi_out->y == 65.5f && mi_out->z == 0.25f);
    CHECK(std::fabs(mi_out->orientation - 1.5708f) < 1e-6f);
    CHECK(mi_out->client_time_ms == 123456789ull);

    // MovementState (IF-2).
    codec::MovementState ms;
    ms.entity_guid = 0xABCDEF01ull;
    ms.ack_seq = 42;
    ms.state_flags = 0x1;
    ms.x = 74.0f;
    ms.y = 65.0f;
    ms.z = 0.0f;
    ms.orientation = 0.0f;
    ms.server_time_ms = 987654321ull;
    auto ms_buf = codec::encode_movement_state(ms);
    auto ms_out = codec::decode_movement_state(ms_buf);
    CHECK(ms_out.has_value());
    CHECK(ms_out->entity_guid == 0xABCDEF01ull && ms_out->ack_seq == 42);
    CHECK(ms_out->x == 74.0f && ms_out->server_time_ms == 987654321ull);

    // Disconnect (IF-2) — reason + message round-trip; empty message survives.
    codec::Disconnect dc{/*reason=*/3, "grant invalid"};
    auto dc_buf = codec::encode_disconnect(dc);
    auto dc_out = codec::decode_disconnect(dc_buf);
    CHECK(dc_out.has_value());
    CHECK(dc_out->reason == 3 && dc_out->message == "grant invalid");
    auto dc_empty = codec::decode_disconnect(codec::encode_disconnect({4, ""}));
    CHECK(dc_empty.has_value() && dc_empty->reason == 4 && dc_empty->message.empty());

    // A garbage buffer is rejected by the verifier (never GetRoot on bad bytes).
    Bytes garbage = bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01, 0x02, 0x03});
    CHECK(!codec::decode_movement_intent(garbage).has_value());
    CHECK(!codec::decode_client_hello(garbage).has_value());
}

// ---------------------------------------------------------------------------
// 5. Full client stack composition (offline)
// ---------------------------------------------------------------------------
void test_full_stack() {
    std::puts("[stack] payload -> IF-2 frame -> length-prefix -> deframe -> decode");

    codec::MovementIntent mi;
    mi.seq = 7;
    mi.state_flags = 0x2;
    mi.x = 10.0f;
    mi.y = 20.0f;
    mi.z = 30.0f;
    mi.orientation = 0.75f;
    mi.client_time_ms = 555ull;

    // Outbound: encode payload, wrap in IF-2 frame, length-prefix for the wire.
    Bytes payload = codec::encode_movement_intent(mi);
    Bytes if2 = encode_world_frame(kOpMovementIntent, mi.seq, payload);
    auto wire = frame_message(if2);
    CHECK(wire.has_value());

    // Inbound: a stream reader gets the bytes (in two chunks to exercise reassembly),
    // deframes the transport frame, decodes the IF-2 header, decodes the payload.
    FrameReader r;
    const std::size_t split = wire->size() / 2;
    r.feed(wire->data(), split);
    CHECK(!r.next().has_value());  // only half the frame so far
    r.feed(wire->data() + split, wire->size() - split);
    auto got_body = r.next();
    CHECK(got_body.has_value());

    auto frame = decode_world_frame(*got_body);
    CHECK(frame.has_value());
    CHECK(frame->opcode == kOpMovementIntent && frame->seq == 7);

    auto mi_out = codec::decode_movement_intent(frame->payload);
    CHECK(mi_out.has_value());
    CHECK(mi_out->seq == 7 && mi_out->state_flags == 0x2);
    CHECK(mi_out->x == 10.0f && mi_out->y == 20.0f && mi_out->z == 30.0f);
    CHECK(std::fabs(mi_out->orientation - 0.75f) < 1e-6f);
    CHECK(mi_out->client_time_ms == 555ull);
}

// A trivial compile+link check that the transport types are usable as ITransport
// (they need a live server to run, so we only construct against an unreachable host
// to prove the API links; ok() is false, which is the expected offline outcome).
void test_transport_links() {
    std::puts("[transport] TLS client constructs + reports offline cleanly");
    TlsClientTransport tls("127.0.0.1", 1);  // nothing is listening on port 1
    CHECK(!tls.ok());                          // connect/handshake must fail cleanly
    CHECK(!tls.error().empty());               // and surface a reason
    ITransport* as_iface = &tls;               // usable behind the seam
    bool would_block = false;
    CHECK(!as_iface->recv_frame_nb(would_block).has_value());
}

}  // namespace

int main() {
    std::puts("=== meridian-clientnet unit tests (#95) ===");
    test_framing();
    test_wire_frame();
    test_world_session();
    test_codec();
    test_full_stack();
    test_transport_links();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::puts("ALL PASS");
        return 0;
    }
    std::puts("FAILURES PRESENT");
    return 1;
}
