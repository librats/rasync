#pragma once

/**
 * @file sync_service.h
 * @brief One synced folder: its manifests, and a SyncSession per peer sharing it.
 *
 * SyncService is the application-side glue for a *single* folder: it owns the
 * authoritative local + baseline manifests and spins up one SyncSession per peer
 * that turns out to share the folder. The daemon feeds it fresh manifests after
 * each scan; it fans those out to the sessions, which do the actual reconcile +
 * transfer.
 *
 * It does not touch the librats Node's handlers — a node carries every folder at
 * once, so registering the channel and the peer up/down callbacks belongs to
 * SyncRouter (net/sync_router.h), which owns the services and hands each one the
 * traffic tagged for it. The service still holds a Node reference, because
 * sending is per-peer and needs one.
 *
 * Everything the sessions need from the outside world — sending on the channel,
 * reading the current manifests, persisting the baseline, reporting progress —
 * goes through this one class, so a session never touches the Node or the UI
 * directly.
 */

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/diff.h"
#include "core/manifest.h"
#include "core/delta.h"
#include "net/protocol.h"
#include "net/send_gate.h"

#include "librats/node/node.h"
#include "librats/peer/peer_id.h"

namespace rasync {

/// Scratch directory for in-flight downloads, a sibling of the synced files so a
/// finished transfer can be renamed into place without crossing a filesystem —
/// the one thing rasync writes into the tree, and only while a transfer is
/// running. It is created on demand, removed as soon as the last transfer of a
/// round lands, and wiped at both startup and shutdown, so an idle rasync leaves
/// the synced directory holding nothing but the user's own files. Never synced
/// (the scanner's ignore list excludes it by this exact name).
constexpr const char* kTempDirName = ".rasync-tmp";

/// Everything that shapes how one directory is synced.
struct SyncConfig {
    std::string    root;                 ///< absolute path of the synced directory
    std::string    data_dir;             ///< rasync state (baseline); see core/state_dir.h

    /// The folder's name, as *both* peers know it. This is the only thing that
    /// says "your D:\docs and my /home/me/docs are the same tree": local paths
    /// differ between machines, so they cannot be it. It is hashed into the
    /// routing tag every message carries (proto::folder_tag) and sent verbatim in
    /// Hello, where the peer checks it. Two peers that name one folder
    /// differently simply never meet on it — each logs the other's name as a
    /// folder it does not have. Defaults to the directory's own leaf name.
    std::string    folder;

    SyncMode       mode              = SyncMode::TwoWay;
    ConflictPolicy conflict          = ConflictPolicy::Newer;
    bool           source            = false;   ///< mirror mode: this side is authoritative
    bool           propagate_deletes = true;

    /// Treat a symbolic link as the file or directory it points at, both when
    /// scanning (see core/scanner.h) and when a transfer lands on one: the bytes
    /// then replace the *target*, exactly as they already do for a file inside a
    /// linked directory, where the OS resolves the path for us. Renaming over the
    /// link instead would quietly turn it into a copy and detach the tree from the
    /// data it was deliberately pointed at.
    ///
    /// Purely local: nothing about it is negotiated, because a manifest describes
    /// files either way. One peer may follow links while the other has none.
    bool           follow_symlinks   = false;

    /// If non-empty, the only peers we will sync with. Anyone else still
    /// completes the librats handshake (they hold the shared key, or there is no
    /// key) but is then ignored outright: no session, so we never describe our
    /// tree and never serve a request. This is the second lever on top of the
    /// key — it survives a leaked key, and it is the only one available when
    /// running keyless. Empty = any peer that handshakes may sync.
    std::unordered_set<librats::PeerId, librats::PeerId::Hash> allowed_peers;

    bool     use_delta        = true;                       ///< rsync-style delta for changed files
    uint32_t block_size       = kDefaultBlockSize;
    uint32_t chunk_size       = 64 * 1024;                  ///< whole-file / literal chunk payload
    uint64_t window_bytes     = 4ull * 1024 * 1024;         ///< max un-acked bytes in flight
    uint64_t delta_max_bytes  = 256ull * 1024 * 1024;       ///< above this, skip delta (bounded memory)

    /// How many pulls may be requested at once. Requests are not free: each one
    /// carries an rsync signature (~1% of the file) and parks it on the peer's
    /// serve queue, so asking for a whole plan at once can both exhaust memory
    /// and overrun the transport's send high-water mark. The serve side is
    /// serial anyway — a handful in flight is enough to keep it busy.
    uint32_t max_pending_pulls = 8;

