#pragma once

/**
 * @file serialize.h
 * @brief Minimal, allocation-light big-endian binary reader/writer.
 *
 * rasync speaks a compact binary protocol (no JSON) both on the wire and for its
 * on-disk state. `BinaryWriter` appends into a growable buffer; `BinaryReader` is
 * a bounds-checked cursor over a read-only view. Every reader method validates
 * remaining length before touching memory, so a truncated or hostile payload can
 * never over-read — it just flips `ok()` to false and yields zeroes thereafter.
 *
 * Integers are big-endian (network order). Strings and byte blobs are length-
 * prefixed; the caller picks the prefix width (u16 for short paths, u32 for bulk).
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rasync {

using Bytes = std::vector<uint8_t>;

class BinaryWriter {
public:
    BinaryWriter() = default;
    explicit BinaryWriter(size_t reserve) { buf_.reserve(reserve); }

    void u8(uint8_t v) { buf_.push_back(v); }

    void u16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v >> 8));
        buf_.push_back(static_cast<uint8_t>(v));
    }

    void u32(uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) buf_.push_back(static_cast<uint8_t>(v >> s));
    }

    void u64(uint64_t v) {
        for (int s = 56; s >= 0; s -= 8) buf_.push_back(static_cast<uint8_t>(v >> s));
    }

    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }

    void raw(const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + n);
    }

    template <size_t N>
    void bytes(const std::array<uint8_t, N>& a) { raw(a.data(), N); }

    /// Length-prefixed string / blob. Prefix width chosen by the caller.
    void str16(const std::string& s) { u16(static_cast<uint16_t>(s.size())); raw(s.data(), s.size()); }
    void str32(const std::string& s) { u32(static_cast<uint32_t>(s.size())); raw(s.data(), s.size()); }
    void blob32(const void* data, size_t n) { u32(static_cast<uint32_t>(n)); raw(data, n); }

    const Bytes& buffer() const noexcept { return buf_; }
    Bytes        take() noexcept { return std::move(buf_); }
    size_t       size() const noexcept { return buf_.size(); }

private:
    Bytes buf_;
};

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    explicit BinaryReader(const Bytes& b) : BinaryReader(b.data(), b.size()) {}

    bool   ok()        const noexcept { return ok_; }
    size_t remaining() const noexcept { return static_cast<size_t>(end_ - p_); }

    uint8_t u8() {
        if (!take(1)) return 0;
        return *p_++;
    }

    uint16_t u16() {
        if (!take(2)) return 0;
        uint16_t v = (uint16_t(p_[0]) << 8) | uint16_t(p_[1]);
        p_ += 2;
        return v;
    }

    uint32_t u32() {
        if (!take(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | p_[i];
        p_ += 4;
        return v;
    }

    uint64_t u64() {
        if (!take(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | p_[i];
        p_ += 8;
        return v;
    }

    int64_t i64() { return static_cast<int64_t>(u64()); }

    template <size_t N>
    std::array<uint8_t, N> bytes() {
        std::array<uint8_t, N> a{};
        if (take(N)) { std::memcpy(a.data(), p_, N); p_ += N; }
        return a;
    }

    std::string str16() { return read_str(u16()); }
    std::string str32() { return read_str(u32()); }

    /// Read a u32-prefixed blob into a caller-owned vector.
    Bytes blob32() {
        uint32_t n = u32();
        if (!take(n)) return {};
        Bytes out(p_, p_ + n);
        p_ += n;
        return out;
    }

private:
    bool take(size_t n) {
        if (!ok_ || remaining() < n) { ok_ = false; return false; }
        return true;
    }

    std::string read_str(uint32_t n) {
        if (!take(n)) return {};
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }

    const uint8_t* p_;
    const uint8_t* end_;
    bool           ok_ = true;
};

} // namespace rasync
