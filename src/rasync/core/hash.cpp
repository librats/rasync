#include "core/hash.h"

#include "util/fs.h"

extern "C" {
#include "sha256.h"
}

#include <cstdio>

namespace rasync {

std::string to_hex(const Hash& h) {
    static const char* k = "0123456789abcdef";
    std::string out(h.size() * 2, '0');
    for (size_t i = 0; i < h.size(); ++i) {
        out[2 * i]     = k[h[i] >> 4];
        out[2 * i + 1] = k[h[i] & 0x0f];
    }
    return out;
}

std::optional<Hash> hash_from_hex(const std::string& hex) {
    if (hex.size() != 64) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Hash h{};
    for (size_t i = 0; i < 32; ++i) {
        int hi = nibble(hex[2 * i]);
        int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        h[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return h;
}

Hash sha256(const void* data, size_t size) {
    Hash h{};
    sha256_hash(h.data(), data, size);
    return h;
}

std::optional<Hash> sha256_file(const std::string& path) {
    librats::FileStream in;
    if (!in.open_read(path.c_str())) return std::nullopt;

    sha256_context_t ctx;
    sha256_reset(&ctx);

    // 256 KiB streaming window: large enough to amortise syscalls, small enough
    // to keep memory flat regardless of file size.
    constexpr size_t kBuf = 256 * 1024;
    std::vector<uint8_t> buf(kBuf);
    for (;;) {
        size_t n = in.read(buf.data(), kBuf);
        if (n == 0) break;
        sha256_update(&ctx, buf.data(), n);
        if (n < kBuf) break;
    }

    Hash h{};
    sha256_finish(&ctx, h.data());
    return h;
}

Hash empty_hash() { return sha256("", 0); }

} // namespace rasync
