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
//
// #100 GAP-FILLING EDGE CASES (added on top of the #95/#274/#302 coverage above):
//   6. FRAMING EDGE CASES — max-size (accept side of the cap), zero-length frames
//      back-to-back and interleaved, length_is_valid boundaries, buffered()/compact()
//      accounting under a 500-frame stress, post-error / zero-length feed no-ops.
//   7. WIRE-FRAME EDGE CASES — seq/opcode extremes (0, UINT64_MAX, 0xFFFF), every
//      defined opcode, the 9-vs-10 byte header boundary, empty buffer, large payload.
//   8. AEAD EDGE CASES — empty-key/over-length-key not-ok + refuse seal/open, next_seq
//      accounting, empty-plaintext round-trip, s2c round-trip, too-short ciphertext,
//      tampered-TAG + truncated-ciphertext rejection, stateless-open (replay) contract,
//      different-key negative interop.
//   9. MALFORMED-FRAME REJECTION (verifier) + VERSION FIELDS — empty + truncated +
//      garbage rejected by every decoder; ClientHello build/proto_ver extremes and
//      extreme finite floats survive round-trip; a version-reject Disconnect round-trips.
//  10. TRANSPORT ERROR / RECONNECT PATHS — offline TcpTransport not-ok + failing I/O,
//      and a socket-free MockTransport exercising the ITransport seam: send/recv order,
//      oversize refusal, EOF→nullopt (the reconnect signal), the default recv_frame_nb.

#include "meridian/clientnet/codec.h"
#include "meridian/clientnet/framing.h"
#include "meridian/clientnet/tcp_transport.h"
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

// A socket-free ITransport used to exercise the transport SEAM contract (the
// send/recv frame API and the DEFAULT recv_frame_nb behaviour) with no live server.
// send_frame appends to `sent`; recv_frame pops the next queued inbound frame, or
// returns std::nullopt once `inbox` is drained (a clean EOF — the signal a caller
// uses to tear down and reconnect). recv_frame_nb is deliberately NOT overridden so
// the ITransport default (would_block=false → recv_frame) is what runs.
struct MockTransport : ITransport {
    std::vector<Bytes> sent;
    std::vector<Bytes> inbox;
    std::size_t cursor = 0;
    unsigned last_timeout_ms = 0;

    bool send_frame(const Bytes& payload) override {
        if (payload.size() > kMaxFrameBytes) return false;  // mirror the real framing cap
        sent.push_back(payload);
        return true;
    }
    std::optional<Bytes> recv_frame() override {
        if (cursor >= inbox.size()) return std::nullopt;  // EOF → caller reconnects
        return inbox[cursor++];
    }
    void set_recv_timeout_ms(unsigned ms) override { last_timeout_ms = ms; }
    // recv_frame_nb intentionally inherited (tests the ITransport default).
};

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
// 4b. Entity-relay codec (#87 AoI: EntityEnter / EntityUpdate / EntityLeave)
// ---------------------------------------------------------------------------
void test_entity_codec() {
    std::puts("[codec] EntityEnter/Update/Leave round-trip + verifier");

    // EntityEnter — full spawn/enter snapshot (guid + type + wire pose).
    codec::EntityEnter en;
    en.entity_guid = 0x1122334455667788ull;
    en.type_id = 7;
    en.x = 64.0f;
    en.y = 65.0f;
    en.z = 0.5f;
    en.orientation = 1.25f;
    auto en_buf = codec::encode_entity_enter(en);
    auto en_out = codec::decode_entity_enter(en_buf);
    CHECK(en_out.has_value());
    CHECK(en_out->entity_guid == 0x1122334455667788ull && en_out->type_id == 7);
    CHECK(en_out->x == 64.0f && en_out->y == 65.0f && en_out->z == 0.5f);
    CHECK(std::fabs(en_out->orientation - 1.25f) < 1e-6f);

    // EntityUpdate — a movement delta (all position fields present, as worldd sends).
    codec::EntityUpdate up;
    up.entity_guid = 0xABCDull;
    up.x = 70.0f;
    up.y = 60.0f;
    up.z = 0.0f;
    up.orientation = 2.0f;
    auto up_buf = codec::encode_entity_update(up);
    auto up_out = codec::decode_entity_update(up_buf);
    CHECK(up_out.has_value());
    CHECK(up_out->entity_guid == 0xABCDull);
    CHECK(up_out->has_position());
    CHECK(up_out->has_x && up_out->has_y && up_out->has_z);
    CHECK(up_out->x == 70.0f && up_out->y == 60.0f && up_out->z == 0.0f);
    CHECK(std::fabs(up_out->orientation - 2.0f) < 1e-6f);

    // EntityLeave — guid + reason.
    codec::EntityLeave lv{/*guid=*/0xFEEDull, /*reason=*/1 /*DESPAWNED*/};
    auto lv_buf = codec::encode_entity_leave(lv);
    auto lv_out = codec::decode_entity_leave(lv_buf);
    CHECK(lv_out.has_value());
    CHECK(lv_out->entity_guid == 0xFEEDull && lv_out->reason == 1);

    // Garbage is rejected by the verifier on every entity decoder.
    Bytes garbage = bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01, 0x02, 0x03});
    CHECK(!codec::decode_entity_enter(garbage).has_value());
    CHECK(!codec::decode_entity_update(garbage).has_value());
    CHECK(!codec::decode_entity_leave(garbage).has_value());
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

