#include <gtest/gtest.h>

#include "app/dial_retry.h"

using namespace rasync;
using namespace std::chrono;

namespace {

// A hand-driven clock: the policy is pure, so a test never has to wait for one.
struct Clock {
    DialRetry::Clock::time_point now{DialRetry::Clock::time_point{} + hours(1)};
    void advance(milliseconds d) { now += d; }
};

constexpr auto kFirst = seconds(8);
constexpr auto kCap   = seconds(120);

} // namespace

TEST(DialRetryTest, NeverDialsWhileAPeerIsConnected) {
    DialRetry r;
    Clock c;
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(r.due(c.now, 1));
        c.advance(seconds(30));
    }
}

TEST(DialRetryTest, WaitsTheFirstDelayBeforeRedialing) {
    DialRetry r;
    Clock c;

    // The tick that first sees nobody connected only arms the timer: the caller
    // has just dialed at startup, and a retry on top of an attempt still in
    // flight is what makes two dials race to replace each other.
    EXPECT_FALSE(r.due(c.now, 0));

    c.advance(kFirst - milliseconds(1));
    EXPECT_FALSE(r.due(c.now, 0)) << "redialed before the first delay elapsed";

    c.advance(milliseconds(1));
    EXPECT_TRUE(r.due(c.now, 0));
}

TEST(DialRetryTest, BacksOffAndCaps) {
    DialRetry r;
    Clock c;
    r.due(c.now, 0);                       // arm

    // Each attempt doubles the wait before the next one.
    auto expect_fires_after = [&](milliseconds wait) {
        c.advance(wait - milliseconds(1));
        EXPECT_FALSE(r.due(c.now, 0)) << "fired early at a " << wait.count() << "ms wait";
        c.advance(milliseconds(1));
        EXPECT_TRUE(r.due(c.now, 0)) << "did not fire at a " << wait.count() << "ms wait";
    };

    expect_fires_after(kFirst);            // 8s  → delay becomes 16s
    expect_fires_after(seconds(16));
    expect_fires_after(seconds(32));
    expect_fires_after(seconds(64));

    // Doubling 64s would overshoot; it settles on the cap and stays there.
    expect_fires_after(kCap);
    EXPECT_EQ(r.delay(), kCap);
    expect_fires_after(kCap);
    EXPECT_EQ(r.delay(), kCap);
}

TEST(DialRetryTest, ReconnectingResetsTheBackoff) {
    DialRetry r;
    Clock c;

    // Grow the delay well past its starting value.
    r.due(c.now, 0);
    for (int i = 0; i < 4; ++i) { c.advance(kCap); r.due(c.now, 0); }
    EXPECT_GT(r.delay(), kFirst);

    // A peer shows up: the next outage must not inherit the previous one's wait,
    // or a link that flaps once ends up taking two minutes to come back.
    EXPECT_FALSE(r.due(c.now, 2));
    EXPECT_EQ(r.delay(), kFirst);

    EXPECT_FALSE(r.due(c.now, 0));         // arms again
    c.advance(kFirst);
    EXPECT_TRUE(r.due(c.now, 0));
}
