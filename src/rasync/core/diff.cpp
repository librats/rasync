#include "core/diff.h"

#include <algorithm>
#include <set>

namespace rasync {
namespace {

// A file's state relative to the baseline.
enum class Change { Unchanged, Added, Modified, Deleted, Absent };

Change classify(const FileMeta* cur, const FileMeta* base) {
    if (cur) return !base ? Change::Added
                          : (cur->same_content(*base) ? Change::Unchanged : Change::Modified);
    return base ? Change::Deleted : Change::Absent;
}

bool changed(Change c) {
    return c == Change::Added || c == Change::Modified || c == Change::Deleted;
}

/// True if the LOCAL version should win a conflict. Deterministic and symmetric:
/// the peer, computing local_wins(R, L, policy), reaches the complementary
/// answer, so both sides pick the same surviving content.
bool local_wins(const FileMeta* L, const FileMeta* R, ConflictPolicy p) {
    if (L && !R) return p != ConflictPolicy::PreferRemote;  // modify beats delete
    if (!L && R) return p == ConflictPolicy::PreferLocal;
    if (!L && !R) return true;                               // both gone: irrelevant
    switch (p) {
        case ConflictPolicy::PreferLocal:  return true;
        case ConflictPolicy::PreferRemote: return false;
        case ConflictPolicy::Larger:
            if (L->size != R->size) return L->size > R->size;
            [[fallthrough]];
        case ConflictPolicy::Newer:
        default:
            if (L->mtime != R->mtime) return L->mtime > R->mtime;
            // Content differs (it's a conflict) so hashes differ: break the tie on
            // hash bytes — same comparison on both peers ⇒ same winner.
            return std::lexicographical_compare(R->hash.begin(), R->hash.end(),
                                                L->hash.begin(), L->hash.end());
    }
}

/// For identical-content paths that differ only in mtime/mode, pick a canonical
/// survivor so both peers persist the same baseline meta. (mtime is ignored by
/// content comparison, so the choice never triggers a spurious resync.)
const FileMeta* canonical(const FileMeta* L, const FileMeta* R) {
    if (!R) return L;
    if (!L) return R;
    if (L->mtime != R->mtime) return L->mtime > R->mtime ? L : R;
    return L;  // fully identical — either is fine
}

struct Decision {
    enum Kind { None, Pull, DeleteLocal, Push, DeleteRemote } kind = None;
    const FileMeta* survivor = nullptr;  ///< surviving meta (nullptr = deleted)
    bool conflict = false;
};

Decision decide_two_way(const FileMeta* L, const FileMeta* R, const FileMeta* B,
                        const ReconcileOptions& o) {
    if (!L && !R) return {};
    if (L && R && L->same_content(*R)) return {Decision::None, canonical(L, R), false};

    const bool lc = changed(classify(L, B));
    const bool rc = changed(classify(R, B));

    if (lc && !rc) return L ? Decision{Decision::Push, L, false}
                            : Decision{Decision::DeleteRemote, nullptr, false};
    if (rc && !lc) {
        if (R) return {Decision::Pull, R, false};
        // Remote deleted this path. Propagate the deletion, or keep our copy.
        return o.propagate_deletes ? Decision{Decision::DeleteLocal, nullptr, false}
                                   : Decision{Decision::None, L, false};
    }

    // Both changed (or an inconsistent baseline): a genuine conflict.
    const bool lw = local_wins(L, R, o.conflict);
    if (lw) return L ? Decision{Decision::Push, L, true}
                     : Decision{Decision::DeleteRemote, nullptr, true};
    if (R) return {Decision::Pull, R, true};
    return o.propagate_deletes ? Decision{Decision::DeleteLocal, nullptr, true}
                               : Decision{Decision::None, L, true};
}

Decision decide_mirror(const FileMeta* L, const FileMeta* R, const ReconcileOptions& o) {
    if (o.source) {
        if (L) return (R && R->same_content(*L)) ? Decision{Decision::None, L, false}
                                                 : Decision{Decision::Push, L, false};
        return {Decision::DeleteRemote, nullptr, false};  // replica-only extra
    }
    // replica
    if (R) return (L && L->same_content(*R)) ? Decision{Decision::None, R, false}
                                             : Decision{Decision::Pull, R, false};
    return o.propagate_deletes ? Decision{Decision::DeleteLocal, nullptr, false}
                               : Decision{Decision::None, L, false};
}

Decision decide(const std::string& path, const Manifest& base, const Manifest& local,
                const Manifest& remote, const ReconcileOptions& o) {
    const FileMeta* L = local.find(path);
    const FileMeta* R = remote.find(path);
    if (o.mode == SyncMode::Mirror) return decide_mirror(L, R, o);
    return decide_two_way(L, R, base.find(path), o);
}

std::set<std::string> union_keys(const Manifest& a, const Manifest& b, const Manifest& c) {
    std::set<std::string> keys;
    for (const auto& [k, _] : a.entries()) keys.insert(k);
    for (const auto& [k, _] : b.entries()) keys.insert(k);
    for (const auto& [k, _] : c.entries()) keys.insert(k);
    return keys;
}

} // namespace

ConflictPolicy complement(ConflictPolicy p) {
    switch (p) {
        case ConflictPolicy::PreferLocal:  return ConflictPolicy::PreferRemote;
        case ConflictPolicy::PreferRemote: return ConflictPolicy::PreferLocal;
        case ConflictPolicy::Newer:
        case ConflictPolicy::Larger:       break;  // decided by comparing the versions
    }
    return p;
}

const char* conflict_policy_name(ConflictPolicy p) {
    switch (p) {
        case ConflictPolicy::Newer:        return "newer";
        case ConflictPolicy::Larger:       return "larger";
        case ConflictPolicy::PreferLocal:  return "local";
        case ConflictPolicy::PreferRemote: return "remote";
    }
    return "?";
}

SyncPlan reconcile(const Manifest& base, const Manifest& local,
                   const Manifest& remote, const ReconcileOptions& opts) {
    SyncPlan plan;
    const Manifest empty;
    const Manifest& b = (opts.mode == SyncMode::Mirror) ? empty : base;

    for (const auto& path : union_keys(b, local, remote)) {
        Decision d = decide(path, b, local, remote, opts);
        switch (d.kind) {
            case Decision::Pull:         plan.pull.push_back(path); break;
            case Decision::DeleteLocal:  plan.delete_local.push_back(path); break;
            case Decision::Push:         plan.push.push_back(path); break;
            case Decision::DeleteRemote: plan.delete_remote.push_back(path); break;
            case Decision::None:         break;
        }
        if (d.conflict) plan.conflicts.push_back(path);
    }
    return plan;
}

Manifest merged_baseline(const Manifest& base, const Manifest& local,
                         const Manifest& remote, const ReconcileOptions& opts) {
    Manifest out;
    for (const auto& path : union_keys(base, local, remote)) {
        Decision d = decide(path, base, local, remote, opts);
        if (d.survivor) out.set(path, *d.survivor);
    }
    return out;
}

} // namespace rasync
