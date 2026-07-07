// tools/mcc/src/hash/blake3.cpp — vendored BLAKE3 implementation (IF-4).
//
// A from-scratch, portable C++20 implementation of BLAKE3 (hash mode), following
// the public BLAKE3 specification / reference pseudocode. See blake3.h for
// provenance/licensing. No SIMD, no threads — the pure reference algorithm,
// deterministic and byte-exact across compilers/platforms (the mcc determinism
// requirement, SAD §5).
//
// Structure (spec terms):
//   * 64-byte block, 1024-byte chunk (16 blocks), binary tree of chunks.
//   * A ChunkState compresses blocks as they fill; on completion it yields a
//     chaining value (CV).
//   * Completed chunk CVs are merged pairwise up a CV stack into parent CVs
//     (the standard "add_chunk_chaining_value" carry over the completed-chunk
//     count's trailing-zero bits).
//   * output(): the root node re-runs its final compression with the ROOT flag,
//     producing the 32-byte hash.

#include "hash/blake3.h"

#include <cstring>

namespace mcc::hash {
namespace {

// BLAKE3 IV (first 8 words of the SHA-256 IV).
constexpr std::uint32_t kIV[8] = {
    0x6A09E667UL, 0xBB67AE85UL, 0x3C6EF372UL, 0xA54FF53AUL,
    0x510E527FUL, 0x9B05688CUL, 0x1F83D9ABUL, 0x5BE0CD19UL,
};

enum Flags : std::uint32_t {
    kChunkStart = 1u << 0,
    kChunkEnd   = 1u << 1,
    kParent     = 1u << 2,
    kRoot       = 1u << 3,
};

constexpr std::uint32_t kBlockLen = 64;

constexpr std::uint8_t kMsgPermute[16] = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8,
};

inline std::uint32_t rotr32(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

inline void g(std::uint32_t st[16], int a, int b, int c, int d, std::uint32_t mx,
              std::uint32_t my) {
    st[a] = st[a] + st[b] + mx;
    st[d] = rotr32(st[d] ^ st[a], 16);
    st[c] = st[c] + st[d];
    st[b] = rotr32(st[b] ^ st[c], 12);
    st[a] = st[a] + st[b] + my;
    st[d] = rotr32(st[d] ^ st[a], 8);
    st[c] = st[c] + st[d];
    st[b] = rotr32(st[b] ^ st[c], 7);
}

void round_fn(std::uint32_t st[16], const std::uint32_t m[16]) {
    g(st, 0, 4, 8, 12, m[0], m[1]);
    g(st, 1, 5, 9, 13, m[2], m[3]);
    g(st, 2, 6, 10, 14, m[4], m[5]);
    g(st, 3, 7, 11, 15, m[6], m[7]);
    g(st, 0, 5, 10, 15, m[8], m[9]);
    g(st, 1, 6, 11, 12, m[10], m[11]);
    g(st, 2, 7, 8, 13, m[12], m[13]);
    g(st, 3, 4, 9, 14, m[14], m[15]);
}

inline std::uint32_t load32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void store32_le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

// Full compression: writes all 16 output words to `out16`. The low 8 words are
// the chaining value; all 16 are the extended-output words for the root.
void compress(const std::uint32_t cv[8], const std::uint8_t block[64],
              std::uint64_t counter, std::uint32_t block_len, std::uint32_t flags,
              std::uint32_t out16[16]) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = load32_le(block + i * 4);

    std::uint32_t st[16] = {
        cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
        kIV[0], kIV[1], kIV[2], kIV[3],
        static_cast<std::uint32_t>(counter),
        static_cast<std::uint32_t>(counter >> 32),
        block_len, flags,
    };

    std::uint32_t msg[16];
    std::memcpy(msg, m, sizeof(msg));
    for (int r = 0; r < 7; ++r) {
        round_fn(st, msg);
        if (r < 6) {
            std::uint32_t perm[16];
            for (int i = 0; i < 16; ++i) perm[i] = msg[kMsgPermute[i]];
            std::memcpy(msg, perm, sizeof(msg));
        }
    }

    for (int i = 0; i < 8; ++i) {
        out16[i] = st[i] ^ st[i + 8];
        out16[i + 8] = st[i + 8] ^ cv[i];
    }
}

// Compress a chunk-final or parent-final node with kRoot to produce 32 bytes.
void root_output(const std::uint32_t cv[8], const std::uint8_t block[64],
                 std::uint64_t counter, std::uint32_t block_len, std::uint32_t flags,
                 std::uint8_t out[32]) {
    std::uint32_t out16[16];
    compress(cv, block, counter, block_len, flags | kRoot, out16);
    for (int i = 0; i < 8; ++i) store32_le(out + i * 4, out16[i]);
}

}  // namespace

// A running chunk: accumulates up to 16 blocks (1024 bytes), tracking the flags
// and the byte offset within the chunk. Kept in the header's opaque fields.
Blake3::Blake3() {
    std::memcpy(chunk_cv_, kIV, sizeof(chunk_cv_));
    std::memset(buf_, 0, sizeof(buf_));
    std::memset(cv_stack_, 0, sizeof(cv_stack_));
}

