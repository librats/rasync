#pragma once

/**
 * @file send_gate.h
 * @brief The one place rasync waits for a peer's send queue to drain.
 *
 * librats bounds what it will buffer for a peer: past its send high-water mark
 * (8 MiB by default) the peer is dropped as a slow consumer, and `Node::send`
 * answers `false` long before that — at a quarter of the mark — to say so while
 * there is still room to stop. That answer is the transport's backpressure
 * signal, and it is per **peer**, not per folder.
 *
 * That distinction is the whole reason this class exists. A node carries every
 * folder over one connection, but a SyncSession is per (folder, peer) and each
 * one used to pace itself against its own FileAck window alone — so N folders
 * transferring at once offered N windows' worth of data to a queue that only has
 * room for one. Four folders of large files was enough to cross the mark and have
 * the connection closed mid-transfer, on a link that was never unhealthy.
 *
 * A gate is shared by every session of a node, so waiting on it is waiting on the
 * connection rather than on a folder's share of it, and the arithmetic stops
 * depending on how many folders the user happens to sync.
 *
 * **Sending and waiting are one call** (`send`). They were two, and the pacing
 * was then whatever each caller remembered to do with the answer — one path that
 * sent without waiting is all it takes to put the connection back over the mark.
 *
 * Waiting is only ever done by a session's **background** threads (the sender and
 * the requester). Nothing on a reactor thread may block here: a reactor thread
 * blocked waiting for its own queue to drain is the deadlock this class would
 * otherwise be.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "librats/core/bytes.h"
#include "librats/node/node.h"
#include "librats/peer/peer_id.h"

namespace rasync {

class SendGate {
public:
    /// @param channel the librats application channel every message goes out on.
    SendGate(librats::Node& node, std::string channel)
        : node_(node), channel_(std::move(channel)) {}

    SendGate(const SendGate&) = delete;
    SendGate& operator=(const SendGate&) = delete;

    /// Send one message, then wait for room if the transport says its queue is
    /// filling. The message itself always goes out; the wait bounds the *next*
    /// one, which is what backpressure can ever do.
    ///
    /// Returns true when there is room to carry on, false when the caller was
    /// told to stop waiting instead (see wait_writable). Callers may ignore it:
    /// the next send asks the same question again.
    ///
    /// Background threads only — never call this from a reactor thread.
    bool send(const librats::PeerId& to, librats::ByteView payload,
              const std::function<bool()>& keep_waiting,
              std::chrono::milliseconds timeout);

    /// Block until `peer`'s send queue has room again.
    ///
    /// Returns true when there is room, false when the caller should stop waiting
    /// and carry on regardless — `keep_waiting` said so, the peer went away, the
    /// gate was stopped, or `timeout` elapsed. Giving up rather than waiting
    /// forever is deliberate: a wait with no end is how a sender thread becomes
    /// permanently wedged, and the worst a timeout costs is the behaviour we had
    /// before this class existed.
    ///
    /// @param keep_waiting Polled under the lock; return false to abandon the
    ///        wait (a session passes its `running_` flag, so a shutdown or a
    ///        disconnect releases the thread promptly).
    ///
    /// Background threads only — never call this from a reactor thread.
    bool wait_writable(const librats::PeerId& peer,
                       const std::function<bool()>& keep_waiting,
                       std::chrono::milliseconds timeout);

    /// Re-check every waiter. Called when a peer's send queue drops back below
    /// the low-water mark, when a peer disconnects, and when a session stops —
    /// all of which can turn a waiter's answer from "keep waiting" into "stop".
    void wake_all();

    /// Release every waiter and refuse to block again. Idempotent.
    void stop();

    // — observability; a stalled sender is otherwise invisible, and these are what
    //   a test asserts on to know the pacing really is in the path —
    uint64_t offers()   const noexcept { return offers_.load(); }   ///< paced sends
    uint64_t waits()    const noexcept { return waits_.load(); }    ///< of those, the ones told to wait
    uint64_t timeouts() const noexcept { return timeouts_.load(); } ///< of those, the ones that gave up

private:
    /// A waiter sleeps until it is woken and then re-reads `Node::peer_writable`,
    /// which answers the same question `Node::send` does: it weighs both what the
    /// reactor has queued and what has been handed to it that it has not taken up
    /// yet. Only the first of those has an event behind it. A queue that filled
    /// with bytes still in transit never crossed anything on the connection, so
    /// nothing announces the moment they drain — and that is what this poll
    /// interval is for. It is a fallback, not the mechanism: the ordinary wake-up
    /// is `on_peer_writable` arriving through wake_all(), so the value bounds a
    /// rare wait rather than the throughput of a transfer.
    static constexpr std::chrono::milliseconds kPoll{5};

    librats::Node&          node_;
    const std::string       channel_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    /// Bumped by every wake_all(). A waiter reads it before testing its condition
    /// and sleeps only while it is unchanged, so a wake-up that lands in between
    /// — wake_all() deliberately notifies without the lock, from a reactor thread
    /// — is seen rather than lost.
    std::atomic<uint64_t>   generation_{0};
    std::atomic<bool>       stopped_{false};
    std::atomic<uint64_t>   offers_{0};
    std::atomic<uint64_t>   waits_{0};
    std::atomic<uint64_t>   timeouts_{0};
};

} // namespace rasync
