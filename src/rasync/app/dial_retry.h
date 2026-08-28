#pragma once

/**
 * @file dial_retry.h
 * @brief When to dial the `--peer` addresses again after losing them.
 *
 * A `--peer` address used to be dialed exactly once, at startup. Every way a
 * connection can end — the transport's idle timeout, a peer restarting, a laptop
 * changing network — therefore ended the sync for good: both processes stayed up,
 * both kept scanning, and nothing was ever exchanged again until someone noticed
 * and restarted them. Discovery-based setups recover on their own; an explicit
 * `--peer` had nothing to recover with.
 *
 * The policy is deliberately small and pure so it can be tested without a
 * network: feed it a clock and a peer count, and it says whether to dial.
 *
 * Two rules shape it:
 *
 *  - **Only while nothing is connected.** Re-dialing a peer we are already
 *    talking to is not free: librats resolves two connections to one peer by
 *    keeping the newer (`PeerTable::add` — same role, same transport, so the
 *    newcomer is taken for a reconnect), which would tear down the live session
 *    and any transfer running on it. So the trigger is "no peers at all", which
 *    cannot describe a connection worth protecting.
 *
 *  - **Slower than one dial attempt.** A dial resolves in about five seconds
 *    (three Syn attempts on a doubling 500 ms timeout, with TCP raced in after
 *    the 1.2 s fallback delay). Retrying faster than that would put a second dial
 *    in flight beside the first, and the two would race to replace each other.
 *    Hence a first delay comfortably past it, doubling to a cap so an unreachable
 *    peer costs one dial every couple of minutes rather than a permanent trickle.
 */

#include <chrono>

namespace rasync {

class DialRetry {
public:
    using Clock = std::chrono::steady_clock;

    /// @param first Delay before the first retry after peers are lost.
    /// @param cap   Longest the delay grows to.
    explicit DialRetry(std::chrono::milliseconds first = std::chrono::seconds(8),
                       std::chrono::milliseconds cap   = std::chrono::seconds(120))
        : first_(first), cap_(cap), delay_(first) {}

    /// Call on every tick of the watch loop. Returns true when the caller should
    /// dial every `--peer` address again.
    ///
    /// While `connected` is non-zero the backoff resets, so a peer that comes and
    /// goes is re-dialed promptly each time rather than inheriting the delay its
    /// predecessor's outage grew.
    bool due(Clock::time_point now, int connected) {
        if (connected > 0) {
            armed_ = false;
            delay_ = first_;
            return false;
        }
        if (!armed_) {                 // first tick with nobody connected
            armed_ = true;
            next_  = now + delay_;
            return false;
        }
        if (now < next_) return false;
        // Grow *before* re-arming, so the next wait is the longer one.
        delay_ = delay_ * 2 > cap_ ? cap_ : delay_ * 2;
        next_  = now + delay_;
        return true;
    }

    /// The wait currently in force. Exposed for tests and for a log line.
    std::chrono::milliseconds delay() const noexcept { return delay_; }

private:
    std::chrono::milliseconds first_;
    std::chrono::milliseconds cap_;
    std::chrono::milliseconds delay_;
    Clock::time_point         next_{};
    bool                      armed_ = false;
};

} // namespace rasync
