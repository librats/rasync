#include "core/delta.h"

#include "core/hash.h"
#include "util/fs.h"

#include <cstring>
#include <unordered_map>

namespace rasync {
namespace {

std::array<uint8_t, 16> strong16(const uint8_t* data, size_t len) {
    Hash full = sha256(data, len);
    std::array<uint8_t, 16> s{};
    std::memcpy(s.data(), full.data(), 16);
    return s;
}

} // namespace

uint32_t weak_checksum(const uint8_t* data, size_t len) {
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a += data[i];
        b += static_cast<uint32_t>(len - i) * data[i];
    }
    return ((b & 0xffff) << 16) | (a & 0xffff);
}

// ── Signature ────────────────────────────────────────────────────────────────

Signature signature_of(const uint8_t* data, size_t size, uint32_t block_size) {
    Signature sig;
    sig.block_size = block_size ? block_size : kDefaultBlockSize;
    sig.file_size  = size;
    for (size_t off = 0; off < size; off += sig.block_size) {
        size_t len = std::min<size_t>(sig.block_size, size - off);
        BlockSig bs;
        bs.weak   = weak_checksum(data + off, len);
        bs.strong = strong16(data + off, len);
        sig.blocks.push_back(bs);
    }
    return sig;
}

std::optional<Signature> signature_of_file(const std::string& path, uint32_t block_size) {
    librats::FileStream in;
    if (!in.open_read(path.c_str())) return std::nullopt;
    Signature sig;
    sig.block_size = block_size ? block_size : kDefaultBlockSize;
    sig.file_size  = 0;
    std::vector<uint8_t> buf(sig.block_size);
    for (;;) {
        size_t n = in.read(buf.data(), sig.block_size);
        if (n == 0) break;
        BlockSig bs;
        bs.weak   = weak_checksum(buf.data(), n);
        bs.strong = strong16(buf.data(), n);
        sig.blocks.push_back(bs);
        sig.file_size += n;
        if (n < sig.block_size) break;
    }
    return sig;
}

void Signature::encode(BinaryWriter& w) const {
    w.u32(block_size);
    w.u64(file_size);
    w.u32(static_cast<uint32_t>(blocks.size()));
    for (const auto& b : blocks) { w.u32(b.weak); w.bytes(b.strong); }
}

Bytes Signature::encode() const {
    BinaryWriter w(blocks.size() * 20 + 16);
    encode(w);
    return w.take();
}

std::optional<Signature> Signature::decode(BinaryReader& r) {
    Signature s;
    s.block_size = r.u32();
    s.file_size  = r.u64();
    uint32_t n   = r.u32();
    if (!r.ok() || r.remaining() < static_cast<size_t>(n) * 20) return std::nullopt;
    s.blocks.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        BlockSig b;
        b.weak   = r.u32();
        b.strong = r.bytes<16>();
        s.blocks.push_back(b);
    }
    return r.ok() ? std::optional<Signature>(std::move(s)) : std::nullopt;
}

std::optional<Signature> Signature::decode(const Bytes& data) {
    BinaryReader r(data);
    return decode(r);
}

// ── Delta ────────────────────────────────────────────────────────────────────

uint64_t Delta::literal_bytes() const {
    uint64_t t = 0;
    for (const auto& op : ops) if (!op.copy) t += op.literal.size();
    return t;
}

uint64_t Delta::copied_bytes() const {
    uint64_t t = 0;
    for (const auto& op : ops) if (op.copy) t += op.len;
    return t;
}

void Delta::encode(BinaryWriter& w) const {
    w.u64(out_size);
    w.u32(static_cast<uint32_t>(ops.size()));
    for (const auto& op : ops) {
        w.u8(op.copy ? 1 : 0);
        if (op.copy) { w.u64(op.offset); w.u32(op.len); }
        else         { w.blob32(op.literal.data(), op.literal.size()); }
    }
}

Bytes Delta::encode() const {
    BinaryWriter w;
    encode(w);
    return w.take();
}

std::optional<Delta> Delta::decode(BinaryReader& r) {
    Delta d;
    d.out_size = r.u64();
    uint32_t n = r.u32();
    if (!r.ok()) return std::nullopt;
    d.ops.reserve(std::min<uint32_t>(n, 1u << 20));
    for (uint32_t i = 0; i < n; ++i) {
        DeltaOp op;
        op.copy = r.u8() != 0;
        if (op.copy) { op.offset = r.u64(); op.len = r.u32(); }
        else         { op.literal = r.blob32(); }
        if (!r.ok()) return std::nullopt;
        d.ops.push_back(std::move(op));
    }
    return d;
}

