#pragma once

/**
 * @file send_gate.h
 * @brief The one place rasync waits for a peer's send queue to drain.
 *
 * librats bounds what it will buffer for a peer: past its send high-water mark
 * (8 MiB by default) the peer is dropped as a slow consumer, and `Node::send`
 * answers `false` long before that — at a quarter of the mark — to say so while
 * there is still room to stop. That answer is the *only* backpressure signal the
 * transport gives, and it is per **peer**, not per folder.
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

#include "librats/node/node.h"
#include "librats/peer/peer_id.h"

namespace rasync {

class SendGate {
public:
    explicit SendGate(librats::Node& node) : node_(node) {}

    SendGate(const SendGate&) = delete;
    SendGate& operator=(const SendGate&) = delete;

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

    /// Re-check every waiter. Called when a peer's queue drops back below the
    /// low-water mark, when a peer disconnects, and when a session stops — all of
    /// which can turn a waiter's answer from "keep waiting" into "stop".
    void wake_all();

    /// Release every waiter and refuse to block again. Idempotent.
    void stop();

    // — observability; a stalled sender is otherwise invisible, and these are what
    //   a test asserts on to know the gate really engaged —
    uint64_t waits()    const noexcept { return waits_.load(); }
    uint64_t timeouts() const noexcept { return timeouts_.load(); }

private:
    /// Two different refusals hide behind one `false` from `Node::send`, and they
    /// need different waits.
    ///
    ///  - The **connection's** queue is over its mark. Visible here as
    ///    `peer_writable`, and its recovery arrives as an event — so a waiter can
    ///    sleep until woken, re-polling every `kPoll` in case the wake-up was for
    ///    somebody else.
    ///
    ///  - The bytes we just handed over **have not reached the reactor yet**
    ///    (`Node::send` charges them the moment it is called and the reactor
    ///    discharges them when it runs). Nothing observable changes and no event
    ///    ever fires: the connection itself never crossed anything. A wait that
    ///    only re-read `peer_writable` therefore returned immediately and paced
    ///    nothing at all — which is exactly how a tight sender loop walked the
    ///    queue up to the high-water mark with the gate in place.
    ///
    /// `kSettle` is the floor that answers the second: never come back before the
    /// reactor has had a turn to drain what it was handed. It costs one short
    /// sleep per refusal, and a refusal only happens once the sender is already
    /// ahead of the reactor by a quarter of the whole queue — so the throughput it
    /// can cost is a chunk per millisecond in the worst case, and nothing at all
    /// in the common one, where the answer was true and nobody waited.
    static constexpr std::chrono::milliseconds kSettle{1};
    static constexpr std::chrono::milliseconds kPoll{20};

    librats::Node&          node_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::atomic<bool>       stopped_{false};
    std::atomic<uint64_t>   waits_{0};
    std::atomic<uint64_t>   timeouts_{0};
};

} // namespace rasync
