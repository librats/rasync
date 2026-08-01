#include "core/file_stat.h"

#include "core/file_meta.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>   // ::realpath, ::free — POSIX, not the <cstdlib> subset
#include <sys/stat.h>
#endif

namespace rasync {

#ifdef _WIN32
namespace {

/// Sharing everything: a scan must not disturb — or be refused by — a file some
/// other process is busy writing.
constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

/// FILETIME (100 ns ticks since 1601) → Unix seconds. Deliberately the same
/// arithmetic librats' `list_directory` applies to its `FindFirstFile` data: a
/// followed link's mtime is stored alongside — and compared against — mtimes
/// that came from there, so the two conversions must not disagree by a second.
int64_t unix_seconds(const FILETIME& ft) {
    LARGE_INTEGER li;
    li.LowPart  = ft.dwLowDateTime;
    li.HighPart = static_cast<LONG>(ft.dwHighDateTime);
    return (li.QuadPart - 116444736000000000LL) / 10000000LL;
}

/// A handle good for nothing but asking questions. `FILE_READ_ATTRIBUTES` opens
/// a file whose contents we may not be allowed to read and is not subject to
/// another process's sharing mode; `BACKUP_SEMANTICS` lets the subject be a
/// directory. `extra` is `FILE_FLAG_OPEN_REPARSE_POINT` when the link itself is
/// the subject, and 0 to resolve it — that flag's *absence* is what makes an
/// open follow a link, and is the whole reason this file exists.
///
/// Narrow-char on purpose: every path in rasync reaches the OS through librats'
/// `FindFirstFileA` and `fopen`, so these bytes are already in the process code
/// page. Widening them here as UTF-8 would mis-resolve exactly the non-ASCII
/// names the rest of the walk handles.
HANDLE open_meta(const std::string& path, DWORD extra) {
    return ::CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | extra, nullptr);
}

} // namespace

FileStat stat_follow(const std::string& path) {
    FileStat s;
    const HANDLE h = open_meta(path, 0);  // no OPEN_REPARSE_POINT: resolves the link
    if (h == INVALID_HANDLE_VALUE) return s;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = ::GetFileInformationByHandle(h, &info) != 0;
    ::CloseHandle(h);
    if (!ok) return s;

    s.exists    = true;
    s.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!s.directory)
        s.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    s.mtime = unix_seconds(info.ftLastWriteTime);
    // Windows has no executable bit worth carrying, so `mode` stays 0.
    return s;
}

FileStat stat_nofollow(const std::string& path) {
    FileStat s;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return s;

    s.exists    = true;
    s.directory = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    s.mtime     = unix_seconds(fad.ftLastWriteTime);

    // The overwhelming majority of entries stop here, at one cheap lookup.
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // Being a reparse point does not make an entry a link: a cloud
        // placeholder, a deduplicated file and a WIM-backed one are all reparse
        // points that still *are* the file, and treating them as links would drop
        // them from the sync. Only the tag tells them apart, and reading it means
        // opening the point rather than what it points at.
        const HANDLE h = open_meta(path, FILE_FLAG_OPEN_REPARSE_POINT);
        if (h != INVALID_HANDLE_VALUE) {
            FILE_ATTRIBUTE_TAG_INFO tag{};
            if (::GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag))
                s.link = IsReparseTagNameSurrogate(tag.ReparseTag) != 0;
            ::CloseHandle(h);
        }
    }

    if (!s.directory && !s.link)
        s.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return s;
}

std::string real_path(const std::string& path) {
    const HANDLE h = open_meta(path, 0);  // follows: we want where the path ends up
    if (h == INVALID_HANDLE_VALUE) return {};

    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    char  stack_buf[MAX_PATH];
    DWORD n = ::GetFinalPathNameByHandleA(h, stack_buf, MAX_PATH, kFlags);
    std::string out;
    if (n > 0 && n < MAX_PATH) {
        out.assign(stack_buf, n);  // n excludes the terminator when it fit
    } else if (n >= MAX_PATH) {
        // Too long for the stack buffer: n now counts the terminator too.
        out.resize(n);
        const DWORD written = ::GetFinalPathNameByHandleA(h, &out[0], n, kFlags);
        out.resize(written > 0 && written < n ? written : 0);
    }
    ::CloseHandle(h);

    // The API answers in `\\?\` form — "do not parse this". Put it back in the
    // shape the rest of the walk spells paths in, so a resolved directory and a
    // lexically built one compare equal.
    if (out.compare(0, 8, "\\\\?\\UNC\\") == 0) out = "\\\\" + out.substr(8);
    else if (out.compare(0, 4, "\\\\?\\") == 0) out = out.substr(4);
    return out;
}
#else
FileStat stat_follow(const std::string& path) {
    FileStat s;
    struct stat st{};
    // Follows links by definition, so a dangling one fails here rather than
    // reporting the link.
    if (::stat(path.c_str(), &st) != 0) return s;

    s.exists    = true;
    s.directory = S_ISDIR(st.st_mode);
    if (!s.directory) {
        s.size = static_cast<uint64_t>(st.st_size);
        if (st.st_mode & S_IXUSR) s.mode = FileMeta::kExecutable;
    }
    s.mtime = static_cast<int64_t>(st.st_mtime);
    return s;
}

FileStat stat_nofollow(const std::string& path) {
    FileStat s;
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return s;

    s.exists    = true;
    s.link      = S_ISLNK(st.st_mode);
    s.directory = S_ISDIR(st.st_mode);
    // A link's own size and mode describe the link (0777 on every POSIX system,
    // and the length of the target string) — nothing worth carrying, and nothing
    // Windows has an equivalent of. What matters is the target's, and that is
    // `stat_follow`'s answer.
    if (!s.directory && !s.link) {
        s.size = static_cast<uint64_t>(st.st_size);
        if (st.st_mode & S_IXUSR) s.mode = FileMeta::kExecutable;
    }
    s.mtime = static_cast<int64_t>(st.st_mtime);
    return s;
}

std::string real_path(const std::string& path) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) return {};
    std::string out(resolved);
    ::free(resolved);
    return out;
}
#endif

} // namespace rasync