    /// Largest ManifestUpdate message to put on the wire; bigger updates are
    /// split into chunks the receiver reassembles. Configurable so the chunking
    /// path can be exercised without building a million-file tree.
    size_t   max_update_bytes = proto::kMaxUpdateBytes;

    /// How long a background sender waits for the transport's send queue to drain
    /// before giving up and pushing on anyway (see net/send_gate.h). The wait has
    /// to end somewhere: a peer that never drains is a peer the transport will
    /// disconnect on its own, and a thread parked here forever would serve that
    /// folder nothing ever again. Timing out restores the un-paced behaviour for
    /// one message rather than wedging the session.
    uint32_t send_stall_timeout_ms = 30000;
};

/// One finished file transfer, for the UI.
struct TransferInfo {
    enum Direction { Send, Recv };
    Direction   direction = Recv;
    std::string path;
    uint64_t    total = 0;      ///< file size
    bool        delta = false;  ///< sent as an rsync delta
    uint64_t    on_wire = 0;    ///< bytes that actually crossed the network
};

/// Per-folder UI hooks. All are optional; all fire from librats reactor / session
/// threads, so a handler must be quick and thread-safe.
///
/// Peer up/down is deliberately absent: a connection is node-level and carries
/// every folder, so reporting it per folder would count one peer N times. It
/// lives on RouterEvents (net/sync_router.h) instead.
struct SyncEvents {
    std::function<void(const librats::PeerId&, const SyncPlan&)>   round;
    std::function<void(const TransferInfo&)>                       file_done;
    std::function<void(const librats::PeerId&, uint64_t files, uint64_t bytes)> synced;
    std::function<void(int level, const std::string&)>            log;
};

class SyncSession;  // net/sync_session.h

/// The folder name a directory gets when the user does not choose one: the
/// directory's own leaf. Convenient, and right whenever both machines happen to
/// call the tree the same thing — which is why a mismatch has to be *reported*
/// rather than silently produce two folders that never meet (see SyncRouter).
std::string default_folder_name(const std::string& root);

class SyncService {
public:
    /// `config.folder` defaults to default_folder_name(config.root) when empty.
    ///
    /// `gate` is the node-wide send gate (net/send_gate.h), owned by the
    /// SyncRouter because backpressure is a property of the connection and every
    /// folder shares one. Null means no pacing — every send goes straight out, as
    /// it did before the gate existed.
    SyncService(librats::Node& node, SyncConfig config, SyncEvents events = {},
                SendGate* gate = nullptr);
    ~SyncService();

    SyncService(const SyncService&) = delete;
    SyncService& operator=(const SyncService&) = delete;

    /// The folder's shared name and the routing tag derived from it. The tag is
    /// fixed at construction because every message carries it and it must not
    /// change under a live session.
    const std::string& folder() const noexcept { return config_.folder; }
    uint32_t           tag()    const noexcept { return tag_; }

    // — driven by SyncRouter, all on the librats reactor thread —

    /// A peer finished the librats handshake. Announces this folder to it (one
    /// Hello) if the allow-list permits; deliberately creates no session, so a
    /// peer that does not have this folder costs one message and no threads.
    void peer_connected(const librats::PeerId& id);

    /// A peer went away: drop and stop its session for this folder, if any.
    void peer_disconnected(const librats::PeerId& id);

    /// One inbound message for this folder, already stripped of its header
    /// (proto::kHeaderBytes). `op` is the opcode the router peeled off.
    void handle_message(const librats::PeerId& from, proto::Op op, BinaryReader& body);

    /// Load the persisted baseline (if any) from data_dir. Call once at startup.
    /// data_dir lives outside the synced tree by default (see core/state_dir.h).
    void load_baseline();

    /// Drop any `.rasync-tmp` leftovers from a previous run (a crash or a kill
    /// mid-transfer strands `.part` files there). Call once at startup, BEFORE
    /// node.start() — it is unconditional, so it must not race a live transfer.
    void clean_temp_dir();

    /// Replace the current local manifest wholesale and nudge every session.
    /// Used for the first scan; afterwards prefer apply_local_patch, which does
    /// not discard mutations the sessions made while the scan was running.
    void set_local_manifest(Manifest m);

