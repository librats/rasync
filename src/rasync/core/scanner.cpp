#include "core/scanner.h"

#include "util/fs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace rasync {
namespace {

namespace fs = std::filesystem;

/// What one directory entry is, beyond what `list_directory` reports. The first
/// look is always a *no-follow* one: `list_directory` uses `stat`, which reports a
/// symlink as whatever it points at, and the executable bit has to be read off the
/// file's own mode anyway. When links are being followed the target is looked at
/// too, because that is what the entry then stands for — and on Windows a link's
/// own directory entry carries neither the target's size nor its mtime.
struct EntryProbe {
    bool     symlink   = false;
    bool     broken    = false;  ///< a followed link whose target isn't there
    bool     directory = false;  ///< what the target is (followed links only)
    uint64_t size      = 0;      ///< target's size  (followed links only)
    int64_t  mtime     = 0;      ///< target's mtime (followed links only)
    uint32_t mode      = 0;      ///< portable permission bits (see FileMeta::kExecutable)
};

EntryProbe probe_entry(const std::string& abs_path, bool follow) {
    EntryProbe p;
#ifndef _WIN32
    struct stat st{};
    if (::lstat(abs_path.c_str(), &st) != 0) return p;
    p.symlink = S_ISLNK(st.st_mode);
    if (p.symlink) {
        if (!follow) return p;
        // Same path, this time resolving the link: a dangling one fails here and
        // is the caller's cue to skip rather than sync a file that isn't there.
        struct stat target{};
        if (::stat(abs_path.c_str(), &target) != 0) { p.broken = true; return p; }
        st = target;
    }
    p.directory = S_ISDIR(st.st_mode);
    p.size      = static_cast<uint64_t>(st.st_size);
    p.mtime     = static_cast<int64_t>(st.st_mtime);
    // Owner-executable bit, POSIX only (Windows has no such concept for files).
    // Read off whatever the entry stands for, so a followed link carries the
    // permission of the file it points at.
    if (!p.directory && (st.st_mode & S_IXUSR)) p.mode = FileMeta::kExecutable;
#else
    // No executable bit worth carrying on Windows, but it does have symlinks and
    // directory junctions — both reparse points, both reported as a symlink here,
    // and a junction loops the walk just as effectively as a symlink would.
    std::error_code ec;
    p.symlink = fs::is_symlink(fs::symlink_status(abs_path, ec));
    if (!p.symlink || !follow) return p;
    const fs::file_status target = fs::status(abs_path, ec);  // follows
    if (ec || !fs::exists(target)) { p.broken = true; return p; }
    p.directory = fs::is_directory(target);
    if (!p.directory) {
        const int64_t size = librats::get_file_size(abs_path.c_str());
        p.size  = size < 0 ? 0 : static_cast<uint64_t>(size);
        p.mtime = static_cast<int64_t>(librats::get_file_modified_time(abs_path.c_str()));
    }
#endif
    return p;
}

std::string join_rel(const std::string& base, const std::string& name) {
    return base.empty() ? name : base + "/" + name;
}

/// A directory's identity for loop detection: its path with every link already
/// resolved, folded to lower case where the platform's paths are case-insensitive
/// — two spellings of one directory have to compare equal, or a cycle spelled
/// inconsistently would never be recognised and the walk would not terminate.
std::string canon_key(const fs::path& p) {
    std::string s = p.string();
#ifdef _WIN32
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return s;
}

/// One link in the chain of directories the walk descended through, kept as an
/// immutable list so each pending directory shares its ancestors instead of
/// copying them. Only built when following links — nothing else can cycle.
struct DirChain {
    std::string                     key;     ///< canon_key of this directory
    std::shared_ptr<const DirChain> parent;

    bool on_path(const std::string& candidate) const {
        for (const DirChain* n = this; n; n = n->parent.get())
            if (n->key == candidate) return true;
        return false;
    }
};

std::shared_ptr<const DirChain> extend(const std::shared_ptr<const DirChain>& parent,
                                       std::string key) {
    return std::make_shared<const DirChain>(DirChain{std::move(key), parent});
}

} // namespace

Manifest Scanner::scan(const std::string& root,
                       const Manifest* previous,
                       ScanStats* stats) const {
    Manifest out;
    ScanStats local;

    // Explicit work stack (relative dir paths) instead of recursion, so an
    // arbitrarily deep tree can't overflow the C++ stack. `chain` is the resolved
    // directories walked through to reach this one, and is only carried when
    // following links — without them a tree is a tree and cannot cycle.
    struct Pending {
        std::string                     rel;
        std::shared_ptr<const DirChain> chain;
    };
    std::vector<Pending> pending;
    {
        std::error_code ec;
        fs::path real = fs::weakly_canonical(fs::path(root), ec);
        if (ec || real.empty()) real = fs::path(root);
        pending.push_back({std::string(), follow_ ? extend(nullptr, canon_key(real)) : nullptr});
    }

    while (!pending.empty()) {
        Pending item = std::move(pending.back());
        pending.pop_back();
        const std::string& rel_dir = item.rel;

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

            // A symlink is skipped wherever it appears unless we were asked to
            // follow — the probe comes after the ignore check so an excluded
            // subtree costs no extra syscalls either way.
            if (e.is_directory) {
                if (ignore_.matches(rel, /*is_dir=*/true)) continue;
                const EntryProbe info = probe_entry(abs, follow_);
                if (!info.symlink) {
                    pending.push_back({rel, follow_ ? extend(item.chain,
                                                             canon_key(fs::path(item.chain->key) / e.name))
                                                    : nullptr});
                    continue;
                }
                // Descending here is what a link back into the tree (or a Windows
                // junction) turns into an endless walk: loop/, loop/loop/, ...
                if (!follow_ || info.broken || !info.directory) {
                    ++local.symlinks_skipped;
                    continue;
                }
                std::error_code ec;
                const fs::path real = fs::weakly_canonical(fs::path(abs), ec);
                // Already on the way in: this link closes a cycle, and walking it
                // would produce loop/, loop/loop/, ... without ever finishing. A
                // link to a directory we merely visited *elsewhere* is not a cycle
                // and is walked again — it really is in two places.
                if (ec || item.chain->on_path(canon_key(real))) {
                    ++local.symlinks_skipped;
                    continue;
                }
                ++local.symlinks_followed;
                pending.push_back({rel, extend(item.chain, canon_key(real))});
                continue;
            }

            if (ignore_.matches(rel, /*is_dir=*/false)) continue;
            const EntryProbe info = probe_entry(abs, follow_);
            // A link to a file elsewhere would otherwise be hashed and sent as a
            // regular file, quietly copying data from outside the tree to the peer
            // — which is exactly what following asks for, and nothing else does.
            if (info.symlink) {
                // A dangling link has no content to hash, and one whose target is
                // a directory is not a file however the entry was reported.
                if (!follow_ || info.broken || info.directory) {
                    ++local.symlinks_skipped;
                    continue;
                }
                ++local.symlinks_followed;
            }
            ++local.files_seen;

            FileMeta meta;
            // For a followed link the entry describes the link, not the file it
            // stands for: on Windows that is a zero-length reparse point, which
            // would advertise an empty file to the peer.
            meta.size  = info.symlink ? info.size : e.size;
            meta.mtime = info.symlink ? info.mtime : static_cast<int64_t>(e.modified_time);
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
            }

            out.set(rel, meta);
        }
    }

    if (stats) *stats = local;
    return out;
}

} // namespace rasync
