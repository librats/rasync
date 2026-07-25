#pragma once

/**
 * @file sync_session.h
 * @brief The per-peer reconciliation + transfer state machine.
 *
 * One SyncSession drives the protocol with exactly one peer. It is symmetric:
 * both ends run the same logic. On any manifest change (ours or theirs) it
 * reconciles the two trees, applies its own deletions, and requests the files it
 * needs; concurrently it serves the peer's requests from a background sender
 * thread (so large reads/sends never block the librats reactor). Files it already
 * has an older copy of are pulled as rsync deltas.
 *
 * Two things keep the session's cost proportional to what *changed* rather than
 * to the size of the tree, which is what makes a large tree workable at all:
 *
 *  - **Advertisements are incremental.** `last_sent_` is the tree as the peer
 *    last saw it; every advertisement ships the diff against it. Receiving a file
 *    only marks the session dirty — the update is coalesced and flushed once the
 *    inbound queue drains, instead of re-describing the whole tree N times.
 *  - **Pulls are windowed.** A plan may name a million paths; only
 *    `max_pending_pulls` are requested at a time, and the requester thread builds
 *    their rsync signatures off the reactor.
 *
 * Threading: protocol messages and control calls arrive on the reactor thread and
 * mutate state under `mtx_`. A sender thread drains the serve queue, honouring a
 * byte window (FileAck) via `send_cv_`; a requester thread drains the pull queue,
 * where each request costs a full read of our copy of the file. Incoming file
 * writes happen on the reactor thread into a temp file, verified by SHA-256 and
 * atomically renamed into place on FileEnd. Where a session lock and the
 * service's manifest lock are both needed, `mtx_` is always taken first.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/delta.h"
#include "core/manifest.h"
#include "net/protocol.h"

#include "peer/peer_id.h"
#include "util/fs.h"

extern "C" {
#include "sha256.h"
}

namespace rasync {

class SyncService;

class SyncSession : public std::enable_shared_from_this<SyncSession> {
public:
    SyncSession(SyncService& service, librats::PeerId peer);
    ~SyncSession();

    void start();                              ///< say Hello; begin discovery
    void stop();                               ///< tear down: join sender, drop temp files
    void handle(librats::ByteView payload);    ///< one inbound protocol message (reactor thread)
    void local_changed();                      ///< the local tree changed — re-advertise + reconcile

    const librats::PeerId& peer() const { return peer_; }

private:
    // — receive side (reactor thread) —
    struct Incoming {
        uint64_t          xid = 0;
        std::string       rel_path;
        std::string       temp_path;
        std::string       final_path;
        uint64_t          size = 0;
        int64_t           mtime = 0;
        uint32_t          mode = 0;
        bool              is_delta = false;
        librats::FileStream out;      ///< temp file being written
        librats::FileStream base;     ///< base file for delta COPY reads
        bool              base_open = false;
        uint64_t          written = 0;
        uint64_t          on_wire = 0;   ///< literal bytes actually received
        uint64_t          last_ack = 0;  ///< `on_wire` at the last FileAck we sent
        sha256_context_t  hash{};
        Hash              result_hash{};  ///< whole-file digest, filled at verify time
        bool              failed = false;
        ~Incoming();
    };

    // — send side (sender thread) —
    struct Serve {
        std::string rel_path;
        bool        has_sig = false;
        Signature   sig;
        uint64_t    xid = 0;
    };

    // message handlers
    void handle_hello(BinaryReader& r);
    void handle_manifest_update(BinaryReader& r);
    void handle_request(BinaryReader& r);
    void handle_file_start(BinaryReader& r);
    void handle_file_literal(BinaryReader& r);
    void handle_file_copy(BinaryReader& r);
    void handle_file_end(BinaryReader& r);
    void handle_file_ack(BinaryReader& r);
    void handle_not_found(BinaryReader& r);

    // reconciliation
    void send_hello();
    void advertise();               ///< ship whatever the peer hasn't heard yet
    void advertise_locked();        ///< …with mtx_ already held (keeps updates ordered)
    void send_update_locked(const ManifestPatch& patch, bool reset);
    void reconcile_and_act();       ///< the round: delete + queue pulls
    void maybe_synced();            ///< emit "in sync" + persist baseline at quiescence

    // pulling (requester thread)
    void requester_loop();
    void send_request(const std::string& rel_path);
    bool may_request_locked() const;  ///< a queued pull and a free window slot
    void release_pull(const std::string& rel_path);  ///< done with a path; frees a slot

    // serving (sender thread)
    void sender_loop();
    void serve_one(const Serve& job);
    void serve_delta(const Serve& job, const std::string& abs, uint64_t size, int64_t mtime, uint32_t mode);
    void serve_whole(const Serve& job, const std::string& abs, uint64_t size, int64_t mtime, uint32_t mode);
    void window_wait(uint64_t xid);       ///< block until in-flight bytes drop below the window
    void note_sent(uint64_t xid, uint64_t bytes);

    // helpers
    void finalize_incoming(const std::shared_ptr<Incoming>& in);
    void fail_incoming(const std::shared_ptr<Incoming>& in, const std::string& why);
    void send_msg(BinaryWriter& w);
    uint64_t next_xid() { return next_xid_.fetch_add(1); }

    SyncService&    service_;
    librats::PeerId peer_;
    std::string     temp_tag_;   ///< peer-derived prefix that scopes our temp files

    std::mutex               mtx_;             ///< guards the state below
    Manifest                 remote_;          ///< peer's tree, as described so far
    bool                     have_remote_ = false;
    uint64_t                 remote_version_ = 0;  ///< bumped when remote_ changes
    Manifest                 staging_;         ///< a multi-chunk update being assembled
    bool                     staging_active_ = false;

    Manifest                 last_sent_;       ///< our tree as the peer last saw it
    bool                     last_sent_valid_ = false;
    uint64_t                 last_sent_version_ = 0;  ///< manifest version behind last_sent_
    bool                     advertise_pending_ = false;  ///< owed an update once inbound work drains

    Hash                     last_synced_fp_{}; ///< suppress duplicate "in sync" events
    bool                     last_synced_valid_ = false;
    /// Manifest versions at the last convergence check. Re-running the check
    /// against unchanged inputs would just re-derive the same answer over the
    /// whole tree, and it is asked after every single transferred file.
    uint64_t                 last_eval_local_ver_ = 0;
    uint64_t                 last_eval_remote_ver_ = 0;
    bool                     last_eval_valid_ = false;

    /// Paths this side is responsible for pulling: every one of them is either
    /// still in `pull_queue_` or has been requested and is awaiting data. The
    /// difference of the two sizes is what is in flight.
    std::unordered_set<std::string>                       pulling_;
    std::deque<std::string>                               pull_queue_;
    std::unordered_map<uint64_t, std::shared_ptr<Incoming>> incoming_;

    std::thread              requester_;
    std::condition_variable  pull_cv_;          ///< wakes the requester: new pulls / free slot

    // sender thread + serve queue
    std::thread              sender_;
    std::deque<Serve>        serve_q_;
    std::atomic<int>         outstanding_serves_{0};
    std::atomic<bool>        running_{false};

    // send window (separate lock so disk reads / acks don't contend on mtx_)
    std::mutex               send_mtx_;
    std::condition_variable  send_cv_;
    uint64_t                 cur_xid_ = 0;
    uint64_t                 cur_sent_ = 0;
    uint64_t                 cur_acked_ = 0;

    std::condition_variable  queue_cv_;         ///< wakes the sender for new jobs / shutdown
    std::atomic<uint64_t>    next_xid_{1};
};

} // namespace rasync