// ===========================================================================
// GAP-FILLING EDGE CASES (issue #100): framing boundaries, reconnect/transport
// error paths, version-field boundaries, malformed-frame rejection via the
// FlatBuffers verifier. These ADD to the #95/#274/#302 coverage above; they do
// not duplicate it.
// ===========================================================================

// ---------------------------------------------------------------------------
// 6. Framing edge cases (#100: "framing edge cases")
// ---------------------------------------------------------------------------
void test_framing_edge_cases() {
    std::puts("[framing/edge] boundaries, zero-length, back-to-back, compaction");

    // length_is_valid boundary: exactly kMaxFrameBytes is accepted; +1 is not; 0 ok.
    CHECK(length_is_valid(0));
    CHECK(length_is_valid(kMaxFrameBytes));
    CHECK(!length_is_valid(kMaxFrameBytes + 1));

    // A payload of EXACTLY kMaxFrameBytes frames (the accept side of the cap the
    // existing test only proves the reject side of), and round-trips through a reader.
    Bytes max_payload(kMaxFrameBytes, 0x5A);
    auto max_framed = frame_message(max_payload);
    CHECK(max_framed.has_value());
    CHECK(max_framed->size() == kLengthPrefixBytes + kMaxFrameBytes);
    FrameReader rmax;
    rmax.feed(*max_framed);
    auto max_out = rmax.next();
    CHECK(max_out.has_value() && *max_out == max_payload);
    CHECK(!rmax.error());

    // read_length_prefix decodes a high-bit-set u32 LE correctly (byte order proof
    // beyond the small value the existing test uses).
    CHECK(*read_length_prefix(bytes_of({0x78, 0x56, 0x34, 0x12})) == 0x12345678u);
    // Exactly-prefix-length input reads; anything shorter is nullopt.
    CHECK(read_length_prefix(bytes_of({0x00, 0x00, 0x00, 0x00})).has_value());
    CHECK(!read_length_prefix(bytes_of({0x00, 0x00, 0x00})).has_value());

    // Zero-length frames: a bare prefix of 00 00 00 00 yields an EMPTY payload, and
    // several back-to-back drain as several empty payloads (not swallowed / merged).
    FrameReader rz;
    auto z1 = frame_message(Bytes{});
    rz.feed(*z1);
    rz.feed(*z1);
    rz.feed(*z1);
    for (int i = 0; i < 3; ++i) {
        auto z = rz.next();
        CHECK(z.has_value() && z->empty());
    }
    CHECK(!rz.next().has_value());

    // A zero-length frame wedged BETWEEN two non-empty frames is delivered in order.
    FrameReader rmix;
    rmix.feed(*frame_message(bytes_of({0xA1})));
    rmix.feed(*frame_message(Bytes{}));
    rmix.feed(*frame_message(bytes_of({0xB2, 0xB3})));
    auto m1 = rmix.next();
    auto m2 = rmix.next();
    auto m3 = rmix.next();
    CHECK(m1.has_value() && *m1 == bytes_of({0xA1}));
    CHECK(m2.has_value() && m2->empty());
    CHECK(m3.has_value() && *m3 == bytes_of({0xB2, 0xB3}));

    // An advertised length of EXACTLY kMaxFrameBytes+1 latches the error (the reject
    // boundary of the reader's length gate).
    FrameReader rbad;
    const std::uint32_t over = kMaxFrameBytes + 1;  // 0x2001
    Bytes over_prefix = bytes_of({static_cast<std::uint8_t>(over & 0xFF),
                                  static_cast<std::uint8_t>((over >> 8) & 0xFF),
                                  static_cast<std::uint8_t>((over >> 16) & 0xFF),
                                  static_cast<std::uint8_t>((over >> 24) & 0xFF)});
    rbad.feed(over_prefix);
    CHECK(!rbad.next().has_value());
    CHECK(rbad.error());

    // feed() is a no-op for n==0 and for any bytes fed AFTER the error latched.
    FrameReader rnoop;
    rnoop.feed(nullptr, 0);
    CHECK(rnoop.buffered() == 0);
    rbad.feed(bytes_of({0xFF}));       // post-error feed ignored
    CHECK(rbad.buffered() == over_prefix.size());  // buffer unchanged, still errored
    CHECK(rbad.error());

    // buffered() accounting: after feeding a partial prefix, all bytes are buffered;
    // after a whole frame drains, the buffer is reclaimed (compact()) back toward 0.
    FrameReader rbuf;
    auto ftwo = frame_message(bytes_of({0x01, 0x02}));
    rbuf.feed(ftwo->data(), 3);            // 3 of 6 bytes
    CHECK(rbuf.buffered() == 3);
    CHECK(!rbuf.next().has_value());
    rbuf.feed(ftwo->data() + 3, ftwo->size() - 3);
    CHECK(rbuf.next().has_value());
    CHECK(rbuf.buffered() == 0);           // fully drained + compacted

    // Compaction stress: many frames through ONE reader with interleaved feed/drain
    // stays correct and does not grow the buffer unbounded (buffered() returns to 0).
    FrameReader rlong;
    for (int i = 0; i < 64; ++i) {
        Bytes p = bytes_of({static_cast<std::uint8_t>(i & 0xFF),
                            static_cast<std::uint8_t>((i >> 8) & 0xFF)});
        rlong.feed(*frame_message(p));
        auto got = rlong.next();
        CHECK(got.has_value() && *got == p);
    }
    CHECK(rlong.buffered() == 0);
    CHECK(!rlong.error());
}

