#pragma once

/**
 * @file delta.h
 * @brief rsync-style rolling-checksum delta: send only the bytes that changed.
 *
 * When a peer already has an older version of a file, rasync transfers just the
 * difference instead of the whole file. The classic rsync algorithm:
 *
 *   1. The receiver splits its copy (the "base") into fixed-size blocks and sends
 *      a Signature: a fast rolling checksum + a strong SHA-256 (truncated) per
 *      block.
 *   2. The sender slides a byte-at-a-time window over its newer file, cheaply
 *      testing each position's rolling checksum against the signature, and only
 *      confirming a hit with the strong hash. A confirmed hit becomes a COPY
 *      instruction (reuse the receiver's own bytes); unmatched runs become
 *      LITERAL bytes actually sent.
 *   3. The receiver reconstructs the new file from COPY (read from its base) and
 *      LITERAL (from the wire) instructions.
 *
 * The whole module is pure and buffer-oriented, so it is unit-tested in isolation:
 * `apply(base, compute_delta(signature(base), target)) == target` for any inputs.
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/serialize.h"

namespace rasync {

/// Default block size. Smaller ⇒ finer-grained matching but a larger signature;
/// 2 KiB is a good balance for source trees and documents.
constexpr uint32_t kDefaultBlockSize = 2048;

/// One base block's fingerprints. `strong` is the first 16 bytes of the block's
/// SHA-256 — a false match is astronomically unlikely and, if it ever happened,
/// the end-to-end whole-file hash check would catch it and force a full resend.
struct BlockSig {
    uint32_t                 weak = 0;
    std::array<uint8_t, 16>  strong{};
};

struct Signature {
    uint32_t              block_size = kDefaultBlockSize;
    uint64_t              file_size  = 0;
    std::vector<BlockSig> blocks;

    void  encode(BinaryWriter& w) const;
    Bytes encode() const;
    static std::optional<Signature> decode(BinaryReader& r);
    static std::optional<Signature> decode(const Bytes& data);
};

/// One reconstruction instruction: either COPY `len` bytes from the base at
/// `offset`, or append the `literal` bytes.
struct DeltaOp {
    bool     copy = false;
    uint64_t offset = 0;  ///< COPY: byte offset into the base file
    uint32_t len = 0;     ///< COPY: length
    Bytes    literal;     ///< LITERAL: bytes to append
};

/// A delta is never serialized as a unit: the sender streams its ops as
/// individual FileCopy/FileLiteral messages so a huge one never has to fit in a
/// single message (or in memory twice). Hence no codec here — unlike Signature,
/// which does travel whole, inside a Request.
struct Delta {
    uint64_t             out_size = 0;  ///< size of the reconstructed file
    std::vector<DeltaOp> ops;

    /// Bytes that must actually travel the wire (sum of literal lengths).
    uint64_t literal_bytes() const;
    /// Bytes reused from the base (sum of copy lengths).
    uint64_t copied_bytes() const;
};

/// Fast rolling (Adler-style) weak checksum of a buffer.
uint32_t weak_checksum(const uint8_t* data, size_t len);

/// Build a signature from an in-memory buffer.
Signature signature_of(const uint8_t* data, size_t size, uint32_t block_size = kDefaultBlockSize);

/// Build a signature from a file, streamed (bounded memory). nullopt if unreadable.
std::optional<Signature> signature_of_file(const std::string& path,
                                           uint32_t block_size = kDefaultBlockSize);

/// Compute a delta that turns the base described by `sig` into `target`.
Delta compute_delta(const Signature& sig, const uint8_t* target, size_t size);

/// Apply a delta in memory: reconstruct the target from `base` bytes + the delta.
/// The receiver does this incrementally as ops arrive (see net/sync_session.cpp);
/// this whole-buffer form is what pins the module's contract down in one line —
/// `apply_delta(base, compute_delta(signature_of(base), target)) == target`.
Bytes apply_delta(const uint8_t* base, size_t base_size, const Delta& delta);

} // namespace rasync
