#include "core/file_stat.h"

#include "core/file_meta.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace rasync {

#ifdef _WIN32
namespace {

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

} // namespace

FileStat stat_follow(const std::string& path) {
    FileStat s;
    // Narrow-char on purpose: every path in rasync reaches the OS through
    // librats' `FindFirstFileA` and `fopen`, so these bytes are in the process
    // code page. Widening them as UTF-8 here would mis-resolve exactly the
    // non-ASCII names the rest of the walk handles.
    //
    // Without FILE_FLAG_OPEN_REPARSE_POINT the open resolves the link and the
    // handle describes its target — which is the whole point of this function.
    // BACKUP_SEMANTICS lets that target be a directory; FILE_READ_ATTRIBUTES
    // alone opens a file whose contents we may not be allowed to read; sharing
    // everything keeps a scan from disturbing a file another process is writing.
    const HANDLE h = ::CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
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
#endif

} // namespace rasync
