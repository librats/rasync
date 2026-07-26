#include "core/manifest.h"

#include "util/fs.h"

extern "C" {
#include "sha256.h"
}

namespace rasync {
namespace {

// One `path -> FileMeta` entry, shared by the full manifest and by patches so the
// two can never drift apart on the wire.
constexpr size_t kMinEntryBytes = 2 + 8 + 8 + 4 + 32;  ///< empty path + fixed fields

void write_entry(BinaryWriter& w, const std::string& path, const FileMeta& m) {
    w.str16(path);
    w.u64(m.size);
    w.i64(m.mtime);
    w.u32(m.mode);
    w.bytes(m.hash);
}

/// Reads one entry. Returns false on a truncated read or a path we refuse to
/// track — entries are peer-supplied and end up driving filesystem writes.
bool read_entry(BinaryReader& r, std::string& path, FileMeta& m) {
    path    = r.str16();
    m.size  = r.u64();
    m.mtime = r.i64();
    m.mode  = r.u32();
    m.hash  = r.bytes<32>();
    return r.ok() && !path.empty();
}

} // namespace

void Manifest::encode(BinaryWriter& w) const {
    w.u32(static_cast<uint32_t>(entries_.size()));
    for (const auto& [path, m] : entries_) write_entry(w, path, m);
}

Bytes Manifest::encode() const {
    BinaryWriter w(entries_.size() * 64 + 8);
    encode(w);
    return w.take();
}

std::optional<Manifest> Manifest::decode(BinaryReader& r) {
    uint32_t count = r.u32();
    Manifest m;
    for (uint32_t i = 0; i < count; ++i) {
        std::string path;
        FileMeta fm;
        if (!read_entry(r, path, fm)) return std::nullopt;
        m.entries_.emplace(std::move(path), fm);
    }
    return r.ok() ? std::optional<Manifest>(std::move(m)) : std::nullopt;
}

std::optional<Manifest> Manifest::decode(const Bytes& data) {
    BinaryReader r(data);
    return decode(r);
}

void Manifest::apply(const ManifestPatch& patch) {
    for (const auto& [path, meta] : patch.set) entries_[path] = meta;
    for (const auto& path : patch.removed) entries_.erase(path);
}

// ── ManifestPatch ────────────────────────────────────────────────────────────

ManifestPatch diff_manifests(const Manifest& from, const Manifest& to) {
    ManifestPatch patch;
    // Both sides are std::map, i.e. sorted by path: one merge walk, no lookups.
    auto a = from.entries().begin(), a_end = from.entries().end();
    auto b = to.entries().begin(),   b_end = to.entries().end();
    while (a != a_end || b != b_end) {
        if (b == b_end || (a != a_end && a->first < b->first)) {
            patch.removed.push_back(a->first);        // in `from` only — dropped
            ++a;
        } else if (a == a_end || b->first < a->first) {
            patch.set.emplace_back(b->first, b->second);  // in `to` only — added
            ++b;
        } else {
            if (a->second != b->second) patch.set.emplace_back(b->first, b->second);
            ++a; ++b;
        }
    }
    return patch;
}

void ManifestPatch::encode(BinaryWriter& w) const {
    w.u32(static_cast<uint32_t>(set.size()));
    for (const auto& [path, m] : set) write_entry(w, path, m);
    w.u32(static_cast<uint32_t>(removed.size()));
    for (const auto& path : removed) w.str16(path);
}

Bytes ManifestPatch::encode() const {
    BinaryWriter w(size() * 64 + 8);
    encode(w);
    return w.take();
}

std::optional<ManifestPatch> ManifestPatch::decode(BinaryReader& r) {
    ManifestPatch p;

    uint32_t n_set = r.u32();
    // Bound the count by what the buffer could possibly hold before reserving, so
    // a bogus header can't make us allocate for entries that aren't there.
    if (!r.ok() || r.remaining() < static_cast<size_t>(n_set) * kMinEntryBytes)
        return std::nullopt;
    p.set.reserve(n_set);
    for (uint32_t i = 0; i < n_set; ++i) {
        std::string path;
        FileMeta m;
        if (!read_entry(r, path, m)) return std::nullopt;
        p.set.emplace_back(std::move(path), m);
    }

    uint32_t n_del = r.u32();
    if (!r.ok() || r.remaining() < static_cast<size_t>(n_del) * 2) return std::nullopt;
    p.removed.reserve(n_del);
    for (uint32_t i = 0; i < n_del; ++i) {
        std::string path = r.str16();
        if (!r.ok() || path.empty()) return std::nullopt;
        p.removed.push_back(std::move(path));
    }
    return p;
}

std::optional<ManifestPatch> ManifestPatch::decode(const Bytes& data) {
    BinaryReader r(data);
    return decode(r);
}

Hash Manifest::fingerprint() const {
    sha256_context_t ctx;
    sha256_reset(&ctx);
    for (const auto& [path, m] : entries_) {
        sha256_update(&ctx, path.data(), path.size());
        uint8_t meta[8 + 4 + 32];
        for (int i = 0; i < 8; ++i) meta[i]     = static_cast<uint8_t>(m.size >> (56 - 8 * i));
        for (int i = 0; i < 4; ++i) meta[8 + i] = static_cast<uint8_t>(m.mode >> (24 - 8 * i));
        std::memcpy(meta + 12, m.hash.data(), 32);
        sha256_update(&ctx, meta, sizeof(meta));
    }
    Hash h{};
    sha256_finish(&ctx, h.data());
    return h;
}

bool Manifest::save(const std::string& path) const {
    Bytes data = encode();
    return librats::create_file_binary(path.c_str(), data.data(), data.size());
}

std::optional<Manifest> Manifest::load(const std::string& path) {
    size_t size = 0;
    void* raw = librats::read_file_binary(path.c_str(), &size);
    if (!raw) return std::nullopt;
    Bytes data(static_cast<uint8_t*>(raw), static_cast<uint8_t*>(raw) + size);
    librats::free_file_buffer(raw);
    return decode(data);
}

} // namespace rasync