std::optional<Delta> Delta::decode(const Bytes& data) {
    BinaryReader r(data);
    return decode(r);
}

// ── delta computation (the rolling-checksum scan) ────────────────────────────

Delta compute_delta(const Signature& sig, const uint8_t* target, size_t size) {
    Delta d;
    d.out_size = size;
    const uint32_t L = sig.block_size;

    auto emit_copy = [&](uint64_t offset, uint32_t len) {
        // Coalesce with a contiguous preceding COPY for a compact instruction stream.
        if (!d.ops.empty() && d.ops.back().copy &&
            d.ops.back().offset + d.ops.back().len == offset) {
            d.ops.back().len += len;
        } else {
            DeltaOp op; op.copy = true; op.offset = offset; op.len = len;
            d.ops.push_back(std::move(op));
        }
    };
    auto emit_literal = [&](size_t from, size_t to) {
        if (to > from) {
            DeltaOp op; op.copy = false;
            op.literal.assign(target + from, target + to);
            d.ops.push_back(std::move(op));
        }
    };

    if (L == 0 || sig.blocks.empty() || size < L) {
        emit_literal(0, size);
        return d;
    }

    std::unordered_multimap<uint32_t, uint32_t> by_weak;
    by_weak.reserve(sig.blocks.size() * 2);
    for (uint32_t i = 0; i < sig.blocks.size(); ++i)
        by_weak.emplace(sig.blocks[i].weak, i);

    size_t pos = 0, literal_start = 0;
    uint32_t a = 0, b = 0;
    auto init_window = [&](size_t at) {
        a = 0; b = 0;
        for (uint32_t i = 0; i < L; ++i) {
            a += target[at + i];
            b += static_cast<uint32_t>(L - i) * target[at + i];
        }
    };
    init_window(0);

    while (pos + L <= size) {
        uint32_t weak = ((b & 0xffff) << 16) | (a & 0xffff);
        bool matched = false;
        auto range = by_weak.equal_range(weak);
        if (range.first != range.second) {
            auto s = strong16(target + pos, L);
            for (auto it = range.first; it != range.second; ++it) {
                const auto& blk = sig.blocks[it->second];
                // Only whole-size base blocks are COPY targets (skip a short tail block).
                if (blk.strong == s && static_cast<uint64_t>(it->second) * L + L <= sig.file_size) {
                    emit_literal(literal_start, pos);
                    emit_copy(static_cast<uint64_t>(it->second) * L, L);
                    pos += L;
                    literal_start = pos;
                    matched = true;
                    break;
                }
            }
        }
        if (matched) {
            if (pos + L <= size) init_window(pos);
        } else {
            if (pos + L < size) {
                uint8_t out = target[pos], in = target[pos + L];
                a = (a - out + in) & 0xffff;
                b = (b - static_cast<uint32_t>(L) * out + a) & 0xffff;
            }
            ++pos;
        }
    }
    emit_literal(literal_start, size);
    return d;
}

// ── delta application ────────────────────────────────────────────────────────

Bytes apply_delta(const uint8_t* base, size_t base_size, const Delta& delta) {
    Bytes out;
    out.reserve(delta.out_size);
    for (const auto& op : delta.ops) {
        if (op.copy) {
            if (op.offset + op.len > base_size) return {};  // malformed / wrong base
            out.insert(out.end(), base + op.offset, base + op.offset + op.len);
        } else {
            out.insert(out.end(), op.literal.begin(), op.literal.end());
        }
    }
    return out;
}

bool apply_delta_file(const std::string& base_path, const Delta& delta,
                      const std::string& out_path) {
    librats::FileStream base, out;
    bool need_base = false;
    for (const auto& op : delta.ops) if (op.copy) { need_base = true; break; }
    if (need_base && !base.open_read(base_path.c_str())) return false;
    if (!out.open_write(out_path.c_str())) return false;

    uint64_t written = 0;
    std::vector<uint8_t> buf(64 * 1024);
    for (const auto& op : delta.ops) {
        if (op.copy) {
            uint32_t left = op.len;
            uint64_t off = op.offset;
            while (left > 0) {
                size_t want = std::min<size_t>(buf.size(), left);
                if (!base.seek(off)) return false;
                size_t got = base.read(buf.data(), want);
                if (got == 0) return false;
                if (!out.write_at(written, buf.data(), got)) return false;
                written += got; off += got; left -= static_cast<uint32_t>(got);
            }
        } else {
            if (!op.literal.empty() &&
                !out.write_at(written, op.literal.data(), op.literal.size()))
                return false;
            written += op.literal.size();
        }
    }
    return true;
}

} // namespace rasync
