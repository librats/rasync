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

/// How a modify/modify (or modify/delete) conflict is resolved. Both peers must
/// use the same policy so they pick the same winner and converge.
enum class ConflictPolicy { Newer, Larger, PreferLocal, PreferRemote };

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
    size_t local_work() const { return pull.size() + delete_local.size(); }
};

/// Compute this peer's plan. `base` is the last-synced manifest (may be empty on
/// first sync); ignored in mirror mode.
SyncPlan reconcile(const Manifest& base,
                   const Manifest& local,
                   const Manifest& remote,
                   const ReconcileOptions& opts);

/// The manifest both peers should persist as the new baseline after a successful
/// two-way round: the winning version of every surviving path. Deterministic and
/// identical on both sides given the same inputs and policy.
Manifest merged_baseline(const Manifest& base,
                         const Manifest& local,
                         const Manifest& remote,
                         const ReconcileOptions& opts);

} // namespace rasync
