#pragma once

/**
 * @file file_stat.h
 * @brief What a path is, and what it stands for — asked of the OS rather than
 *        of whatever the C runtime and standard library happen to implement.
 *
 * Everything a manifest records about a file besides its content — size, mtime,
 * the portable permission bits — has to describe the file itself, and with
 * `--follow-symlinks` the entry naming it may be a link. `stat()` is the obvious
 * way to ask and the wrong one on Windows, because it is not one implementation:
 * UCRT's `common_stat` either answers *by name* (a newer fast path that never
 * opens the file and so reports the link — size 0 and the link's own mtime) or
 * opens a handle and reports the target. Which one a build gets is a property of
 * the ucrtbase it runs against, not of the code, so one binary read a followed
 * link correctly on one Windows host and as an empty file on the next. librats'
 * `get_file_size` inherits that split, and goes through a 32-bit `_off_t`
 * besides, which makes `stat` fail outright on a file past 2 GiB.
 *
 * Classification has the same problem one layer up. `std::filesystem` disagrees
 * with itself about what a link even is: MSVC gives a junction its own
 * `file_type::junction`, so `is_symlink` is false for one, and libstdc++ on
 * MinGW has no `lstat` and reports *nothing* on Windows as a link at all. A
 * junction points at its own ancestor exactly as well as a symlink does — the
 * per-user profile ships with several that do — so a walk that trusts either
 * answer descends until it runs out of memory.
 *
 * Both questions are therefore answered here, once, against the platform API:
 * `lstat` / `stat` / `realpath` on POSIX, and on Windows an opened handle plus
 * the reparse tag. `IsReparseTagNameSurrogate` is the OS's own definition of
 * "this name stands for somewhere else": it covers symlinks and junctions, and
 * leaves the reparse points that are still ordinary files — cloud placeholders,
 * deduplicated and WIM-backed files — exactly that, so a OneDrive folder does
 * not quietly turn into a tree of skipped links.
 */

#include <cstdint>
#include <string>

namespace rasync {

struct FileStat {
    bool     exists    = false;  ///< false if the path is gone — or is a broken link
    bool     link      = false;  ///< a symlink, or on Windows a junction; never set by stat_follow
    bool     directory = false;
    uint64_t size      = 0;      ///< bytes; 0 for a directory or a link
    int64_t  mtime     = 0;      ///< last modification, Unix seconds
    uint32_t mode      = 0;      ///< portable permission bits (FileMeta::kExecutable); 0 for a directory or a link
};

/// Metadata of whatever `path` resolves to, links followed: a link reports its
/// target, and a link with no target reports `exists == false`. Costs a single
/// filesystem lookup.
FileStat stat_follow(const std::string& path);

/// Metadata of the entry `path` names, links *not* followed: a link reports
/// itself, which is what lets a walk decide about it before descending. One
/// lookup for an ordinary entry, two for a reparse point.
FileStat stat_nofollow(const std::string& path);

/// `path` with every link along it resolved — a stable identity for the
/// directory it names, so that two spellings of one directory give one string.
/// Empty when it cannot be resolved (every component has to exist).
///
/// This is what the scanner's cycle detection compares, which is why it is not
/// `std::filesystem::weakly_canonical`: on Windows libstdc++ cannot resolve a
/// reparse point and hands the junction's own path straight back, so a cycle
/// through one would never be recognised.
std::string real_path(const std::string& path);

} // namespace rasync