// Compress the buffered 64-byte block into the running chunk CV.
void Blake3::compress_chunk_block(const std::uint8_t block[64], std::uint32_t block_len,
                                  std::uint32_t flags) {
    std::uint32_t f = flags;
    if (blocks_compressed_ == 0) f |= kChunkStart;
    std::uint32_t out16[16];
    compress(chunk_cv_, block, chunk_counter_, block_len, f, out16);
    std::memcpy(chunk_cv_, out16, 32);
    ++blocks_compressed_;
}

// Add a completed chunk's CV to the stack; merge up while the completed-chunk
// count (total_chunks) is even at this level (spec add_chunk_chaining_value).
void Blake3::push_chunk_cv(const std::uint32_t cv[8], std::uint64_t total_chunks) {
    std::uint32_t new_cv[8];
    std::memcpy(new_cv, cv, sizeof(new_cv));
    while ((total_chunks & 1) == 0) {
        const std::uint32_t* top = cv_stack_ + (cv_stack_len_ - 1) * 8;
        std::uint8_t block[64];
        std::memcpy(block, top, 32);
        std::memcpy(block + 32, new_cv, 32);
        std::uint32_t out16[16];
        compress(kIV, block, 0, kBlockLen, kParent, out16);
        std::memcpy(new_cv, out16, 32);
        --cv_stack_len_;
        total_chunks >>= 1;
    }
    std::memcpy(cv_stack_ + cv_stack_len_ * 8, new_cv, sizeof(new_cv));
    ++cv_stack_len_;
}

void Blake3::update(const void* data, std::size_t len) {
    const std::uint8_t* in = static_cast<const std::uint8_t*>(data);

    while (len > 0) {
        // Reference ChunkState.update order: a FULL buffered block is compressed
        // at the top of the next iteration (i.e. only when more input follows),
        // so the final block of a chunk/stream is always held for the
        // kChunkEnd/kRoot pass. Two sub-cases when the buffer is full:
        if (buf_len_ == kBlockLen) {
            if (blocks_compressed_ == 15) {
                // This full block is the 16th (last) of the chunk. The chunk is
                // complete: compress it with kChunkEnd, push its CV, start anew.
                compress_chunk_block(buf_, kBlockLen, kChunkEnd);
                buf_len_ = 0;
                std::uint32_t cv[8];
                std::memcpy(cv, chunk_cv_, sizeof(cv));
                push_chunk_cv(cv, chunk_counter_ + 1);
                std::memcpy(chunk_cv_, kIV, sizeof(chunk_cv_));
                blocks_compressed_ = 0;
                ++chunk_counter_;
            } else {
                // An ordinary interior full block of the current chunk.
                compress_chunk_block(buf_, kBlockLen, 0);
                buf_len_ = 0;
            }
        }

        // Fill the block buffer with the next slice.
        const std::uint32_t want = kBlockLen - buf_len_;
        const std::size_t take = len < want ? len : want;
        std::memcpy(buf_ + buf_len_, in, take);
        buf_len_ += static_cast<std::uint8_t>(take);
        in += take;
        len -= take;
    }
}

void Blake3::finalize(std::uint8_t out[kBlake3OutLen]) const {
    // Non-destructive: operate on copies of the running state.
    std::uint32_t chunk_cv[8];
    std::memcpy(chunk_cv, chunk_cv_, sizeof(chunk_cv));

    // The final block sits in buf_ (buf_len_ bytes), not yet folded in. BLAKE3
    // requires the final (possibly partial) block to be ZERO-PADDED to 64 bytes;
    // buf_ may still carry stale bytes from a previous full block, so pad a copy.
    std::uint8_t block[64];
    std::memcpy(block, buf_, buf_len_);
    std::memset(block + buf_len_, 0, kBlockLen - buf_len_);

    std::uint32_t final_flags = kChunkEnd;
    if (blocks_compressed_ == 0) final_flags |= kChunkStart;

    if (cv_stack_len_ == 0) {
        // Single-chunk message: this chunk IS the root.
        root_output(chunk_cv, block, chunk_counter_, buf_len_, final_flags, out);
        return;
    }

    // Multi-chunk: finish the last chunk into a CV, then merge the stack down.
    std::uint32_t out16[16];
    compress(chunk_cv, block, chunk_counter_, buf_len_, final_flags, out16);
    std::uint32_t cur_cv[8];
    std::memcpy(cur_cv, out16, 32);

    std::uint8_t idx = cv_stack_len_;
    while (idx > 0) {
        const std::uint32_t* left = cv_stack_ + (idx - 1) * 8;
        std::uint8_t parent_block[64];
        std::memcpy(parent_block, left, 32);
        std::memcpy(parent_block + 32, cur_cv, 32);
        if (idx == 1) {
            // Root parent node: kParent | kRoot output is the hash.
            root_output(kIV, parent_block, 0, kBlockLen, kParent, out);
            return;
        }
        std::uint32_t merged16[16];
        compress(kIV, parent_block, 0, kBlockLen, kParent, merged16);
        std::memcpy(cur_cv, merged16, 32);
        --idx;
    }
}

std::string Blake3::hex() const {
    std::uint8_t digest[kBlake3OutLen];
    finalize(digest);
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.resize(kBlake3HexLen);
    for (std::size_t i = 0; i < kBlake3OutLen; ++i) {
        s[i * 2] = kHex[digest[i] >> 4];
        s[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return s;
}

std::string blake3_hex(const void* data, std::size_t len) {
    Blake3 h;
    h.update(data, len);
    return h.hex();
}

}  // namespace mcc::hash