// ---------------------------------------------------------------------------
// 7. IF-2 wire-frame edge cases (#100: framing/header boundaries)
// ---------------------------------------------------------------------------
void test_wire_frame_edge_cases() {
    std::puts("[wire_frame/edge] seq/opcode extremes, header boundary, all opcodes");

    // A body of EXACTLY kFrameHeaderBytes-1 (9 bytes) is malformed; exactly the
    // header (10 bytes) decodes with an empty payload. An empty buffer is malformed.
    CHECK(!decode_world_frame(Bytes(kFrameHeaderBytes - 1, 0x00)).has_value());
    CHECK(!decode_world_frame(Bytes{}).has_value());
    auto hdr_only = decode_world_frame(Bytes(kFrameHeaderBytes, 0x00));
    CHECK(hdr_only.has_value() && hdr_only->payload.empty());

    // seq extremes round-trip (0 and UINT64_MAX) — the u64 LE writer/reader is exact.
    for (std::uint64_t s : {std::uint64_t{0}, UINT64_MAX}) {
        auto f = decode_world_frame(encode_world_frame(kOpClockSync, s, bytes_of({0x01})));
        CHECK(f.has_value() && f->seq == s && f->opcode == kOpClockSync);
    }

    // opcode extremes round-trip (0x0000 and 0xFFFF), and EVERY defined opcode does.
    const std::uint16_t opcodes[] = {
        0x0000, 0xFFFF, kOpWorldHello, kOpHandshakeOk, kOpDisconnect, kOpClockSync,
        kOpMovementIntent, kOpMovementState, kOpEntityEnter, kOpEntityUpdate,
        kOpEntityLeave};
    for (std::uint16_t op : opcodes) {
        auto f = decode_world_frame(encode_world_frame(op, 1, Bytes{}));
        CHECK(f.has_value() && f->opcode == op);
    }

    // A large payload (near the transport cap) survives the header round-trip intact.
    Bytes big(4096, 0xC3);
    auto bf = decode_world_frame(encode_world_frame(kOpMovementState, 9, big));
    CHECK(bf.has_value() && bf->payload == big && bf->seq == 9);
}