    /// Fold a scan's findings into the local manifest and nudge every session.
    /// Only the paths the scan actually saw change are touched, so a file that
    /// landed mid-scan (and is therefore in neither of the scan's endpoints)
    /// keeps the entry the transfer recorded instead of being forgotten and
    /// pulled a second time.
    void apply_local_patch(const ManifestPatch& patch);

    /// Stop and drop all sessions, then clear the temp directory out of the
    /// synced tree (called before node.stop()). Idempotent; after it, a late
    /// message from the reactor no longer creates a session.
    void shutdown();

    // — accessors used by sessions (all thread-safe) —
    const SyncConfig& config() const { return config_; }
    const SyncEvents& events() const { return events_; }
    Manifest local_manifest() const;
    Manifest base_manifest()  const;
    void     set_base_manifest(const Manifest& m);   ///< also persists to disk

    /// Bumped on every mutation of the local or baseline manifest. Comparing it
    /// is O(1), which lets a session skip a full reconcile when nothing can have
    /// changed since the last one — the alternative is re-deciding the whole tree
    /// after every single transferred file.
    uint64_t manifest_version() const noexcept { return manifest_version_.load(); }

    /// Manifest-advertisement traffic produced so far (messages, encoded bytes).
    /// Exposed because "how much bookkeeping did a sync cost?" is the property
    /// incremental updates exist to bound — and the one worth a regression test.
    uint64_t advert_messages() const noexcept { return advert_messages_.load(); }
    uint64_t advert_bytes()    const noexcept { return advert_bytes_.load(); }
    void     note_advert_sent(size_t bytes);

    /// Apply a locally-observed mutation to the tracked local manifest (so the
    /// session's next reconcile sees convergence without waiting for a rescan).
    void note_local_set(const std::string& path, const FileMeta& meta);
    void note_local_removed(const std::string& path);

    std::string abs_path(const std::string& rel) const;
    std::string temp_dir() const;                    ///< <root>/.rasync-tmp
    std::string ensure_temp_dir() const;             ///< create it (hidden on Windows) and return it
    void        prune_temp_dir() const;              ///< remove it again once nothing is in flight
    /// Put one message on the wire without pacing. For reactor-thread callers
    /// only — they cannot block, so keep what goes out this way small and
    /// bounded (acks, NotFound, Bye). Returns false when librats' queue for this
    /// peer is filling up; a reactor-thread caller has no choice but to ignore
    /// that, which is exactly why bulk traffic must not take this path.
    bool        send(const librats::PeerId& to, const Bytes& msg);
    /// Put one message on the wire from a background thread, waiting afterwards
    /// if the transport says its queue is filling. This is the path every bulk
    /// message takes — see net/send_gate.h for why sending without it is what
    /// closed connections. `keep_waiting` abandons the wait (a session passes its
    /// `running_` flag). False means the caller was told to carry on without room
    /// (shutdown, disconnect, or the stall timeout) and may be ignored.
    bool        send_paced(const librats::PeerId& to, const Bytes& msg,
                           const std::function<bool()>& keep_waiting);
    /// Re-check every thread parked in the gate. Called when a session stops, so
    /// a sender waiting on a peer that just went away is released before anyone
    /// tries to join it.
    void        wake_senders() const;
    void        log(int level, const std::string& msg) const;

private:
    bool peer_allowed(const librats::PeerId& id) const;  ///< against config_.allowed_peers
    void send_hello(const librats::PeerId& to);
    void notify_sessions();   ///< tell every session the local tree moved
    std::shared_ptr<SyncSession> session_for(const librats::PeerId& id);

    librats::Node&   node_;
    SendGate*        gate_;   ///< node-wide, owned by SyncRouter; may be null
    SyncConfig       config_;
    SyncEvents       events_;
    const uint32_t   tag_;    ///< proto::folder_tag(config_.folder)

    // Lock order, where both are held: a session's own mutex first, then this one.
    // Nothing here calls back into a session while holding it.
    mutable std::mutex    manifest_mutex_;
    Manifest              local_;
    Manifest              base_;
    std::atomic<uint64_t> manifest_version_{0};

    std::atomic<uint64_t> advert_messages_{0};
    std::atomic<uint64_t> advert_bytes_{0};

    std::mutex sessions_mutex_;
    std::unordered_map<librats::PeerId, std::shared_ptr<SyncSession>, librats::PeerId::Hash> sessions_;
    std::atomic<bool> stopped_{false};   ///< set by shutdown(); no session is created after it
};

} // namespace rasync
