// tools/mcc/src/hash/blake3.h — vendored BLAKE3 reference implementation (IF-4).
//
// The Tools SAD (§2.6 / decision table "BLAKE3 for all content/artifact hashing")
// specifies BLAKE3 as the content hash for the world_manifest.content_hash column
// and the three-way content-hash tie (server DB / client .pck / source tag). This
// is a portable, dependency-free BLAKE3 implementation, vendored so mcc has no
// external hashing dep — exactly the "vendored, no OpenSSL dep in mcc" rationale
// in the SAD.
//
// PROVENANCE: a from-scratch C++ re-implementation of the BLAKE3 algorithm
// following the public BLAKE3 specification (paper + reference pseudocode,
// https://github.com/BLAKE3-team/BLAKE3-specs). BLAKE3 is published under CC0 /
// Apache-2.0 (public domain dedication). It produces the standard 32-byte BLAKE3
// digest; rendered as 64 lowercase-hex chars it is exactly the
// world_manifest.content_hash shape worldd (#89) reads + verifies
// (kContentHashHexLen = 64, is_well_formed_hash: 64 lowercase-hex).
//
// Scope used by mcc: single-threaded, incremental hashing of the canonical
// content source tree. No keyed/derive-key modes are exposed (not needed for
// IF-4); the hash-mode IV is the standard all-zero key.

#ifndef MCC_HASH_BLAKE3_H
#define MCC_HASH_BLAKE3_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mcc::hash {

// The BLAKE3 default output length (bytes) and its hex rendering length.
inline constexpr std::size_t kBlake3OutLen = 32;      // 256-bit digest
inline constexpr std::size_t kBlake3HexLen = 64;      // lowercase-hex chars

// Incremental BLAKE3 hasher (hash mode). Feed bytes with update(), then call
// finalize()/hex() once. Deterministic and byte-exact per the BLAKE3 spec.
class Blake3 {
public:
    Blake3();

    // Absorb `len` bytes from `data` into the hash state.
    void update(const void* data, std::size_t len);

    // Write the 32-byte digest to `out` (kBlake3OutLen bytes). Non-destructive:
    // may be called to snapshot the current digest; further update() then
    // finalize() continues the same stream.
    void finalize(std::uint8_t out[kBlake3OutLen]) const;

    // Convenience: the digest as 64 lowercase-hex chars (world_manifest shape).
    std::string hex() const;

private:
    // Compress a single 64-byte block of the current chunk into chunk_cv_.
    void compress_chunk_block(const std::uint8_t block[64], std::uint32_t block_len,
                              std::uint32_t flags);
    // Push a completed chunk's chaining value onto the CV stack, merging parents.
    void push_chunk_cv(const std::uint32_t cv[8], std::uint64_t total_chunks);

    std::uint32_t cv_stack_[54 * 8];  // up to 54 chaining values on the CV stack
    std::uint8_t  cv_stack_len_ = 0;  // number of CVs currently stacked

    std::uint64_t chunk_counter_ = 0;      // index of the current chunk
    std::uint8_t  buf_[64];                // partial-block buffer
    std::uint8_t  buf_len_ = 0;            // bytes currently in buf_
    std::uint32_t chunk_cv_[8];            // current chunk's running chaining value
    std::uint8_t  blocks_compressed_ = 0;  // 64-byte blocks compressed in this chunk
};

// One-shot convenience: BLAKE3 of a byte range, as 64 lowercase-hex chars.
std::string blake3_hex(const void* data, std::size_t len);

}  // namespace mcc::hash

#endif  // MCC_HASH_BLAKE3_H
