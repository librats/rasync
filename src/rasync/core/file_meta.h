#pragma once

/**
 * @file file_meta.h
 * @brief Per-file metadata tracked in a manifest.
 *
 * A rasync manifest maps a POSIX relative path to one `FileMeta`. Content
 * identity is the SHA-256 `hash`; `size` + `mtime` are the cheap change signal
 * the scanner uses to decide whether a re-hash is even needed. `mode` carries the
 * handful of permission bits worth preserving across platforms (currently just
 * the executable bit) so a synced script stays runnable on POSIX hosts.
 */

#include <cstdint>

#include "core/hash.h"

namespace rasync {

struct FileMeta {
    uint64_t size  = 0;
    int64_t  mtime = 0;   ///< modification time, Unix seconds
    uint32_t mode  = 0;   ///< portable permission bits (see kExecutable)
    Hash     hash{};      ///< SHA-256 of file contents

    static constexpr uint32_t kExecutable = 0x1;  ///< owner-executable bit

    bool executable() const noexcept { return (mode & kExecutable) != 0; }

    /// Same *content*: identical hash and size. mtime/mode deliberately excluded —
    /// a touched-but-unchanged file must not read as different content.
    bool same_content(const FileMeta& o) const noexcept {
        return size == o.size && hash == o.hash;
    }
};

} // namespace rasync
