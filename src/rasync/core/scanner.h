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
 *
 * Regular files only: a symlink is skipped by default, never followed. rasync has
 * no way to represent a link in a manifest, and following one would either copy
 * data from outside the synced tree to the peer or — for a link that points back
 * into the tree — walk in circles forever.
 *
 * `follow_symlinks` opts out of that: a link is then read as the thing it points
 * at, so a link to a file is scanned as a regular file and a link to a directory
 * is descended into. The peer receives ordinary files and directories and can
 * rebuild the structure on a platform with no links of its own — which is the
 * point of the option. Two costs come with it, and both are handled here rather
 * than left to the caller:
 *
 *  - **Loops.** Following turns the tree into a graph, and a graph can cycle
 *    (`link → ..`, or two directories linking to each other). The walk carries
 *    the chain of resolved directories it descended through and refuses to enter
 *    one that is already on it — the same rule `find -L` uses. Two *sibling*
 *    links to one directory are not a cycle and are both walked, so the peer
 *    genuinely sees the structure that is there.
 *  - **Broken links.** A link whose target does not exist has nothing to hash;
 *    it is skipped and counted, never synced as an empty file.
 */

#include <cstdint>
#include <string>

#include "core/ignore.h"
#include "core/manifest.h"

namespace rasync {

struct ScanStats {
    uint64_t files_seen        = 0;  ///< regular files considered (post-ignore)
    uint64_t files_hashed      = 0;  ///< files actually read + hashed this scan
    uint64_t symlinks_skipped  = 0;  ///< links passed over (not followed, not synced)
    uint64_t symlinks_followed = 0;  ///< links read as the file/directory they point at
};

class Scanner {
public:
    /// `follow_symlinks` makes a link scan as its target instead of being skipped.
    explicit Scanner(IgnoreList ignore = {}, bool follow_symlinks = false)
        : ignore_(std::move(ignore)), follow_(follow_symlinks) {}

    /// Scan `root` into a manifest. If `previous` is non-null it is used as a
    /// hash cache (size+mtime match ⇒ reuse hash). `stats` (optional) receives
    /// work counters. A path that vanishes mid-scan is simply skipped.
    Manifest scan(const std::string& root,
                  const Manifest* previous = nullptr,
                  ScanStats* stats = nullptr) const;

    bool follows_symlinks() const noexcept { return follow_; }

private:
    IgnoreList ignore_;
    bool       follow_ = false;
};

} // namespace rasync
