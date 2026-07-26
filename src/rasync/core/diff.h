#pragma once

/**
 * @file diff.h
 * @brief Reconcile two directory manifests into a per-side sync plan.
 *
 * rasync's protocol is symmetric and pull-based: each peer independently decides
 * which paths to *pull* from the other and which to *delete* locally, and the two
 * plans are complements, so the trees converge without a central coordinator. The
 * whole decision lives here as a pure function of three manifests, which makes it
 * exhaustively unit-testable.
 *
 * Two modes:
 *  - **Two-way** (default): a proper three-way merge against a persisted `base`
 *    (the last synced state). This distinguishes "created here" from "deleted
 *    there", so deletions propagate correctly and edits on both sides surface as
 *    conflicts resolved by a deterministic, identical-on-both-peers policy.
 *  - **Mirror**: one side is the authoritative `source`; the other is made byte-
 *    identical to it (pull differences, optionally delete extras). `base` unused.
 */

#include <string>
#include <vector>

#include "core/manifest.h"

namespace rasync {

enum class SyncMode { TwoWay, Mirror };

/// How a modify/modify (or modify/delete) conflict is resolved. The two peers must
/// run *complementary* policies (see `complement`) so they pick the same winner and
/// converge — which, for `Newer` and `Larger`, means the same policy on both sides.
enum class ConflictPolicy { Newer, Larger, PreferLocal, PreferRemote };

/// The policy a peer must run for its plan to complement ours.
///
/// `Newer` and `Larger` compare the two versions themselves, so both peers reach the
/// same verdict from the same policy: they are their own complement. `PreferLocal`
/// and `PreferRemote` instead name a *side*, and "local" is a different tree on each
/// peer — two peers both preferring local each keep their own version, so both plan a
/// push, neither ever pulls, and the trees never converge. Their complement is each
/// other. `complement(complement(p)) == p` for every policy, so the pairing rule reads
/// the same from either end.
ConflictPolicy complement(ConflictPolicy p);

/// The `--conflict` spelling of a policy ("newer", "larger", "local", "remote"), so
/// a diagnostic can name the flag the user would actually type.
const char* conflict_policy_name(ConflictPolicy p);

struct ReconcileOptions {
    SyncMode       mode             = SyncMode::TwoWay;
    ConflictPolicy conflict         = ConflictPolicy::Newer;
    bool           source           = false;  ///< mirror only: am I the source of truth?
    bool           propagate_deletes = true;  ///< delete files the winning side dropped
};

/// The actions THIS peer will take. `push`/`delete_remote` are informational
/// (they are the peer's own pulls/deletes) and drive the progress summary only.
struct SyncPlan {
    std::vector<std::string> pull;          ///< request these paths from the peer
    std::vector<std::string> delete_local;  ///< remove these paths locally
    std::vector<std::string> conflicts;     ///< paths that conflicted (reporting)

    std::vector<std::string> push;          ///< peer will pull these from us (info)
    std::vector<std::string> delete_remote; ///< peer will delete these (info)

    bool empty() const {
        return pull.empty() && delete_local.empty() && push.empty() && delete_remote.empty();
    }
};

/// Compute this peer's plan. `base` is the last-synced manifest (may be empty on
/// first sync); ignored in mirror mode.
///
/// The new baseline both peers persist afterwards is simply the local manifest at
/// the moment the plan comes back empty: once there is nothing left to pull or
/// delete, the two trees agree, so each side's own view *is* the merged result.
SyncPlan reconcile(const Manifest& base,
                   const Manifest& local,
                   const Manifest& remote,
                   const ReconcileOptions& opts);

} // namespace rasync
