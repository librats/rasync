#include "core/scanner.h"

#include "util/fs.h"

#include <algorithm>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace rasync {
namespace {

/// Owner-executable bit, POSIX only (Windows has no such concept for files).
uint32_t detect_mode(const std::string& abs_path) {
#ifndef _WIN32
    struct stat st{};
    if (::stat(abs_path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
        return FileMeta::kExecutable;
#else
    (void)abs_path;
#endif
    return 0;
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

            if (e.is_directory) {
                ++local.dirs_seen;
                if (ignore_.matches(rel, /*is_dir=*/true)) continue;
                pending.push_back(rel);
                continue;
            }

            if (ignore_.matches(rel, /*is_dir=*/false)) continue;
            ++local.files_seen;

            FileMeta meta;
            meta.size  = e.size;
            meta.mtime = static_cast<int64_t>(e.modified_time);
            const std::string abs = librats::combine_paths(root, rel);
            meta.mode  = detect_mode(abs);

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
