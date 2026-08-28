#include "net/send_gate.h"

#include <algorithm>

namespace rasync {

bool SendGate::wait_writable(const librats::PeerId& peer,
                             const std::function<bool()>& keep_waiting,
                             std::chrono::milliseconds timeout) {
    if (stopped_.load()) return false;

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + timeout;
    waits_.fetch_add(1, std::memory_order_relaxed);

    const auto started = Clock::now();

    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        if (stopped_.load()) return false;
        if (keep_waiting && !keep_waiting()) return false;

        const auto now = Clock::now();
        // Writability is asked of the node rather than remembered here: it is the
        // connection's property and it changes on a reactor thread, so a copy kept
        // in this class would only ever be stale. It is also false for a peer that
        // has gone away, which is the right answer for a waiter on a dead peer —
        // it stops waiting and lets its session tear down.
        //
        // The elapsed-time guard in front of it is not politeness: see kSettle. A
        // refusal caused by bytes still queued for the reactor leaves
        // `peer_writable` true throughout, so without the floor this returns at
        // once and the caller carries straight on filling the queue.
        const bool settled = (now - started) >= kSettle;
        if (settled && node_.peer_writable(peer)) return true;

        if (now >= deadline) {
            timeouts_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto step = settled
                              ? kPoll
                              : std::chrono::duration_cast<std::chrono::milliseconds>(
                                    kSettle - (now - started)) + std::chrono::milliseconds(1);
        cv_.wait_for(lk, std::min(step, left));
    }
}

void SendGate::wake_all() {
    // No lock: every waiter re-derives its answer from the node and from its own
    // `keep_waiting`, neither of which this class owns, so there is no state here
    // for a wake-up to race against — only a sleep to cut short.
    cv_.notify_all();
}

void SendGate::stop() {
    if (stopped_.exchange(true)) return;
    cv_.notify_all();
}

} // namespace rasync
