#pragma once

/**
 * @file path.h
 * @brief What counts as a relative path rasync is willing to act on.
 *
 * Every path rasync writes to disk arrives from a peer — inside a manifest, or in
 * a FileStart header — so exactly one predicate decides whether it may be joined
 * onto the sync root. It lives here, rather than private to whichever layer
 * happened to need it first, so the manifest side and the transfer side cannot
 * drift apart on the answer, and so it can be tested directly.
 *
 * A path is acceptable only when joining it to the root cannot land outside the
 * root: non-empty, relative, POSIX-separated, no drive letter, and no "." or ".."
 * component. This is deliberately stricter than "a legal filename": a name a
 * platform happens to allow but rasync cannot represent safely everywhere is
 * simply not synced.
 */

#include <string>

namespace rasync {

inline bool is_safe_relpath(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.front() == '\\') return false;
    if (p.find('\\') != std::string::npos) return false;
    if (p.size() >= 2 && p[1] == ':') return false;  // C:...
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg == "." || seg == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

} // namespace rasync
