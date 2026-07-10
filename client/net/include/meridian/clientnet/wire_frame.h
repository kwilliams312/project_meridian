// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-agnostic client net core: IF-2 world-frame codec
// (issue #95).
//
// One IF-2 message body (the bytes INSIDE one length-prefixed transport frame) is:
//
//     [ u16 opcode LE ][ u64 seq LE ][ payload (FlatBuffer table) ]
//
// This is the EXACT in-frame header the server encodes/decodes in
// server/worldd/world_dispatch.cpp (encode_frame/decode_frame,
// kFrameHeaderBytes = 2 + 8) and the bot mirrors in
// client/bot/bot_world_session.cpp (encode_world_frame/decode_world_frame). This
// header factors that same layout into the shared net core; it does NOT invent a
// new one. The opcode values are the wire identifiers from schema/net/world.fbs.

#ifndef MERIDIAN_CLIENTNET_WIRE_FRAME_H
#define MERIDIAN_CLIENTNET_WIRE_FRAME_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "meridian/clientnet/framing.h"  // Bytes

namespace meridian::clientnet {

// IF-2 opcodes (schema/net/world.fbs Opcode). Exposed so codecs, the bot, and
// tests never hard-code magic numbers.
inline constexpr std::uint16_t kOpWorldHello     = 0x0001;  // C→S
inline constexpr std::uint16_t kOpHandshakeOk    = 0x0002;  // S→C
inline constexpr std::uint16_t kOpDisconnect     = 0x0003;  // S→C
inline constexpr std::uint16_t kOpClockSync      = 0x0004;  // C↔S
// Character management (D-35 / #286) over the authenticated world session.
inline constexpr std::uint16_t kOpCharListReq    = 0x0010;  // C→S
inline constexpr std::uint16_t kOpCharListResp   = 0x0011;  // S→C
inline constexpr std::uint16_t kOpCharCreateReq  = 0x0012;  // C→S
inline constexpr std::uint16_t kOpCharCreateResp = 0x0013;  // S→C
inline constexpr std::uint16_t kOpCharDeleteReq  = 0x0014;  // C→S
inline constexpr std::uint16_t kOpCharDeleteResp = 0x0015;  // S→C
// Server-authoritative enter-world (D-35 / #341): spawn as an OWNED character.
inline constexpr std::uint16_t kOpEnterWorldReq  = 0x0016;  // C→S
inline constexpr std::uint16_t kOpEnterWorldResp = 0x0017;  // S→C
inline constexpr std::uint16_t kOpMovementIntent = 0x1001;  // C→S
inline constexpr std::uint16_t kOpMovementState  = 0x1002;  // S→C
inline constexpr std::uint16_t kOpEntityEnter    = 0x2001;  // S→C
inline constexpr std::uint16_t kOpEntityUpdate   = 0x2002;  // S→C
inline constexpr std::uint16_t kOpEntityLeave    = 0x2003;  // S→C
inline constexpr std::uint16_t kOpVitalsUpdate   = 0x2004;  // S→C  HUD delta (#430/#431)
// Combat — ability use + GCD/cast (M1 — CMB-01, D-10, #432). CAST_REQUEST is the C→S
// ability use; the server ACCEPTS (CAST_START) or REJECTS (CAST_FAILED, carrying the
// GCD-resync remainder), and the attack-table resolution arrives as CAST_RESULT.
inline constexpr std::uint16_t kOpCastRequest    = 0x3001;  // C→S
inline constexpr std::uint16_t kOpCastStart      = 0x3002;  // S→C  ACCEPT (cast_ms; 0=instant)
inline constexpr std::uint16_t kOpCastFailed     = 0x3003;  // S→C  REJECT (reason + gcd_remaining_ms)
inline constexpr std::uint16_t kOpCastResult     = 0x3004;  // S→C  attack-table resolution
// Quest state (M1 — QST-01, #371/#433). QUEST_LOG is bidirectional: C→S it REQUESTS
// the log (an empty QuestLog table body); S→C it CARRIES the log snapshot. The others
// are one C→S request paired with a typed S→C result; QUEST_PROGRESS is S→C only.
inline constexpr std::uint16_t kOpQuestAccept       = 0x4001;  // C→S
inline constexpr std::uint16_t kOpQuestAcceptResult = 0x4002;  // S→C
inline constexpr std::uint16_t kOpQuestProgress     = 0x4003;  // S→C
inline constexpr std::uint16_t kOpQuestTurnIn       = 0x4004;  // C→S
inline constexpr std::uint16_t kOpQuestTurnInResult = 0x4005;  // S→C
inline constexpr std::uint16_t kOpQuestLog          = 0x4006;  // C↔S (request / snapshot)
// NPC gossip (M1 — NPC-01/02, #372/#433). GOSSIP_HELLO opens gossip on an NPC guid;
// GOSSIP_MENU is the server-computed, state-gated option list.
inline constexpr std::uint16_t kOpGossipHello    = 0x5201;  // C→S
inline constexpr std::uint16_t kOpGossipMenu     = 0x5202;  // S→C
// Corpse looting (M1 — ITM-02, #369/#441). Open (LOOT_REQUEST → LOOT_RESPONSE), take a
// slot / the money (LOOT_TAKE → LOOT_RESULT), close (LOOT_RELEASE → LOOT_CLOSED).
inline constexpr std::uint16_t kOpLootRequest    = 0x5001;  // C→S
inline constexpr std::uint16_t kOpLootResponse   = 0x5002;  // S→C
inline constexpr std::uint16_t kOpLootTake       = 0x5003;  // C→S
inline constexpr std::uint16_t kOpLootResult     = 0x5004;  // S→C
inline constexpr std::uint16_t kOpLootRelease    = 0x5005;  // C→S
inline constexpr std::uint16_t kOpLootClosed     = 0x5006;  // S→C
// Vendor transactions (M1 — ECO-01, #370/#441). buy / sell / buyback each ride ONE
// C→S request paired with a typed S→C result; all prices/balances server-computed.
inline constexpr std::uint16_t kOpVendorBuyReq      = 0x5101;  // C→S
inline constexpr std::uint16_t kOpVendorBuyResult   = 0x5102;  // S→C
inline constexpr std::uint16_t kOpVendorSellReq     = 0x5103;  // C→S
inline constexpr std::uint16_t kOpVendorSellResult  = 0x5104;  // S→C
inline constexpr std::uint16_t kOpVendorBuybackReq  = 0x5105;  // C→S
inline constexpr std::uint16_t kOpVendorBuybackResult = 0x5106;  // S→C
// Trainer (M1 — NPC-02, #372/#441). TRAINER_LIST is pushed S→C alongside GOSSIP_MENU
// when the player opens gossip on a trainer NPC; TRAINER_LEARN → TRAINER_LEARN_RESULT.
inline constexpr std::uint16_t kOpTrainerList        = 0x5203;  // S→C
inline constexpr std::uint16_t kOpTrainerLearn       = 0x5204;  // C→S
inline constexpr std::uint16_t kOpTrainerLearnResult = 0x5205;  // S→C

// IF-2 in-frame header size: u16 opcode + u64 seq (world_dispatch.h
// kFrameHeaderBytes). A frame body shorter than this is malformed.
inline constexpr std::size_t kFrameHeaderBytes =
    sizeof(std::uint16_t) + sizeof(std::uint64_t);

// A decoded IF-2 frame: opcode + seq + payload (the FlatBuffer body). Mirrors
// server/worldd/world_dispatch.h Frame and the bot's WorldFrame.
struct WorldFrame {
    std::uint16_t opcode = 0;
    std::uint64_t seq = 0;
    Bytes payload;  // the FlatBuffer table bytes (plaintext at M0)
};

// Wrap `payload` in the IF-2 frame header: u16 opcode LE ‖ u64 seq LE ‖ payload.
// The result is the body the length-prefix transport frames on the wire.
Bytes encode_world_frame(std::uint16_t opcode, std::uint64_t seq, const Bytes& payload);

// Decode a received IF-2 frame body (a transport-frame payload) into opcode/seq/
// payload. std::nullopt if the body is shorter than the IF-2 header.
std::optional<WorldFrame> decode_world_frame(const Bytes& frame);

}  // namespace meridian::clientnet

#endif  // MERIDIAN_CLIENTNET_WIRE_FRAME_H
