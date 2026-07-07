// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — length-prefixed transport framing (issue #95). Factored from
// this repo's own proven login_transport.cpp / meridian-net; no GPL source
// consulted (CONTRIBUTING.md).

#include "meridian/clientnet/framing.h"

namespace meridian::clientnet {
namespace {

// The u32-LE length prefix, exactly as login_transport.cpp send_frame writes it.
Bytes encode_length_prefix(std::uint32_t len) {
    return Bytes{static_cast<std::uint8_t>(len & 0xFF),
                 static_cast<std::uint8_t>((len >> 8) & 0xFF),
                 static_cast<std::uint8_t>((len >> 16) & 0xFF),
                 static_cast<std::uint8_t>((len >> 24) & 0xFF)};
}

std::uint32_t decode_length_prefix(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::optional<Bytes> frame_message(const Bytes& payload) {
    if (payload.size() > kMaxFrameBytes) return std::nullopt;
    Bytes frame = encode_length_prefix(static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::optional<std::uint32_t> read_length_prefix(const Bytes& buf) {
    if (buf.size() < kLengthPrefixBytes) return std::nullopt;
    return decode_length_prefix(buf.data());
}

// ---------------------------------------------------------------------------
// FrameReader
// ---------------------------------------------------------------------------

void FrameReader::feed(const std::uint8_t* data, std::size_t n) {
    if (error_ || n == 0) return;
    buf_.insert(buf_.end(), data, data + n);
}

std::optional<Bytes> FrameReader::next() {
    if (error_) return std::nullopt;

    const std::size_t avail = buf_.size() - consumed_;
    if (avail < kLengthPrefixBytes) return std::nullopt;  // not even a length yet

    const std::uint32_t len = decode_length_prefix(buf_.data() + consumed_);
    if (!length_is_valid(len)) {
        // Oversize / hostile length — the stream is corrupt. Latch the error so no
        // further frame is ever yielded (mirrors login_transport rejecting the
        // prefix before allocating).
        error_ = true;
        return std::nullopt;
    }

    if (avail - kLengthPrefixBytes < len) return std::nullopt;  // body not all here

    const std::size_t start = consumed_ + kLengthPrefixBytes;
    Bytes payload(buf_.begin() + static_cast<std::ptrdiff_t>(start),
                  buf_.begin() + static_cast<std::ptrdiff_t>(start + len));
    consumed_ = start + len;
    compact();
    return payload;
}

void FrameReader::compact() {
    // Reclaim the yielded prefix once it dominates the buffer, so a long-lived
    // reader on a busy stream does not grow unbounded. Cheap amortized: only
    // shifts when at least half the buffer is spent.
    if (consumed_ == 0) return;
    if (consumed_ >= buf_.size()) {
        buf_.clear();
        consumed_ = 0;
        return;
    }
    if (consumed_ * 2 >= buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        consumed_ = 0;
    }
}

}  // namespace meridian::clientnet
