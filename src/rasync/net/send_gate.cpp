#include "net/send_gate.h"

#include <algorithm>

namespace rasync {

bool SendGate::send(const librats::PeerId& to, librats::ByteView payload,
                    const std::function<bool()>& keep_waiting,
                    std::chrono::milliseconds timeout) {
    offers_.fetch_add(1, std::memory_order_relaxed);
    // A true answer is the common case and costs nothing: the queue has room and
    // the next message can follow immediately. A false one is librats saying its
    // queue for this peer is filling — what we just handed over still goes out,
    // nothing is dropped, but offering more would walk the connection up to the
    // send high-water mark, where the peer is dropped as a slow consumer
    // mid-transfer.
    if (node_.send(to, channel_, payload)) return true;
    return wait_writable(to, keep_waiting, timeout);
}

bool SendGate::wait_writable(const librats::PeerId& peer,
                             const std::function<bool()>& keep_waiting,
                             std::chrono::milliseconds timeout) {
    if (stopped_.load()) return false;

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + timeout;
    waits_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        // Read before the tests below, so a wake-up that arrives while we are
        // deciding is not lost: it changes the generation, and the sleep at the
        // bottom refuses to start on a generation that has already moved.
        const uint64_t seen = generation_.load(std::memory_order_relaxed);

        if (stopped_.load()) return false;
        if (keep_waiting && !keep_waiting()) return false;
        // Writability is asked of the node rather than remembered here: it is the
        // connection's property and it changes on a reactor thread, so a copy kept
        // in this class would only ever be stale. It is also false for a peer that
        // has gone away, which is the right answer for a waiter on a dead peer —
        // it stops waiting and lets its session tear down.
        if (node_.peer_writable(peer)) return true;

        const auto now = Clock::now();
        if (now >= deadline) {
            timeouts_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Rounded up, so the last fraction of a millisecond before the deadline is
        // slept through rather than spun on.
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        // Explicit template argument, not a bare std::min: on MSVC with
        // <windows.h> in scope `min` is a function-like macro, and `std::min(`
        // then expands into something that no longer parses.
        cv_.wait_for(lk, std::min<std::chrono::milliseconds>(kPoll, left), [&] {
            return stopped_.load() || generation_.load(std::memory_order_relaxed) != seen;
        });
    }
}

void SendGate::wake_all() {
    // No lock: every waiter re-derives its answer from the node and from its own
    // `keep_waiting`, neither of which this class owns, so there is no state here
    // for a wake-up to race against — only a sleep to cut short. The generation
    // is what makes that safe without the lock; it is bumped before the notify so
    // a waiter that has not slept yet sees the change instead of missing it.
    generation_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_all();
}

void SendGate::stop() {
    if (stopped_.exchange(true)) return;
    generation_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_all();
}

} // namespace rasync
