#pragma once

/**
 * @file protocol.h
 * @brief The rasync wire protocol: opcodes and message layouts.
 *
 * All rasync traffic rides one librats application channel (`kChannel`) as
 * length-delimited binary messages — librats preserves message boundaries, so
 * each `send` surfaces as exactly one handler call. Every message is a 1-byte
 * opcode followed by an opcode-specific big-endian body (see `serialize.h`).
 *
 * The protocol is symmetric and pull-based: both peers describe their trees, each
 * independently decides what to pull, and requests it. Bulk data is windowed
 * (FileAck) so a large file never trips librats' per-connection send high-water
 * mark. Files that both peers already share an older version of are sent as an
 * rsync delta (FileStart.is_delta=1, a stream of FileCopy/FileLiteral); otherwise
 * whole-file (a stream of FileLiteral). Every file ends with FileEnd carrying the
 * whole-file SHA-256, verified before the temp file is renamed into place.
 *
 * A tree is described *incrementally*. The first ManifestUpdate of a session
 * carries the whole tree (flagged kReset); afterwards a peer sends only what
 * changed since its last update. This is what keeps a sync of N files at O(N)
 * bytes of bookkeeping instead of O(N²) — re-sending a full manifest after every
 * received file is both quadratic and, past ~100k files, larger than the
 * transport will carry. For the same reason an update that would exceed
 * kMaxUpdateBytes is split into chunks flagged kMore; the receiver buffers them
 * and applies the whole set atomically, because acting on half a tree would read
 * as "the peer deleted everything I haven't heard about yet".
 *
 * Message bodies (after the opcode byte):
 *   Hello        [u8 version][u8 mode][u8 conflict]
 *   ManifestUpdate [u8 flags]<ManifestPatch::encode>   (see kUpdate* below)
 *   Request      [str16 path][u8 has_sig][ <Signature::encode> if has_sig ]
 *   FileStart    [u64 xid][str16 path][u64 size][i64 mtime][u32 mode][u8 is_delta]
 *   FileLiteral  [u64 xid][blob32 data]        (append literal bytes to the temp)
 *   FileCopy     [u64 xid][u64 base_off][u32 len]   (copy from the receiver's base)
 *   FileEnd      [u64 xid][hash:32]            (whole-file SHA-256 of the result)
 *   FileAck      [u64 xid][u64 wire_bytes]   (cumulative *literal* bytes received;
 *                                            drives send windowing. Deliberately not
 *                                            the reconstructed size — COPY bytes never
 *                                            crossed the wire, so counting them would
 *                                            run the sender's window counter backwards.)
 *   NotFound     [str16 path]                  (a requested path is gone, or refused:
 *                                            every request is answered, so the asker
 *                                            can free the pull-window slot it holds)
 *   Bye          (empty)                       (the sender has gone inert and will
 *                                            not sync with you; stop talking to it)
 *
 * A peer whose Hello carries a different kVersion is refused outright: the versions
 * describe a tree differently, so continuing would mean each side logging a
 * malformed message per advertisement while never converging.
 */

#include <cstdint>

#include "core/serialize.h"

namespace rasync::proto {

constexpr const char* kChannel = "rasync";
/// v2: manifests are advertised incrementally (ManifestUpdate replaced the
/// full-snapshot Manifest message). v3: dropped the two opcodes and the two Hello
/// fields nothing ever sent or read. The version is the first byte of Hello, so a
/// mismatched peer is recognised and refused before anything else is parsed.
constexpr uint8_t     kVersion = 3;

/// ManifestUpdate flags.
constexpr uint8_t kUpdateReset = 0x1;  ///< replaces the receiver's whole view of us
constexpr uint8_t kUpdateMore  = 0x2;  ///< another chunk of this update follows

/// Split an update into messages no larger than this. Well under librats'
/// 8 MiB per-connection send high-water mark, so describing even a multi-million
/// file tree can never look like a slow consumer.
constexpr size_t kMaxUpdateBytes = 1u << 20;

enum class Op : uint8_t {
    Hello          = 1,
    ManifestUpdate = 2,
    Request        = 3,
    FileStart      = 4,
    FileLiteral    = 5,
    FileCopy       = 6,
    FileEnd        = 7,
    FileAck        = 8,
    NotFound       = 9,
    Bye            = 10,
};

/// Begin a message: a writer already carrying the opcode byte.
inline BinaryWriter message(Op op) {
    BinaryWriter w;
    w.u8(static_cast<uint8_t>(op));
    return w;
}

} // namespace rasync::proto
