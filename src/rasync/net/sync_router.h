#pragma once

/**
 * @file sync_router.h
 * @brief The one bridge between a librats Node and every folder it syncs.
 *
 * A node carries all of a user's folders over a single connection per peer, so
 * the channel handler and the peer up/down callbacks can only be registered
 * once — by this class. It owns the SyncServices (one per folder), registers the
 * rasync channel on the Node before start(), and hands each inbound message to
 * the folder its tag names.
 *
 * Why one node and not one per folder: the node's identity *is* the PeerId that
 * `--allow` lists and that peers recognise us by, and a second node would mean a
 * second identity, a second listen port, a second DHT instance, and a second
 * Noise handshake with a peer we are already talking to. Folders are cheap;
 * nodes are not.
 *
 * Routing is a plain lookup on the u32 tag every message carries (see
 * protocol.h). The folder table is built before attach() and never mutated
 * afterwards, so the dispatch path — which every 64 KiB of file data goes
 * through — needs no lock at all.
 *
 * A message whose tag matches no folder of ours is dropped. That is the normal
 * case whenever two peers do not share every folder, so it must be cheap and
 * silent; the exception is a `Hello`, which names its folder in full and is
 * therefore worth reporting once per peer. That report is the only thing
 * standing between a user and a silent no-op when the two sides have named one
 * tree differently, which is the mistake this design makes easiest to commit.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/sync_service.h"

#include "librats/node/node.h"
#include "librats/peer/peer_id.h"

namespace rasync {

/// Node-level UI hooks, as opposed to the per-folder SyncEvents. A connection
/// serves every folder at once, so it is reported here exactly once.
struct RouterEvents {
    std::function<void(const librats::PeerId&)>        peer_up;
    std::function<void(const librats::PeerId&)>        peer_down;
    std::function<void(int level, const std::string&)> log;
};

class SyncRouter {
public:
    SyncRouter(librats::Node& node, RouterEvents events = {});
    ~SyncRouter();

    SyncRouter(const SyncRouter&) = delete;
    SyncRouter& operator=(const SyncRouter&) = delete;

    /// Add a folder. Must be called BEFORE attach(); the table is fixed after
    /// that so dispatch can run lock-free.
    ///
    /// Returns nullptr if attach() has already run, or if another folder holds
    /// this one's tag — which means either the same name twice, or (with
    /// probability around 2⁻³²) two names that hash alike. Either way the caller
    /// must reject the folder rather than route two trees to one place.
    SyncService* add_folder(SyncConfig config, SyncEvents events = {});

    /// Register the channel and peer handlers on the node. Call once, AFTER every
    /// add_folder and BEFORE node.start().
    void attach();

    /// Per-folder startup: load each baseline and clear stale temp files. Call
    /// after attach() and before node.start(), while no transfer can be live.
    void start_folders();

    /// Stop every folder's sessions and go inert. Call before node.stop().
    /// Idempotent.
    void shutdown();

    /// The folder holding `tag`, or nullptr. Safe from any thread once attached.
    SyncService* folder_for(uint32_t tag) const;

    /// The folder named `name`, or nullptr.
    SyncService* folder_named(const std::string& name) const;

    const std::vector<std::unique_ptr<SyncService>>& folders() const noexcept { return folders_; }
    size_t                                           size()    const noexcept { return folders_.size(); }

private:
    void on_message(const librats::PeerId& from, librats::ByteView payload);
    /// Report a message for a folder we do not have — at most once per (peer, tag).
    void report_unknown(const librats::PeerId& from, uint32_t tag, librats::ByteView payload);
    void log(int level, const std::string& msg) const;

    librats::Node&                            node_;
    RouterEvents                              events_;
    std::vector<std::unique_ptr<SyncService>> folders_;
    std::unordered_map<uint32_t, SyncService*> by_tag_;   ///< fixed once attached
    bool                                      attached_ = false;
    std::atomic<bool>                         stopped_{false};

    /// (peer, tag) pairs already reported as unknown. Touched only off the happy
    /// path — a folder the peer does not share produces exactly one Hello — so a
    /// plain mutex here costs nothing that matters.
    mutable std::mutex                    unknown_mutex_;
    std::unordered_set<std::string>       unknown_seen_;
};

} // namespace rasync
