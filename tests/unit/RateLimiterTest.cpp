// Unit tests for the fixed-window rate limiter (against an in-memory store).

#include "FakeStore.hpp"
#include "miniLB/redis/RateLimiter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using minilb::RateLimiter;

namespace {
constexpr std::int64_t kNow = 1'000'000;   // an arbitrary "current time" in seconds
}

TEST(RateLimiter, AllowsUpToTheLimitThenRejects) {
    FakeStore store;
    RateLimiter limiter(store, /*maxRequests=*/3, /*windowSeconds=*/60);

    EXPECT_TRUE(limiter.allow("1.2.3.4", kNow).allowed);
    EXPECT_TRUE(limiter.allow("1.2.3.4", kNow).allowed);
    EXPECT_TRUE(limiter.allow("1.2.3.4", kNow).allowed);
    EXPECT_FALSE(limiter.allow("1.2.3.4", kNow).allowed);
    EXPECT_FALSE(limiter.allow("1.2.3.4", kNow + 1).allowed);
}

TEST(RateLimiter, EachClientHasItsOwnBudget) {
    FakeStore store;
    RateLimiter limiter(store, 1, 60);

    EXPECT_TRUE(limiter.allow("1.1.1.1", kNow).allowed);
    EXPECT_FALSE(limiter.allow("1.1.1.1", kNow).allowed);
    EXPECT_TRUE(limiter.allow("2.2.2.2", kNow).allowed);
}

TEST(RateLimiter, ANewWindowResetsTheCount) {
    FakeStore store;
    RateLimiter limiter(store, 2, 60);

    const std::int64_t windowStart = (kNow / 60) * 60;
    EXPECT_TRUE(limiter.allow("ip", windowStart).allowed);
    EXPECT_TRUE(limiter.allow("ip", windowStart + 59).allowed);
    EXPECT_FALSE(limiter.allow("ip", windowStart + 59).allowed);

    // One second later a new window begins.
    EXPECT_TRUE(limiter.allow("ip", windowStart + 60).allowed);
}

TEST(RateLimiter, RetryAfterIsTheTimeLeftInTheWindow) {
    FakeStore store;
    RateLimiter limiter(store, 1, 60);

    const std::int64_t windowStart = (kNow / 60) * 60;
    limiter.allow("ip", windowStart + 10);
    const RateLimiter::Decision decision = limiter.allow("ip", windowStart + 10);
    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.retryAfterSeconds, 50);
}

TEST(RateLimiter, UsesTheDocumentedKeyAndSetsTheTtl) {
    FakeStore store;
    RateLimiter limiter(store, 5, 60);
    limiter.allow("9.9.9.9", 600);   // window number 600 / 60 = 10

    EXPECT_EQ(RateLimiter::makeKey("9.9.9.9", 10), "ratelimit:9.9.9.9:10");
    ASSERT_EQ(store.counters.count("ratelimit:9.9.9.9:10"), 1u);
    EXPECT_EQ(store.counters["ratelimit:9.9.9.9:10"], 1);
    EXPECT_EQ(store.ttls["ratelimit:9.9.9.9:10"], 60) << "counter must expire with its window";
}

TEST(RateLimiter, FailsOpenWhenTheStoreIsUnavailable) {
    FakeStore store;
    store.available = false;
    RateLimiter limiter(store, 1, 60);

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.allow("ip", kNow).allowed);
    }
}

TEST(RateLimiter, ZeroRequestsDisablesLimiting) {
    FakeStore store;
    RateLimiter limiter(store, 0, 60);

    EXPECT_FALSE(limiter.enabled());
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.allow("ip", kNow).allowed);
    }
    EXPECT_TRUE(store.counters.empty()) << "a disabled limiter must not touch the store";
}

TEST(RateLimiter, WallClockOverloadWorks) {
    FakeStore store;
    RateLimiter limiter(store, 1, 3600);
    EXPECT_TRUE(limiter.allow("ip").allowed);
    // (Could only flake if an hour boundary falls between these two lines.)
    EXPECT_FALSE(limiter.allow("ip").allowed);
}

TEST(RateLimiter, ExactlyLimitRequestsPassUnderConcurrency) {
    FakeStore store;
    RateLimiter limiter(store, 100, 60);

    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                if (limiter.allow("ip", kNow).allowed) {
                    ++allowed;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    // 400 attempts, limit 100: the counter is atomic in the store, so exactly
    // 100 get through no matter how the threads interleave.
    EXPECT_EQ(allowed.load(), 100);
}
