#include "core/state_dir.h"

#include "core/hash.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace rasync {
namespace {

/// Read an environment variable. MSVC deprecates `getenv`, so it gets the
/// _s variant rather than a project-wide warning suppression.
std::string env(const char* name) {
#if defined(_MSC_VER)
    char*  buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) return {};
    std::string v(buf);
    std::free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

std::string join(const std::string& base, const std::string& leaf) {
    return (fs::path(base) / leaf).string();
}

/// The absolute path in one canonical spelling, so that `./data`, `data/`, and
/// `/home/u/data` all hash to the same state directory. Deliberately *not*
/// `fs::canonical`: that requires the directory to already exist and resolves
/// symlinks, either of which would move a tree's state around under it.
std::string canonical_key(const std::string& sync_root) {
    std::error_code ec;
    fs::path p(sync_root);
    if (p.is_relative()) {
        fs::path abs = fs::absolute(p, ec);
        if (!ec) p = abs;
    }
    std::string s = p.lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
    // Windows paths are case-insensitive, so two spellings name one directory
    // and must not end up with two independent baselines.
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
    return s;
}

/// The tree's own folder name, reduced to characters that are safe on every
/// filesystem. Cosmetic only — uniqueness comes from the hash beside it.
std::string readable_leaf(const std::string& key) {
    std::string leaf = fs::path(key).filename().string();
    if (leaf.empty()) leaf = fs::path(key).parent_path().filename().string();
    std::string out;
    for (char c : leaf) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
        if (out.size() >= 40) break;
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '_')) out.erase(out.begin());
    return out.empty() ? std::string("dir") : out;
}

} // namespace

std::string user_state_root() {
    std::string override_root = env("RASYNC_HOME");
    if (!override_root.empty()) return override_root;

#if defined(_WIN32)
    std::string local = env("LOCALAPPDATA");
    if (local.empty()) local = env("APPDATA");
    if (local.empty()) {
        std::string profile = env("USERPROFILE");
        if (!profile.empty()) local = join(profile, "AppData\\Local");
    }
    return local.empty() ? std::string() : join(local, "rasync");
#else
    std::string home = env("HOME");
#  if defined(__APPLE__)
    if (home.empty()) return {};
    return join(home, "Library/Application Support/rasync");
#  else
    std::string xdg = env("XDG_DATA_HOME");
    if (!xdg.empty()) return join(xdg, "rasync");
    if (home.empty()) return {};
    return join(home, ".local/share/rasync");
#  endif
#endif
}

std::string state_dir_name(const std::string& sync_root) {
    const std::string key = canonical_key(sync_root);
    return readable_leaf(key) + "-" + to_hex(sha256(key)).substr(0, 16);
}

std::string state_dir_for(const std::string& sync_root) {
    const std::string root = user_state_root();
    if (root.empty()) return join(sync_root, ".rasync");  // no home: nowhere better to go
    return join(join(root, "dirs"), state_dir_name(sync_root));
}

std::string node_state_dir() {
    const std::string root = user_state_root();
    return root.empty() ? std::string() : join(root, "node");
}

void record_state_owner(const std::string& state_dir, const std::string& sync_root) {
    // A hashed directory name is unreadable on its own; this is what lets a user
    // (or a future cleanup command) tell which tree a state directory belongs to.
    const fs::path marker = fs::path(state_dir) / "directory.txt";
    std::error_code ec;
    fs::create_directories(state_dir, ec);
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    if (out) out << sync_root << '\n';
}

size_t migrate_state_dir(const std::string& legacy, const std::string& target) {
    if (legacy.empty() || target.empty()) return 0;

    std::error_code ec;
    const fs::path from(legacy);
    const fs::path to(target);
    if (fs::equivalent(from, to, ec)) return 0;   // already there (ec => one is missing)
    ec.clear();
    if (!fs::is_directory(from, ec)) return 0;
    fs::create_directories(to, ec);
    if (ec) return 0;

    // Snapshot first: the loop renames entries out from under the directory, and
    // an iterator is not required to survive that.
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(from, ec)) entries.push_back(e.path());
    if (ec) return 0;

    size_t moved = 0;
    for (const auto& src : entries) {
        const fs::path dst = to / src.filename();
        if (fs::exists(dst)) continue;            // the new location wins; keep the old copy
        std::error_code mv;
        fs::rename(src, dst, mv);
        if (mv) {
            // A different filesystem (state root on another volume): copy, then drop.
            mv.clear();
            fs::copy(src, dst, fs::copy_options::recursive, mv);
            if (mv) continue;
            fs::remove_all(src, mv);
            if (mv) continue;
        }
        ++moved;
    }
    fs::remove(from, ec);   // non-recursive: only if nothing was left behind
    return moved;
}

} // namespace rasync
