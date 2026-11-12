// Unit tests for Backend, BackendPool and the three load-balancing strategies.

#include "miniLB/balancer/BackendPool.hpp"
#include "miniLB/balancer/LeastConnections.hpp"
#include "miniLB/balancer/RoundRobin.hpp"
#include "miniLB/balancer/WeightedRoundRobin.hpp"

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace minilb;

namespace {

// Three backends a, b, c with the given weights.
struct Backends {
    explicit Backends(int weightA = 1, int weightB = 1, int weightC = 1)
        : a("a", 1, weightA), b("b", 2, weightB), c("c", 3, weightC) {}

    std::vector<Backend*> all() { return {&a, &b, &c}; }

    Backend a, b, c;
    HttpRequest request;   // strategies currently ignore the request
};

// Calls select() `times` times and counts how often each backend was chosen.
std::map<std::string, int> tally(Strategy& strategy, const std::vector<Backend*>& healthy,
                                 int times) {
    HttpRequest request;
    std::map<std::string, int> counts;
    for (int i = 0; i < times; ++i) {
        ++counts[strategy.select(healthy, request)->host];
    }
    return counts;
}

} // namespace

// ---------------------------------- Backend --------------------------------

TEST(Backend, HasIdAndStartsHealthy) {
    Backend backend("10.0.0.1", 8001, 3);
    EXPECT_EQ(backend.id, "10.0.0.1:8001");
    EXPECT_EQ(backend.weight, 3);
    EXPECT_TRUE(backend.healthy.load());
    EXPECT_EQ(backend.activeConnections.load(), 0);
}

TEST(Backend, WeightBelowOneIsClampedToOne) {
    EXPECT_EQ(Backend("h", 1, 0).weight, 1);
    EXPECT_EQ(Backend("h", 1, -4).weight, 1);
}

TEST(Backend, BecomesUnhealthyAfterConsecutiveFailures) {
    Backend backend("h", 1);
    EXPECT_FALSE(backend.recordFailure(3));
    EXPECT_FALSE(backend.recordFailure(3));
    EXPECT_TRUE(backend.healthy.load());

    EXPECT_TRUE(backend.recordFailure(3)) << "third failure flips the state";
    EXPECT_FALSE(backend.healthy.load());

    EXPECT_FALSE(backend.recordFailure(3)) << "already unhealthy: no new transition";
}

TEST(Backend, ASuccessResetsTheFailureStreak) {
    Backend backend("h", 1);
    backend.recordFailure(3);
    backend.recordFailure(3);
    backend.recordSuccess(2);   // streak broken
    EXPECT_FALSE(backend.recordFailure(3));
    EXPECT_FALSE(backend.recordFailure(3));
    EXPECT_TRUE(backend.healthy.load());
}

TEST(Backend, RecoversAfterConsecutiveSuccesses) {
    Backend backend("h", 1);
    backend.recordFailure(1);
    ASSERT_FALSE(backend.healthy.load());

    EXPECT_FALSE(backend.recordSuccess(2));
    EXPECT_FALSE(backend.healthy.load());
    EXPECT_TRUE(backend.recordSuccess(2)) << "second success flips the state";
    EXPECT_TRUE(backend.healthy.load());
}

TEST(Backend, AFailureResetsTheSuccessStreak) {
    Backend backend("h", 1);
    backend.recordFailure(1);
    backend.recordSuccess(2);
    backend.recordFailure(1);   // streak broken
    EXPECT_FALSE(backend.recordSuccess(2));
    EXPECT_FALSE(backend.healthy.load());
}

TEST(BackendLease, CountsActiveConnectionsForItsLifetime) {
    Backend backend("h", 1);
    {
        BackendLease first(backend);
        BackendLease second(backend);
        EXPECT_EQ(backend.activeConnections.load(), 2);
    }
    EXPECT_EQ(backend.activeConnections.load(), 0);
}

TEST(BackendPool, HealthyReturnsOnlyHealthyBackends) {
    BackendPool pool;
    pool.add("a", 1);
    pool.add("b", 2);
    pool.add("c", 3, 5);
    ASSERT_EQ(pool.all().size(), 3u);
    EXPECT_EQ(pool.healthy().size(), 3u);

    pool.all()[1]->healthy = false;
    const auto healthy = pool.healthy();
    ASSERT_EQ(healthy.size(), 2u);
    EXPECT_EQ(healthy[0]->host, "a");
    EXPECT_EQ(healthy[1]->host, "c");
    EXPECT_EQ(healthy[1]->weight, 5);

    for (const auto& backend : pool.all()) {
        backend->healthy = false;
    }
    EXPECT_TRUE(pool.healthy().empty());
}

// -------------------------------- RoundRobin -------------------------------

TEST(RoundRobin, RotatesInOrder) {
    Backends backends;
    RoundRobin strategy;
    const auto healthy = backends.all();

    std::vector<std::string> picks;
    for (int i = 0; i < 6; ++i) {
        picks.push_back(strategy.select(healthy, backends.request)->host);
    }
    EXPECT_EQ(picks, (std::vector<std::string>{"a", "b", "c", "a", "b", "c"}));
}

