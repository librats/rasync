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
 * Threading: protocol messages and control calls arrive on the reactor thread and
 * mutate state under `mtx_`. A single sender thread drains the serve queue; it
 * honours a byte window (FileAck) via `send_cv_`. Incoming file writes happen on
 * the reactor thread into a temp file, verified by SHA-256 and atomically renamed
 * into place on FileEnd.
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
    void handle_manifest(BinaryReader& r);
    void handle_request(BinaryReader& r);
    void handle_file_start(BinaryReader& r);
    void handle_file_literal(BinaryReader& r);
    void handle_file_copy(BinaryReader& r);
    void handle_file_end(BinaryReader& r);
    void handle_file_ack(BinaryReader& r);
    void handle_not_found(BinaryReader& r);

    // reconciliation
    void send_hello();
    void send_manifest_if_changed();
    void reconcile_and_act();       ///< the round: delete + request, under mtx_
    void maybe_synced();            ///< emit "in sync" + persist baseline at quiescence

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

    std::mutex               mtx_;             ///< guards the state below
    Manifest                 remote_;          ///< peer's last-advertised manifest
    bool                     have_remote_ = false;
    Hash                     last_sent_fp_{};   ///< fingerprint of the manifest we last sent
    bool                     last_sent_valid_ = false;
    Hash                     last_synced_fp_{}; ///< suppress duplicate "in sync" events
    bool                     last_synced_valid_ = false;

    std::unordered_set<std::string>                       pulling_;   ///< paths requested, not yet done
    std::unordered_map<uint64_t, std::shared_ptr<Incoming>> incoming_;

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