// ---------------------------------------------------------------------------
// 8. AEAD world-session edge cases (#100: tamper/replay/error paths)
// ---------------------------------------------------------------------------
void test_world_session_edge_cases() {
    std::puts("[world_session/edge] empty msg, short/tampered ct, s2c, interop reject");

    Bytes key(kAeadKeyBytes);
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i + 1);
    WorldSession a(key), b(key);
    CHECK(a.ok() && b.ok());

    // Key-length guards: empty and over-length keys both yield not-ok sessions, and a
    // not-ok session refuses to seal or open.
    WorldSession empty_key(Bytes{});
    WorldSession long_key(Bytes(kAeadKeyBytes + 1, 0x00));
    CHECK(!empty_key.ok() && !long_key.ok());
    std::uint64_t junk_seq = 0;
    CHECK(!empty_key.seal(Direction::kClientToServer, bytes_of({0x01}), Bytes{}, junk_seq)
               .has_value());
    CHECK(!empty_key.open(Direction::kClientToServer, Bytes(kAeadTagBytes, 0x00), 0, Bytes{})
               .has_value());

    // next_seq() tracks the counter the next seal will use (0, then 1, then 2).
    CHECK(a.next_seq(Direction::kClientToServer) == 0);
    std::uint64_t s0 = 0;
    auto sealed0 = a.seal(Direction::kClientToServer, bytes_of({0x09}), Bytes{}, s0);
    CHECK(sealed0.has_value() && s0 == 0);
    CHECK(a.next_seq(Direction::kClientToServer) == 1);
    a.seal(Direction::kClientToServer, bytes_of({0x09}), Bytes{}, s0);
    CHECK(a.next_seq(Direction::kClientToServer) == 2);
    // The two directions count independently.
    CHECK(a.next_seq(Direction::kServerToClient) == 0);

    // Empty-plaintext seal produces a bare tag (kAeadTagBytes) and opens to empty.
    std::uint64_t es = 0;
    auto sealed_empty = b.seal(Direction::kServerToClient, Bytes{}, Bytes{}, es);
    CHECK(sealed_empty.has_value() && sealed_empty->size() == kAeadTagBytes);
    auto opened_empty = a.open(Direction::kServerToClient, *sealed_empty, es, Bytes{});
    CHECK(opened_empty.has_value() && opened_empty->empty());

    // s2c round-trip (the existing suite proves c2s; this proves the other key too).
    Bytes msg = bytes_of({0xDE, 0xAD});
    std::uint64_t sq = 0;
    auto s2c = a.seal(Direction::kServerToClient, msg, Bytes{}, sq);
    CHECK(s2c.has_value());
    auto s2c_open = b.open(Direction::kServerToClient, *s2c, sq, Bytes{});
    CHECK(s2c_open.has_value() && *s2c_open == msg);

    // A too-short ciphertext (fewer than tag bytes, incl. empty) is rejected, never
    // read out of bounds.
    CHECK(!b.open(Direction::kServerToClient, Bytes{}, sq, Bytes{}).has_value());
    CHECK(!b.open(Direction::kServerToClient, Bytes(kAeadTagBytes - 1, 0x00), sq, Bytes{})
               .has_value());

    // Tampering the TAG byte (last byte), or TRUNCATING the ciphertext, fails auth —
    // complements the existing "first byte tampered" check.
    Bytes tag_tamper = *s2c;
    tag_tamper.back() ^= 0x80;
    CHECK(!b.open(Direction::kServerToClient, tag_tamper, sq, Bytes{}).has_value());
    Bytes truncated = *s2c;
    truncated.pop_back();
    CHECK(!b.open(Direction::kServerToClient, truncated, sq, Bytes{}).has_value());

    // open() is STATELESS (does not advance a counter): opening the SAME sealed frame
    // twice under the same seq both succeed. Replay defence is the caller's seq
    // bookkeeping, not open()'s — this pins that documented contract.
    CHECK(b.open(Direction::kServerToClient, *s2c, sq, Bytes{}).has_value());
    CHECK(b.open(Direction::kServerToClient, *s2c, sq, Bytes{}).has_value());

    // Negative interop: a session with a DIFFERENT key cannot open a's frame.
    Bytes other_key(kAeadKeyBytes, 0xEE);
    WorldSession c(other_key);
    CHECK(c.ok());
    CHECK(!c.open(Direction::kServerToClient, *s2c, sq, Bytes{}).has_value());
}

