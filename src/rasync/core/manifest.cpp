#include "core/manifest.h"

#include "util/fs.h"

extern "C" {
#include "sha256.h"
}

namespace rasync {

void Manifest::encode(BinaryWriter& w) const {
    w.u32(static_cast<uint32_t>(entries_.size()));
    for (const auto& [path, m] : entries_) {
        w.str16(path);
        w.u64(m.size);
        w.i64(m.mtime);
        w.u32(m.mode);
        w.bytes(m.hash);
    }
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
        std::string path = r.str16();
        FileMeta fm;
        fm.size  = r.u64();
        fm.mtime = r.i64();
        fm.mode  = r.u32();
        fm.hash  = r.bytes<32>();
        if (!r.ok()) return std::nullopt;
        // Reject empty/absolute/backslash paths defensively — a manifest is
        // peer-supplied and later drives filesystem writes.
        if (path.empty()) return std::nullopt;
        m.entries_.emplace(std::move(path), fm);
    }
    return r.ok() ? std::optional<Manifest>(std::move(m)) : std::nullopt;
}

std::optional<Manifest> Manifest::decode(const Bytes& data) {
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

uint64_t Manifest::generation() const {
    Hash h = fingerprint();
    uint64_t g = 0;
    for (int i = 0; i < 8; ++i) g = (g << 8) | h[i];
    return g;
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
