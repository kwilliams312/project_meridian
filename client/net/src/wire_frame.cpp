// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — IF-2 world-frame codec (issue #95). Byte-identical to
// server/worldd/world_dispatch.cpp encode_frame/decode_frame and the bot's
// bot_world_session.cpp; no GPL source consulted (CONTRIBUTING.md).

#include "meridian/clientnet/wire_frame.h"

namespace meridian::clientnet {
namespace {

// little-endian scalar writers/readers (match world_dispatch.cpp put_u16/put_u64/
// get_u16/get_u64 and the bot's identical helpers).
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
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      static_cast<std::uint16_t>(p[1]) << 8);
}
std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

}  // namespace

Bytes encode_world_frame(std::uint16_t opcode, std::uint64_t seq, const Bytes& payload) {
    Bytes out;
    out.reserve(kFrameHeaderBytes + payload.size());
    put_u16(out, opcode);
    put_u64(out, seq);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<WorldFrame> decode_world_frame(const Bytes& frame) {
    if (frame.size() < kFrameHeaderBytes) return std::nullopt;
    WorldFrame f;
    f.opcode = get_u16(frame.data());
    f.seq = get_u64(frame.data() + sizeof(std::uint16_t));
    f.payload.assign(frame.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes),
                     frame.end());
    return f;
}

}  // namespace meridian::clientnet