TEST(RoundRobin, ReturnsNullWhenNothingIsHealthy) {
    RoundRobin strategy;
    EXPECT_EQ(strategy.select({}, HttpRequest{}), nullptr);
}

TEST(RoundRobin, SkipsBackendsThatAreNotInTheHealthyList) {
    Backends backends;
    RoundRobin strategy;
    const auto counts = tally(strategy, {&backends.a, &backends.c}, 10);
    EXPECT_EQ(counts.at("a"), 5);
    EXPECT_EQ(counts.at("c"), 5);
    EXPECT_EQ(counts.count("b"), 0u);
}

TEST(RoundRobin, IsExactlyFairUnderConcurrency) {
    Backends backends;
    RoundRobin strategy;
    const auto healthy = backends.all();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 3000;   // 24000 picks in total, divisible by 3
    std::vector<std::map<Backend*, int>> perThread(kThreads);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            HttpRequest request;
            for (int i = 0; i < kPerThread; ++i) {
                ++perThread[t][strategy.select(healthy, request)];
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::map<Backend*, int> total;
    for (const auto& counts : perThread) {
        for (const auto& [backend, count] : counts) {
            total[backend] += count;
        }
    }
    // The atomic counter hands out every number exactly once, so the split
    // is exact even though the threads interleave arbitrarily.
    EXPECT_EQ(total[&backends.a], kThreads * kPerThread / 3);
    EXPECT_EQ(total[&backends.b], kThreads * kPerThread / 3);
    EXPECT_EQ(total[&backends.c], kThreads * kPerThread / 3);
}

// ----------------------------- LeastConnections ----------------------------

TEST(LeastConnections, PicksTheBackendWithFewestActiveConnections) {
    Backends backends;
    backends.a.activeConnections = 5;
    backends.b.activeConnections = 1;
    backends.c.activeConnections = 3;

    LeastConnections strategy;
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(strategy.select(backends.all(), backends.request), &backends.b);
    }
}

TEST(LeastConnections, SpreadsTiesAcrossBackends) {
    Backends backends;   // all idle
    LeastConnections strategy;
    const auto counts = tally(strategy, backends.all(), 9);
    EXPECT_EQ(counts.at("a"), 3);
    EXPECT_EQ(counts.at("b"), 3);
    EXPECT_EQ(counts.at("c"), 3);
}

TEST(LeastConnections, FollowsTheLoadAsItChanges) {
    Backends backends;
    LeastConnections strategy;
    const auto healthy = backends.all();

    // Simulate requests that never finish: every pick adds load to its backend,
    // so the load must stay balanced.
    for (int i = 0; i < 30; ++i) {
        ++strategy.select(healthy, backends.request)->activeConnections;
    }
    EXPECT_EQ(backends.a.activeConnections.load(), 10);
    EXPECT_EQ(backends.b.activeConnections.load(), 10);
    EXPECT_EQ(backends.c.activeConnections.load(), 10);
}

TEST(LeastConnections, ReturnsNullWhenNothingIsHealthy) {
    LeastConnections strategy;
    EXPECT_EQ(strategy.select({}, HttpRequest{}), nullptr);
}

// ---------------------------- WeightedRoundRobin ---------------------------

TEST(WeightedRoundRobin, DistributesProportionallyToWeight) {
    Backends backends(1, 1, 2);
    WeightedRoundRobin strategy;
    const auto counts = tally(strategy, backends.all(), 400);
    EXPECT_EQ(counts.at("a"), 100);
    EXPECT_EQ(counts.at("b"), 100);
    EXPECT_EQ(counts.at("c"), 200);
}

TEST(WeightedRoundRobin, OneCycleFollowsTheTickets) {
    Backends backends(1, 1, 2);
    WeightedRoundRobin strategy;
    const auto healthy = backends.all();

    std::vector<std::string> picks;
    for (int i = 0; i < 4; ++i) {
        picks.push_back(strategy.select(healthy, backends.request)->host);
    }
    EXPECT_EQ(picks, (std::vector<std::string>{"a", "b", "c", "c"}));
}

TEST(WeightedRoundRobin, AdaptsWhenABackendDrops) {
    Backends backends(1, 1, 2);
    WeightedRoundRobin strategy;
    const auto counts = tally(strategy, {&backends.a, &backends.c}, 300);
    EXPECT_EQ(counts.at("a"), 100);
    EXPECT_EQ(counts.at("c"), 200);
}

TEST(WeightedRoundRobin, ReturnsNullWhenNothingIsHealthy) {
    WeightedRoundRobin strategy;
    EXPECT_EQ(strategy.select({}, HttpRequest{}), nullptr);
}

// --------------------------------- Factory ---------------------------------

TEST(MakeStrategy, CreatesEachAlgorithmByName) {
    EXPECT_STREQ(makeStrategy("round_robin")->name(), "round_robin");
    EXPECT_STREQ(makeStrategy("least_connections")->name(), "least_connections");
    EXPECT_STREQ(makeStrategy("weighted")->name(), "weighted");
}

TEST(MakeStrategy, ThrowsOnUnknownName) {
    EXPECT_THROW(makeStrategy("random"), std::invalid_argument);
}