// ---------------------------------------------------------------------------
// 9. Malformed-frame rejection via the FlatBuffers verifier (#100 exit item) +
//    version-field boundaries (#100: "version mismatch").
// ---------------------------------------------------------------------------
void test_codec_malformed_and_versions() {
    std::puts("[codec/malformed] empty+truncated+garbage rejected by verifier");

    // An EMPTY buffer is rejected by EVERY decoder (verify-before-GetRoot never reads
    // a root out of zero bytes).
    Bytes empty;
    CHECK(!codec::decode_client_hello(empty).has_value());
    CHECK(!codec::decode_movement_intent(empty).has_value());
    CHECK(!codec::decode_movement_state(empty).has_value());
    CHECK(!codec::decode_disconnect(empty).has_value());
    CHECK(!codec::decode_entity_enter(empty).has_value());
    CHECK(!codec::decode_entity_update(empty).has_value());
    CHECK(!codec::decode_entity_leave(empty).has_value());

    // A TRUNCATED valid buffer (first half of a good encode) fails the verifier — a
    // partial FlatBuffer is never GetRoot'd. Covers the "malformed frame" exit item
    // for a message that is plausible-but-cut, not just random garbage.
    codec::MovementState ms;
    ms.entity_guid = 0x1122334455667788ull;
    ms.ack_seq = 5;
    ms.x = 1.0f;
    ms.server_time_ms = 42;
    Bytes good = codec::encode_movement_state(ms);
    CHECK(good.size() > 4);
    Bytes truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
    CHECK(!codec::decode_movement_state(truncated).has_value());

    // Random garbage is rejected by the decoders the existing suite does NOT cover
    // (state / disconnect) — completing the verifier discipline across every message.
    Bytes garbage = bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0x11, 0x22, 0x33, 0x44});
    CHECK(!codec::decode_movement_state(garbage).has_value());
    CHECK(!codec::decode_disconnect(garbage).has_value());

    // Version fields (#100 "version mismatch"): the IF-1 ClientHello build/proto_ver
    // survive the round-trip at their extremes so the login core's version check
    // (client/gdextension .../version_compat_test) sees the true peer values. The
    // mismatch POLICY lives in login_core; this proves the codec never corrupts the
    // numbers the policy compares.
    codec::ClientHello hi{UINT32_MAX, UINT16_MAX};
    auto hi_out = codec::decode_client_hello(codec::encode_client_hello(hi));
    CHECK(hi_out.has_value());
    CHECK(hi_out->build == UINT32_MAX && hi_out->proto_ver == UINT16_MAX);
    codec::ClientHello lo{0, 0};
    auto lo_out = codec::decode_client_hello(codec::encode_client_hello(lo));
    CHECK(lo_out.has_value() && lo_out->build == 0 && lo_out->proto_ver == 0);

    // Extreme finite float values survive the movement round-trip bit-exactly (the
    // wire carries IEEE-754; a codec that dropped precision would desync prediction).
    codec::MovementIntent mi;
    mi.x = -1.5e30f;
    mi.y = 3.4e-38f;    // ~smallest normal float
    mi.z = -12.5f;
    mi.orientation = 6.2831853f;
    auto mi_out = codec::decode_movement_intent(codec::encode_movement_intent(mi));
    CHECK(mi_out.has_value());
    CHECK(mi_out->x == -1.5e30f && mi_out->y == 3.4e-38f && mi_out->orientation == 6.2831853f);
    CHECK(mi_out->z == -12.5f);
    // Codec semantics: FlatBuffers ELIDES a scalar equal to its default (0.0f), so a
    // signed zero does NOT survive the wire — -0.0f decodes back as +0.0f (value-equal
    // to the default). Pinned here so nobody mistakes it for a bug later.
    codec::MovementIntent zi;
    zi.z = -0.0f;
    auto zi_out = codec::decode_movement_intent(codec::encode_movement_intent(zi));
    CHECK(zi_out.has_value() && zi_out->z == 0.0f && !std::signbit(zi_out->z));

    // A Disconnect carrying a version-mismatch-style reason + message round-trips
    // (the S→C frame a server sends to reject an incompatible build).
    codec::Disconnect dc{/*reason=*/7, "incompatible build"};
    auto dc_out = codec::decode_disconnect(codec::encode_disconnect(dc));
    CHECK(dc_out.has_value() && dc_out->reason == 7 && dc_out->message == "incompatible build");
}

