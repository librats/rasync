#include "core/ignore.h"

#include "librats/util/fs.h"

#include <sstream>

namespace rasync {
namespace {

/// Wildcard match of one path segment (no '/'): '*' = any run, '?' = one char.
/// Classic linear matcher with backtracking on the last '*'.
bool seg_match(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0, star = std::string::npos, mark = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++p; ++s;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;      // remember star position
            mark = s;        // and where we tried to match it as empty
        } else if (star != std::string::npos) {
            p = star + 1;    // backtrack: let the star swallow one more char
            s = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

/// Match anchored segment lists, honouring a `**` that spans zero+ segments.
bool match_segs(const std::vector<std::string>& pat, size_t pi,
                const std::vector<std::string>& path, size_t si) {
    while (pi < pat.size()) {
        if (pat[pi] == "**") {
            if (pi + 1 == pat.size()) return true;         // trailing ** = match rest
            for (size_t k = si; k <= path.size(); ++k)
                if (match_segs(pat, pi + 1, path, k)) return true;
            return false;
        }
        if (si >= path.size()) return false;
        if (!seg_match(pat[pi], path[si])) return false;
        ++pi; ++si;
    }
    return si == path.size();
}

} // namespace

bool glob_match(const std::string& pattern, const std::string& path, bool anchored) {
    if (anchored)
        return match_segs(split(pattern, '/'), 0, split(path, '/'), 0);
    // Unanchored: the pattern (no '/') matches any single segment of the path.
    for (const auto& seg : split(path, '/'))
        if (seg_match(pattern, seg)) return true;
    return false;
}

void IgnoreList::add(const std::string& raw) {
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') return;

    Rule rule;
    if (line[0] == '!') { rule.negate = true; line.erase(0, 1); }
    if (line.empty()) return;
    if (line.back() == '/') { rule.dir_only = true; line.pop_back(); }
    if (!line.empty() && line[0] == '/') { rule.anchored = true; line.erase(0, 1); }
    if (line.find('/') != std::string::npos) rule.anchored = true;
    if (line.empty()) return;

    rule.pattern = std::move(line);
    rules_.push_back(std::move(rule));
}

void IgnoreList::add_lines(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) add(line);
}

bool IgnoreList::add_file(const std::string& path) {
    if (!librats::file_exists(path)) return false;
    add_lines(librats::read_file_text_cpp(path));
    return true;
}

bool IgnoreList::matches(const std::string& relpath, bool is_dir) const {
    bool ignored = false;
    for (const auto& r : rules_) {
        if (r.dir_only && !is_dir) continue;
        if (glob_match(r.pattern, relpath, r.anchored))
            ignored = !r.negate;
    }
    return ignored;
}

} // namespace rasync
