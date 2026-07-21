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
 * The protocol is symmetric and pull-based: both peers exchange manifests, each
 * independently decides what to pull, and requests it. Bulk data is windowed
 * (FileAck) so a large file never trips librats' per-connection send high-water
 * mark. Files that both peers already share an older version of are sent as an
 * rsync delta (FileStart.is_delta=1, a stream of FileCopy/FileLiteral); otherwise
 * whole-file (a stream of FileLiteral). Every file ends with FileEnd carrying the
 * whole-file SHA-256, verified before the temp file is renamed into place.
 *
 * Message bodies (after the opcode byte):
 *   Hello        [u8 version][u8 mode][u8 conflict][u64 base_gen][str16 name]
 *   Manifest     <Manifest::encode>            (full snapshot of the sender's tree)
 *   Request      [str16 path][u8 has_sig][ <Signature::encode> if has_sig ]
 *   FileStart    [u64 xid][str16 path][u64 size][i64 mtime][u32 mode][u8 is_delta]
 *   FileLiteral  [u64 xid][blob32 data]        (append literal bytes to the temp)
 *   FileCopy     [u64 xid][u64 base_off][u32 len]   (copy from the receiver's base)
 *   FileEnd      [u64 xid][hash:32]            (whole-file SHA-256 of the result)
 *   FileAck      [u64 xid][u64 bytes_written]  (cumulative; drives send windowing)
 *   NotFound     [str16 path]                  (a requested path is gone)
 *   RequestsDone [u64 round]                   (I've sent every request this round)
 *   Changed      [u64 generation]              (my tree changed — start a round)
 *   Bye          (empty)
 */

#include <cstdint>
#include <string>

#include "core/serialize.h"

namespace rasync::proto {

constexpr const char* kChannel = "rasync";
constexpr uint8_t     kVersion = 1;

enum class Op : uint8_t {
    Hello        = 1,
    Manifest     = 2,
    Request      = 3,
    FileStart    = 4,
    FileLiteral  = 5,
    FileCopy     = 6,
    FileEnd      = 7,
    FileAck      = 8,
    NotFound     = 9,
    RequestsDone = 10,
    Changed      = 11,
    Bye          = 12,
};

/// Begin a message: a writer already carrying the opcode byte.
inline BinaryWriter message(Op op) {
    BinaryWriter w;
    w.u8(static_cast<uint8_t>(op));
    return w;
}

/// Human-readable opcode name (logging/diagnostics).
const char* op_name(Op op);

} // namespace rasync::proto
