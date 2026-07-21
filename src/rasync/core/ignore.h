#pragma once

/**
 * @file ignore.h
 * @brief gitignore-style path filtering for the scanner.
 *
 * `IgnoreList` decides whether a relative path should be excluded from a sync. It
 * implements a practical subset of gitignore semantics:
 *
 *   - blank lines and `#` comments are skipped;
 *   - `!pattern` negates (re-includes); later patterns win over earlier ones;
 *   - a trailing `/` restricts a pattern to directories;
 *   - a leading `/` anchors the pattern to the sync root;
 *   - a pattern containing no `/` matches that name at *any* depth
 *     (so `node_modules` or `*.log` need no path);
 *   - `*` matches within a path segment, `?` one non-`/` char, `**` any run of
 *     segments.
 *
 * Because the scanner prunes an ignored directory's whole subtree, re-including a
 * file beneath an ignored directory (via `!`) is intentionally not supported —
 * matching gitignore's own "can't re-include under an excluded dir" rule.
 */

#include <string>
#include <vector>

namespace rasync {

class IgnoreList {
public:
    IgnoreList() = default;

    /// Add one gitignore-style pattern. Blank/`#` lines are ignored.
    void add(const std::string& pattern);

    /// Add many newline-separated patterns (e.g. a `.rasyncignore` file body).
    void add_lines(const std::string& text);

    /// Load patterns from a file; missing file is not an error (returns false).
    bool add_file(const std::string& path);

    bool empty() const noexcept { return rules_.empty(); }

    /// Should `relpath` (POSIX, root-relative, no leading '/') be excluded?
    /// `is_dir` selects whether directory-only patterns apply.
    bool matches(const std::string& relpath, bool is_dir) const;

private:
    struct Rule {
        std::string pattern;  ///< normalized (leading '/', trailing '/' stripped)
        bool negate   = false;
        bool dir_only = false;
        bool anchored = false;  ///< contains '/', matched from the root
    };
    std::vector<Rule> rules_;
};

/// Match a single path against one already-parsed glob (exposed for testing).
/// `anchored` matches the whole segment list; otherwise any single segment.
bool glob_match(const std::string& pattern, const std::string& path, bool anchored);

} // namespace rasync
