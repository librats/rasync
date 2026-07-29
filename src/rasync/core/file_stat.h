#pragma once

/**
 * @file file_stat.h
 * @brief What a path *stands for*: metadata with symbolic links resolved.
 *
 * Everything a manifest records about a file besides its content — size, mtime,
 * the portable permission bits — has to describe the file itself, and with
 * `--follow-symlinks` the entry naming it may be a link. `stat()` is the obvious
 * way to ask and the wrong one on Windows: the C runtimes disagree about whether
 * it follows a reparse point at all. UCRT (MSVC) answers out of
 * `GetFileAttributesEx`, which reports the *link* — size 0 and the link's own
 * mtime — while MinGW's opens a handle and reports the target. A build that
 * followed links therefore advertised zero-length files to the peer on one
 * toolchain and the right ones on the other. librats' `get_file_size` inherits
 * the same split, and on Windows goes through 32-bit `_stat` besides, which
 * cannot express a size past 2 GiB.
 *
 * So the follow happens here, once, against the platform API instead of the CRT:
 * `::stat` on POSIX, an opened handle on Windows. One lookup, no toolchain in it.
 */

#include <cstdint>
#include <string>

namespace rasync {

struct FileStat {
    bool     exists    = false;  ///< false if the path is gone — or is a broken link
    bool     directory = false;
    uint64_t size      = 0;      ///< bytes; 0 for a directory
    int64_t  mtime     = 0;      ///< last modification, Unix seconds
    uint32_t mode      = 0;      ///< portable permission bits (see FileMeta::kExecutable)
};

/// Metadata of whatever `path` resolves to, links followed: a link reports its
/// target, and a link with no target reports `exists == false`. Costs a single
/// filesystem lookup.
FileStat stat_follow(const std::string& path);

} // namespace rasync
