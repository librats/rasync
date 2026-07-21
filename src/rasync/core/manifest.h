#pragma once

/**
 * @file manifest.h
 * @brief An ordered snapshot of a directory tree: relative path -> FileMeta.
 *
 * The manifest is the unit both peers exchange to reconcile, and the unit rasync
 * persists as the "last synced" baseline for three-way merge. Entries are kept in
 * a std::map so iteration is deterministic (sorted by path) — this makes the
 * wire encoding, the on-disk form and the `fingerprint()` stable regardless of
 * scan order.
 *
 * Serialization is the same compact big-endian form on the wire and on disk, so
 * a baseline written last run decodes identically this run.
 */

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "core/file_meta.h"
#include "core/serialize.h"

namespace rasync {

class Manifest {
public:
    using Map = std::map<std::string, FileMeta>;

    void set(const std::string& path, const FileMeta& meta) { entries_[path] = meta; }
    void remove(const std::string& path) { entries_.erase(path); }
    void clear() { entries_.clear(); }

    bool contains(const std::string& path) const { return entries_.count(path) != 0; }

    /// Metadata for a path, or nullptr if absent.
    const FileMeta* find(const std::string& path) const {
        auto it = entries_.find(path);
        return it == entries_.end() ? nullptr : &it->second;
    }

    const Map& entries() const noexcept { return entries_; }
    size_t     size()    const noexcept { return entries_.size(); }
    bool       empty()   const noexcept { return entries_.empty(); }

    /// Total bytes across all tracked files.
    uint64_t total_bytes() const {
        uint64_t t = 0;
        for (const auto& [_, m] : entries_) t += m.size;
        return t;
    }

    /// A stable content fingerprint over the whole tree (paths + hashes + sizes +
    /// modes). Two manifests with the same fingerprint describe identical trees.
    Hash fingerprint() const;

    /// A cheap 64-bit "generation" derived from the fingerprint, for lightweight
    /// "did anything change?" comparisons and log lines.
    uint64_t generation() const;

    // — serialization (identical on the wire and on disk) —
    void  encode(BinaryWriter& w) const;
    Bytes encode() const;
    static std::optional<Manifest> decode(BinaryReader& r);
    static std::optional<Manifest> decode(const Bytes& data);

    // — disk persistence for the baseline manifest —
    bool save(const std::string& path) const;
    static std::optional<Manifest> load(const std::string& path);

private:
    Map entries_;
};

} // namespace rasync
