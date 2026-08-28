#include <gtest/gtest.h>

#include "net/send_gate.h"
#include "net/sync_service.h"
#include "test_util.h"

#include "librats/node/node.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace rasync;
using namespace std::chrono;

namespace {

/// A node that never listens and never dials: every peer is unknown to it, which
/// is exactly the "not writable" answer these tests want to drive. Nothing here
/// touches the network.
struct IdleNode {
    test::TempDir                  state;
    std::unique_ptr<librats::Node> node;

    IdleNode() {
        librats::NodeConfig cfg;
        cfg.enable_listen = false;
        cfg.enable_network_monitor = false;
        cfg.data_dir = state.str();
        node = std::make_unique<librats::Node>(cfg);
    }
};

librats::PeerId some_peer() {
    auto id = librats::PeerId::from_hex(std::string(64, '7'));
    EXPECT_TRUE(id.has_value());
    return id.value_or(librats::PeerId{});
}

constexpr auto kAlive = [] { return true; };

} // namespace

// The wait has to end. A sender parked forever on a peer that will never drain is
// how a session stops serving that folder for the rest of its life — the failure
// this timeout exists to make impossible.
TEST(SendGateTest, GivesUpWhenThePeerNeverBecomesWritable) {
    IdleNode n;
    SendGate gate(*n.node);

    const auto started = steady_clock::now();
    EXPECT_FALSE(gate.wait_writable(some_peer(), kAlive, milliseconds(120)));
    const auto took = duration_cast<milliseconds>(steady_clock::now() - started);

    EXPECT_GE(took.count(), 100) << "returned before the timeout it was given";
    EXPECT_LT(took.count(), 3000) << "overshot its own deadline";
    EXPECT_EQ(gate.waits(), 1u);
    EXPECT_EQ(gate.timeouts(), 1u);
}

// A caller that already knows it is finished must not sit out the timeout: this
// is the path a shutdown takes, and the session joins the thread right after.
TEST(SendGateTest, KeepWaitingSaysNoAndItReturnsAtOnce) {
    IdleNode n;
    SendGate gate(*n.node);

    const auto started = steady_clock::now();
    EXPECT_FALSE(gate.wait_writable(some_peer(), [] { return false; }, seconds(30)));
    EXPECT_LT(duration_cast<milliseconds>(steady_clock::now() - started).count(), 1000);
    EXPECT_EQ(gate.timeouts(), 0u) << "counted as a timeout when it was a clean give-up";
}

// stop() is what SyncRouter::shutdown() calls before joining every sender.
TEST(SendGateTest, StopReleasesAWaiterAndRefusesLaterWaits) {
    IdleNode n;
    SendGate gate(*n.node);

    std::atomic<bool> done{false};
    std::thread t([&] {
        gate.wait_writable(some_peer(), kAlive, seconds(30));
        done.store(true);
    });

    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_FALSE(done.load()) << "did not wait at all";
    gate.stop();

    for (int i = 0; i < 200 && !done.load(); ++i)
        std::this_thread::sleep_for(milliseconds(10));
    EXPECT_TRUE(done.load()) << "stop() did not release the waiter";
    t.join();

    // Past a stop nothing may block again, however long a timeout it asks for.
    const auto started = steady_clock::now();
    EXPECT_FALSE(gate.wait_writable(some_peer(), kAlive, seconds(30)));
    EXPECT_LT(duration_cast<milliseconds>(steady_clock::now() - started).count(), 1000);
}

// A waiter re-reads `keep_waiting` when woken. SyncSession::stop() relies on
// exactly this: it clears `running_`, wakes the gate, and then joins.
TEST(SendGateTest, WakeAllRechecksKeepWaiting) {
    IdleNode n;
    SendGate gate(*n.node);

    std::atomic<bool> running{true};
    std::atomic<bool> done{false};
    std::thread t([&] {
        gate.wait_writable(some_peer(), [&] { return running.load(); }, seconds(30));
        done.store(true);
    });

    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_FALSE(done.load());
    running.store(false);
    gate.wake_all();

    for (int i = 0; i < 200 && !done.load(); ++i)
        std::this_thread::sleep_for(milliseconds(10));
    EXPECT_TRUE(done.load()) << "wake_all() did not make the waiter look again";
    t.join();
}

// A service built without a router has no gate. Pacing must then be a no-op that
// answers "there is room" — not a stall, and not a crash on a null gate.
TEST(SendGateTest, AServiceWithoutAGateNeverPaces) {
    IdleNode n;
    test::TempDir root, data;
    SyncConfig cfg;
    cfg.root = root.str();
    cfg.data_dir = data.str();
    cfg.folder = "gateless";
    SyncService svc(*n.node, cfg);   // no gate argument

    const auto started = steady_clock::now();
    EXPECT_TRUE(svc.wait_writable(some_peer(), [] { return true; }));
    EXPECT_LT(duration_cast<milliseconds>(steady_clock::now() - started).count(), 500);
    svc.wake_senders();              // must be safe with no gate behind it
}
