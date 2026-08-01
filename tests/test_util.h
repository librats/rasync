#pragma once

// Shared test scaffolding: a self-cleaning temporary directory and small helpers
// for writing/reading files, so filesystem-touching tests stay terse.

#include <atomic>
#include <cstdlib>
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

/// Create a symbolic link, reporting whether this host allows one at all.
/// Creating a symlink needs Developer Mode or admin rights on Windows, so a test
/// that needs one has to skip on a host without them — but it must never pass by
/// silently not creating the link, hence the explicit check that one now exists.
inline bool make_symlink(const fs::path& target, const std::string& link, bool directory) {
    std::error_code ec;
    if (directory) fs::create_directory_symlink(target, link, ec);
    else           fs::create_symlink(target, link, ec);
    if (ec) return false;
    return fs::is_symlink(fs::symlink_status(link, ec));
}

/// A directory junction, held for the lifetime of the object — Windows' other
/// name-surrogate reparse point, and the one that matters most in practice: it
/// needs neither privilege nor Developer Mode, so unlike a symlink it is always
/// creatable, and Windows ships several in every user profile pointing at their
/// own parent. There is no portable API to make one, hence the shell-out; there
/// is no portable API to *see* one either, which is what the tests assert.
///
/// It removes itself, and that is not tidiness. `std::filesystem::remove_all` on
/// libstdc++ cannot tell a junction from a directory, so a junction left for
/// TempDir to clean up is deleted *through* — taking the target's contents with
/// it, and never terminating at all when the junction points at its own parent.
/// Declare one after the TempDir it lives in and the destruction order is right.
class Junction {
public:
    Junction(const fs::path& target, const std::string& link) {
#ifdef _WIN32
        const std::string cmd =
            "cmd /c mklink /J \"" + link + "\" \"" + target.string() + "\" >nul 2>&1";
        if (std::system(cmd.c_str()) != 0) return;
        std::error_code ec;
        if (!fs::is_directory(link, ec)) return;
        path_ = link;
#else
        (void)target;
        (void)link;  // no junctions here; the test skips
#endif
    }
    ~Junction() {
        if (path_.empty()) return;
        std::error_code ec;
        // `remove`, never `remove_all`: this must unlink the junction itself and
        // leave whatever it points at alone.
        fs::remove(path_, ec);
    }
    Junction(const Junction&) = delete;
    Junction& operator=(const Junction&) = delete;

    /// False where this host has no junctions — the test's cue to skip.
    bool ok() const { return !path_.empty(); }

private:
    std::string path_;
};

inline std::vector<uint8_t> random_bytes(size_t n, uint32_t seed = 1) {
    // Deterministic LCG — reproducible test inputs without <random> overhead.
    std::vector<uint8_t> out(n);
    uint32_t s = seed ? seed : 1;
    for (size_t i = 0; i < n; ++i) { s = s * 1664525u + 1013904223u; out[i] = static_cast<uint8_t>(s >> 24); }
    return out;
}

} // namespace rasync::test
