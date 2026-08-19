#include "core/scanner.h"

#include "core/file_stat.h"
#include "librats/util/fs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <vector>

namespace rasync {
namespace {

namespace fs = std::filesystem;

/// What one directory entry is, beyond what `list_directory` reports. The first
/// look is always a *no-follow* one: an entry has to be recognised as a link
/// before anything decides whether to walk through it, and the executable bit
/// has to be read off the entry's own mode anyway. When links are being followed
/// the target is looked at too, because that is what the entry then stands for —
/// a link's own directory entry carries neither the target's size nor its mtime.
///
/// `symlink` covers a Windows junction as well: it is a different reparse tag but
/// the same hazard, since it points at a directory and can point at its own
/// ancestor. `stat_nofollow` is what draws that line — see file_stat.h for why
/// neither `stat()` nor `std::filesystem` can be asked instead.
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
    const FileStat self = stat_nofollow(abs_path);
    if (!self.exists) return p;  // gone between the listing and now
    p.symlink = self.link;
    if (!p.symlink) {
        p.directory = self.directory;
        p.mode      = self.mode;  // the entry's own permission; always 0 on Windows
        return p;
    }
    if (!follow) return p;

    // Same path, this time resolved: a followed link *is* its target, and its own
    // directory entry describes neither the target's size nor its mtime — on
    // Windows it is a zero-length reparse point, which would advertise an empty
    // file to the peer. A dangling link has nothing behind it and comes back
    // `!exists`, which is the caller's cue to skip rather than sync a file that
    // isn't there. `stat_follow` rather than `stat()` because the CRTs disagree
    // about whether the latter follows a reparse point at all (see file_stat.h).
    const FileStat target = stat_follow(abs_path);
    if (!target.exists) { p.broken = true; return p; }
    p.directory = target.directory;
    p.size      = target.size;
    p.mtime     = target.mtime;
    p.mode      = target.mode;  // the permission of the file pointed at
    return p;
}

std::string join_rel(const std::string& base, const std::string& name) {
    return base.empty() ? name : base + "/" + name;
}

/// A directory's identity for loop detection: its path with every link already
/// resolved, folded to lower case where the platform's paths are case-insensitive
/// — two spellings of one directory have to compare equal, or a cycle spelled
/// inconsistently would never be recognised and the walk would not terminate.
std::string canon_key(std::string s) {
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
    if (!follow_) {
        pending.push_back({std::string(), nullptr});
    } else {
        // Only a followed walk can cycle, so only it needs to know the resolved
        // identity of where it started — and it is a lookup a plain scan, which
        // runs on every poll, does not have to pay for.
        std::string real = real_path(root);
        if (real.empty()) real = root;  // unresolvable: the lexical path still keys it
        pending.push_back({std::string(), extend(nullptr, canon_key(real))});
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
                    // No link was crossed to get here, so this directory's identity
                    // is its parent's resolved key plus a name — no lookup needed.
                    pending.push_back({rel, follow_ ? extend(item.chain,
                                                             canon_key((fs::path(item.chain->key) / e.name).string()))
                                                    : nullptr});
                    continue;
                }
                // Descending here is what a link back into the tree (or a Windows
                // junction) turns into an endless walk: loop/, loop/loop/, ...
                if (!follow_ || info.broken || !info.directory) {
                    ++local.symlinks_skipped;
                    continue;
                }
                const std::string real = real_path(abs);
                // Already on the way in: this link closes a cycle, and walking it
                // would produce loop/, loop/loop/, ... without ever finishing. A
                // link to a directory we merely visited *elsewhere* is not a cycle
                // and is walked again — it really is in two places.
                if (real.empty() || item.chain->on_path(canon_key(real))) {
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
