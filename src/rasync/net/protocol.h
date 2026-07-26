#pragma once

/**
 * @file protocol.h
 * @brief The rasync wire protocol: the folder envelope, opcodes and message layouts.
 *
 * All rasync traffic rides one librats application channel (`kChannel`) as
 * length-delimited binary messages — librats preserves message boundaries, so
 * each `send` surfaces as exactly one handler call. Every message is
 *
 *     [u32 folder tag][u8 opcode][opcode-specific big-endian body]
 *
 * (see `serialize.h`). One connection carries every folder the two peers sync,
 * so the tag is what says which tree a message is about. It is derived from the
 * folder's *name* (`folder_tag`), not from either side's local path — the same
 * tree is `/home/me/docs` here and `D:\docs` there, and only a name both sides
 * agree on can name it. A tag never leaks into a folder it does not belong to:
 * `Hello` carries the name in full and the receiver refuses a peer whose name
 * does not match the one it holds for that tag, which is what makes a 32-bit tag
 * safe to route on.
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
 * Message bodies (after the folder tag and the opcode byte):
 *   Hello        [u8 version][u8 mode][u8 conflict][str16 folder]
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
#include <string>

#include "core/hash.h"
#include "core/serialize.h"

namespace rasync::proto {

constexpr const char* kChannel = "rasync";
/// v2: manifests are advertised incrementally (ManifestUpdate replaced the
/// full-snapshot Manifest message). v3: dropped the two opcodes and the two Hello
/// fields nothing ever sent or read. v4: one connection carries many folders, so
/// every message is prefixed with a folder tag and Hello names its folder. The
/// version is the first byte after the header, so a mismatched peer is recognised
/// and refused before anything else is parsed.
constexpr uint8_t     kVersion = 4;

/// Never a folder tag: `folder_tag()` cannot produce it. A message carrying it is
/// truncated, from another protocol, or from a future version that grew
/// node-level control traffic — none of which any folder should act on.
constexpr uint32_t kControlTag = 0;

/// Bytes every message spends before its body: the folder tag and the opcode.
constexpr size_t kHeaderBytes = sizeof(uint32_t) + 1;

/// The 32-bit routing tag for a folder name. Domain-separated from rasync's other
/// derivations over user input — the separator is a NUL, which a name may not
/// contain, so no other label+value pair can produce these bytes — and never zero.
///
/// 32 bits is small enough that two *different* names could in principle land on
/// one tag. That is why the tag is only ever a routing hint: the name itself
/// travels in Hello and the receiver checks it, so a collision costs a refused
/// session and a log line, never a file written into the wrong tree.
inline uint32_t folder_tag(const std::string& name) {
    std::string input("rasync-folder");
    input.push_back('\0');
    input += name;
    const Hash h = sha256(input);
    const uint32_t tag = (static_cast<uint32_t>(h[0]) << 24) |
                         (static_cast<uint32_t>(h[1]) << 16) |
                         (static_cast<uint32_t>(h[2]) << 8) |
                         static_cast<uint32_t>(h[3]);
    return tag == kControlTag ? 1u : tag;
}

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

/// Begin a message: a writer already carrying the folder tag and the opcode byte.
inline BinaryWriter message(uint32_t folder, Op op) {
    BinaryWriter w;
    w.u32(folder);
    w.u8(static_cast<uint8_t>(op));
    return w;
}

} // namespace rasync::proto
