#include "core/hash.h"

#include "util/fs.h"

extern "C" {
#include "sha256.h"
}

#include <vector>

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

} // namespace rasync
