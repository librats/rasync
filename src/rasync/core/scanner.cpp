#include "core/scanner.h"

#include "util/fs.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace rasync {
namespace {

/// What one directory entry is, beyond what `list_directory` reports. Both facts
/// come from a single *no-follow* look at the entry itself: `list_directory` uses
/// `stat`, which reports a symlink as whatever it points at, and the executable
/// bit has to be read off the file's own mode anyway.
struct EntryProbe {
    bool     symlink = false;
    uint32_t mode    = 0;  ///< portable permission bits (see FileMeta::kExecutable)
};

EntryProbe probe_entry(const std::string& abs_path) {
    EntryProbe p;
#ifndef _WIN32
    struct stat st{};
    if (::lstat(abs_path.c_str(), &st) != 0) return p;
    p.symlink = S_ISLNK(st.st_mode);
    // Owner-executable bit, POSIX only (Windows has no such concept for files).
    if (!p.symlink && (st.st_mode & S_IXUSR)) p.mode = FileMeta::kExecutable;
#else
    // No executable bit worth carrying on Windows, but it does have symlinks and
    // directory junctions — both reparse points, both reported as a symlink here,
    // and a junction loops the walk just as effectively as a symlink would.
    std::error_code ec;
    p.symlink = std::filesystem::is_symlink(std::filesystem::symlink_status(abs_path, ec));
#endif
    return p;
}

std::string join_rel(const std::string& base, const std::string& name) {
    return base.empty() ? name : base + "/" + name;
}

} // namespace

Manifest Scanner::scan(const std::string& root,
                       const Manifest* previous,
                       ScanStats* stats) const {
    Manifest out;
    ScanStats local;

    // Explicit work stack (relative dir paths) instead of recursion, so an
    // arbitrarily deep tree can't overflow the C++ stack.
    std::vector<std::string> pending;
    pending.push_back("");  // the root itself

    while (!pending.empty()) {
        std::string rel_dir = std::move(pending.back());
        pending.pop_back();

        std::string abs_dir = rel_dir.empty() ? root : librats::combine_paths(root, rel_dir);
        std::vector<librats::DirectoryEntry> entries;
        if (!librats::list_directory(abs_dir.c_str(), entries)) continue;

        // Deterministic order keeps behaviour reproducible across platforms.
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });

        for (const auto& e : entries) {
            if (e.name == "." || e.name == "..") continue;
            const std::string rel = join_rel(rel_dir, e.name);
            const std::string abs = librats::combine_paths(root, rel);

            // A symlink is skipped wherever it appears — the probe comes after the
            // ignore check so an excluded subtree costs no extra syscalls.
            if (e.is_directory) {
                ++local.dirs_seen;
                if (ignore_.matches(rel, /*is_dir=*/true)) continue;
                // Descending here is what a link back into the tree (or a Windows
                // junction) turns into an endless walk: loop/, loop/loop/, ...
                if (probe_entry(abs).symlink) { ++local.symlinks_skipped; continue; }
                pending.push_back(rel);
                continue;
            }

            if (ignore_.matches(rel, /*is_dir=*/false)) continue;
            const EntryProbe info = probe_entry(abs);
            // A link to a file elsewhere would otherwise be hashed and sent as a
            // regular file, quietly copying data from outside the tree to the peer.
            if (info.symlink) { ++local.symlinks_skipped; continue; }
            ++local.files_seen;

            FileMeta meta;
            meta.size  = e.size;
            meta.mtime = static_cast<int64_t>(e.modified_time);
            meta.mode  = info.mode;

            // Reuse the cached hash when the cheap signal (size+mtime) is unchanged.
            const FileMeta* prev = previous ? previous->find(rel) : nullptr;
            if (prev && prev->size == meta.size && prev->mtime == meta.mtime) {
                meta.hash = prev->hash;
            } else {
                auto h = sha256_file(abs);
                if (!h) continue;  // unreadable (perms / vanished) — skip this file
                meta.hash = *h;
                ++local.files_hashed;
                local.bytes_hashed += meta.size;
            }

            out.set(rel, meta);
        }
    }

    if (stats) *stats = local;
    return out;
}

} // namespace rasync