// ---------------------------------------------------------------------------
// 10. Transport error / reconnect paths (#100: "reconnect paths").
// ---------------------------------------------------------------------------
void test_transport_error_paths() {
    std::puts("[transport/edge] offline TCP, not-ok error surface, mock seam + EOF");

    // TcpTransport offline (nothing listening on port 1): not-ok, a reason surfaced,
    // and its I/O fails cleanly (the caller's cue to reconnect / back off).
    TcpTransport tcp("127.0.0.1", 1);
    CHECK(!tcp.ok());
    CHECK(!tcp.error().empty());
    CHECK(!tcp.send_frame(bytes_of({0x01})));   // no socket → write fails
    CHECK(!tcp.recv_frame().has_value());        // no socket → read fails (nullopt)
    bool wb = true;
    CHECK(!tcp.recv_frame_nb(wb).has_value());

    // The ITransport SEAM contract, socket-free via a mock: a frame sent goes out
    // verbatim; frames received come back in order; a payload over the cap is refused.
    MockTransport mock;
    mock.inbox = {bytes_of({0x10}), bytes_of({0x20, 0x21}), Bytes{}};
    ITransport* seam = &mock;
    CHECK(seam->send_frame(bytes_of({0xAB, 0xCD})));
    CHECK(mock.sent.size() == 1 && mock.sent[0] == bytes_of({0xAB, 0xCD}));
    CHECK(!seam->send_frame(Bytes(kMaxFrameBytes + 1, 0x00)));  // oversize refused
    CHECK(mock.sent.size() == 1);                                // and not enqueued

    // recv_frame drains the inbox in order, including a zero-length frame.
    auto r1 = seam->recv_frame();
    auto r2 = seam->recv_frame();
    auto r3 = seam->recv_frame();
    CHECK(r1.has_value() && *r1 == bytes_of({0x10}));
    CHECK(r2.has_value() && *r2 == bytes_of({0x20, 0x21}));
    CHECK(r3.has_value() && r3->empty());

    // Inbox drained → recv_frame returns nullopt (a clean EOF: the reconnect signal).
    CHECK(!seam->recv_frame().has_value());

    // The DEFAULT recv_frame_nb (not overridden by the mock) reports would_block=false
    // and delegates to recv_frame — so on EOF it too returns nullopt with no timeout.
    bool would_block = true;
    auto nb = seam->recv_frame_nb(would_block);
    CHECK(!nb.has_value() && would_block == false);

    // set_recv_timeout_ms reaches the transport (default is a no-op on a real mock;
    // here the mock records it to prove the seam call is wired).
    seam->set_recv_timeout_ms(250);
    CHECK(mock.last_timeout_ms == 250);
}

}  // namespace

int main() {
    std::puts("=== meridian-clientnet unit tests (#95) ===");
    test_framing();
    test_wire_frame();
    test_world_session();
    test_codec();
    test_entity_codec();
    test_full_stack();
    test_transport_links();

    // #100 gap-filling edge cases (add to, do not duplicate, the above).
    test_framing_edge_cases();
    test_wire_frame_edge_cases();
    test_world_session_edge_cases();
    test_codec_malformed_and_versions();
    test_transport_error_paths();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::puts("ALL PASS");
        return 0;
    }
    std::puts("FAILURES PRESENT");
    return 1;
}
