#pragma once

/**
 * @file sync_service.h
 * @brief Ties librats' Node to rasync's per-peer SyncSessions.
 *
 * SyncService is the application-side glue: it registers the rasync channel and
 * peer up/down handlers on a librats Node (before start()), owns the authoritative
 * local + baseline manifests, and spins up one SyncSession per connected peer. The
 * daemon feeds it fresh manifests after each scan; it fans those out to the
 * sessions, which do the actual reconcile + transfer.
 *
 * Everything the sessions need from the outside world — sending on the channel,
 * reading the current manifests, persisting the baseline, reporting progress —
 * goes through this one class, so a session never touches the Node or the UI
 * directly.
 */

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/diff.h"
#include "core/manifest.h"
#include "core/delta.h"

#include "node/node.h"
#include "peer/peer_id.h"

namespace rasync {

/// Scratch directory for in-flight downloads, a sibling of the synced files so a
/// finished transfer can be renamed into place without crossing a filesystem.
/// Never synced (the scanner's ignore list excludes it by this exact name).
constexpr const char* kTempDirName = ".rasync-tmp";

/// Everything that shapes how one directory is synced.
struct SyncConfig {
    std::string    root;                 ///< absolute path of the synced directory
    std::string    data_dir;             ///< rasync state (baseline, identity); default <root>/.rasync
    SyncMode       mode              = SyncMode::TwoWay;
    ConflictPolicy conflict          = ConflictPolicy::Newer;
    bool           source            = false;   ///< mirror mode: this side is authoritative
    bool           propagate_deletes = true;

    bool     use_delta        = true;                       ///< rsync-style delta for changed files
    uint32_t block_size       = kDefaultBlockSize;
    uint32_t chunk_size       = 64 * 1024;                  ///< whole-file / literal chunk payload
    uint64_t window_bytes     = 4ull * 1024 * 1024;         ///< max un-acked bytes in flight
    uint64_t delta_max_bytes  = 256ull * 1024 * 1024;       ///< above this, skip delta (bounded memory)
};

/// One in-flight or just-finished file transfer, for the UI.
struct TransferInfo {
    enum Direction { Send, Recv };
    Direction   direction = Recv;
    std::string path;
    uint64_t    bytes = 0;      ///< transferred so far (of the reconstructed file)
    uint64_t    total = 0;      ///< file size
    bool        delta = false;  ///< sent as an rsync delta
    uint64_t    on_wire = 0;    ///< bytes that actually crossed the network
};

/// UI hooks. All are optional; all fire from librats reactor / session threads,
/// so a handler must be quick and thread-safe.
struct SyncEvents {
    std::function<void(const librats::PeerId&)>                    peer_up;
    std::function<void(const librats::PeerId&)>                    peer_down;
    std::function<void(const librats::PeerId&, const SyncPlan&)>   round;
    std::function<void(const TransferInfo&)>                       progress;
    std::function<void(const TransferInfo&)>                       file_done;
    std::function<void(const librats::PeerId&, uint64_t files, uint64_t bytes)> synced;
    std::function<void(int level, const std::string&)>            log;
};

class SyncSession;  // net/sync_session.h

class SyncService {
public:
    SyncService(librats::Node& node, SyncConfig config, SyncEvents events = {});
    ~SyncService();

    SyncService(const SyncService&) = delete;
    SyncService& operator=(const SyncService&) = delete;

    /// Register channel + peer handlers on the node. Call BEFORE node.start().
    void attach();

    /// Load the persisted baseline (if any) from data_dir. Call once at startup.
    void load_baseline();

    /// Drop any `.rasync-tmp` leftovers from a previous run (a crash or a kill
    /// mid-transfer strands `.part` files there). Call once at startup, BEFORE
    /// node.start() — it is unconditional, so it must not race a live transfer.
    void clean_temp_dir();

    /// Replace the current local manifest (called by the daemon after each scan)
    /// and nudge every session to re-sync if it changed.
    void set_local_manifest(Manifest m);

    /// Stop and drop all sessions (called before node.stop()).
    void shutdown();

    // — accessors used by sessions (all thread-safe) —
    const SyncConfig& config() const { return config_; }
    const SyncEvents& events() const { return events_; }
    Manifest local_manifest() const;
    Manifest base_manifest()  const;
    void     set_base_manifest(const Manifest& m);   ///< also persists to disk

    /// Apply a locally-observed mutation to the tracked local manifest (so the
    /// session's next reconcile sees convergence without waiting for a rescan).
    void note_local_set(const std::string& path, const FileMeta& meta);
    void note_local_removed(const std::string& path);

    std::string abs_path(const std::string& rel) const;
    std::string temp_dir() const;                    ///< <root>/.rasync-tmp
    void        send(const librats::PeerId& to, const Bytes& msg);
    void        log(int level, const std::string& msg) const;

private:
    void on_peer_up(const librats::PeerId& id);
    void on_peer_down(const librats::PeerId& id);
    void on_message(const librats::PeerId& from, librats::ByteView payload);
    std::shared_ptr<SyncSession> session_for(const librats::PeerId& id);

    librats::Node&   node_;
    SyncConfig       config_;
    SyncEvents       events_;

    mutable std::mutex manifest_mutex_;
    Manifest           local_;
    Manifest           base_;

    std::mutex sessions_mutex_;
    std::unordered_map<librats::PeerId, std::shared_ptr<SyncSession>, librats::PeerId::Hash> sessions_;
};

} // namespace rasync
