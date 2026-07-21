#pragma once

/**
 * @file scanner.h
 * @brief Walk a directory tree into a Manifest, hashing incrementally.
 *
 * The scanner is the change-detection engine. It recursively lists a root,
 * applies an IgnoreList (pruning ignored directories whole), and produces a
 * Manifest. Hashing dominates the cost on large trees, so the scanner takes the
 * *previous* manifest as a cache: a file whose size and mtime are unchanged
 * reuses its prior hash instead of being re-read. A steady-state rescan of a big
 * tree therefore touches metadata only and hashes nothing — cheap enough to poll.
 */

#include <cstdint>
#include <string>

#include "core/ignore.h"
#include "core/manifest.h"

namespace rasync {

struct ScanStats {
    uint64_t files_seen   = 0;  ///< regular files considered (post-ignore)
    uint64_t files_hashed = 0;  ///< files actually read + hashed this scan
    uint64_t bytes_hashed = 0;  ///< bytes read for hashing this scan
    uint64_t dirs_seen    = 0;
};

class Scanner {
public:
    explicit Scanner(IgnoreList ignore = {}) : ignore_(std::move(ignore)) {}

    const IgnoreList& ignore() const noexcept { return ignore_; }

    /// Scan `root` into a manifest. If `previous` is non-null it is used as a
    /// hash cache (size+mtime match ⇒ reuse hash). `stats` (optional) receives
    /// work counters. A path that vanishes mid-scan is simply skipped.
    Manifest scan(const std::string& root,
                  const Manifest* previous = nullptr,
                  ScanStats* stats = nullptr) const;

private:
    IgnoreList ignore_;
};

} // namespace rasync
