#pragma once

// Shared test scaffolding: a self-cleaning temporary directory and small helpers
// for writing/reading files, so filesystem-touching tests stay terse.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace rasync::test {

namespace fs = std::filesystem;

/// An RAII unique temp directory under the system temp path, removed on scope exit.
class TempDir {
public:
    TempDir() {
        static std::atomic<uint64_t> counter{0};
        path_ = fs::temp_directory_path() /
                ("rasync_test_" + std::to_string(counter.fetch_add(1)) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }
    std::string str() const { return path_.string(); }
    std::string sub(const std::string& rel) const { return (path_ / rel).string(); }

private:
    fs::path path_;
};

inline void write_file(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

inline void write_file(const std::string& path, const std::vector<uint8_t>& content) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()),
              static_cast<std::streamsize>(content.size()));
}

inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

inline std::vector<uint8_t> random_bytes(size_t n, uint32_t seed = 1) {
    // Deterministic LCG — reproducible test inputs without <random> overhead.
    std::vector<uint8_t> out(n);
    uint32_t s = seed ? seed : 1;
    for (size_t i = 0; i < n; ++i) { s = s * 1664525u + 1013904223u; out[i] = static_cast<uint8_t>(s >> 24); }
    return out;
}

} // namespace rasync::test
