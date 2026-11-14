// Pairwise (all-pairs) combinatorial system tests.
//
// Technique: combinatorial testing. Six settings decide what happens to a
// request (see software-testing/tools/pairwise.py for the list). All
// combinations would be 216 system tests; the generated table in
// PairwiseCases.inc has 12 rows in which every pair of values of every two
// settings still occurs together at least once.
//
// Adequacy criterion: 100% pair coverage. It is checked twice, by the
// generator and independently by the PairwiseTable test below, so the table
// cannot silently go stale when a setting is added.
//
// Oracle: each row sends five requests and compares every reply with what
// the requirements say should happen for that combination (see `expected`).
// The oracle is written from the README, not from the code:
//
//   1. rate limit on and Redis up  -> requests 4 and 5 get 429 (limit is 3);
//      Redis down                  -> fail-open, nobody is limited
//   2. no reachable backend        -> 502
//   3. otherwise 200 from a backend (one dead backend is hidden by the retry)
//   4. X-Cache is HIT only for a GET with the cache on and Redis up

#include "TestSupport.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace minilb_test;
using namespace std::chrono_literals;

namespace {

struct Combination {
    const char* algorithm;
    const char* cache;
    const char* redis;
    const char* rateLimit;
    const char* method;
    const char* backends;

    std::array<std::string, 6> values() const {
        return {algorithm, cache, redis, rateLimit, method, backends};
    }
};

void PrintTo(const Combination& c, std::ostream* out) {
    *out << c.algorithm << " cache=" << c.cache << " redis=" << c.redis << " rate_limit=" << c.rateLimit
         << " " << c.method << " backends=" << c.backends;
}

const Combination kCombinations[] = {
#include "PairwiseCases.inc"
};

// The values each setting can take (must match tools/pairwise.py).
const std::vector<std::vector<std::string>> kDomains = {
    {"round_robin", "least_connections", "weighted"},
    {"off", "on"},
    {"down", "up"},
    {"off", "on"},
    {"GET", "HEAD", "POST"},
    {"all_up", "one_down", "all_down"},
};

// ---- The adequacy criterion, checked by a test -------------------------------

TEST(PairwiseTable, CoversEveryPairOfValues) {
    // (setting i, value, setting j, value) for every pair that occurs in a row.
    std::set<std::tuple<std::size_t, std::string, std::size_t, std::string>> covered;
    for (const Combination& combination : kCombinations) {
        const auto row = combination.values();
        for (std::size_t i = 0; i < row.size(); ++i) {
            for (std::size_t j = i + 1; j < row.size(); ++j) {
                covered.insert({i, row[i], j, row[j]});
            }
        }
    }

    std::size_t wanted = 0;
    for (std::size_t i = 0; i < kDomains.size(); ++i) {
        for (std::size_t j = i + 1; j < kDomains.size(); ++j) {
            for (const std::string& a : kDomains[i]) {
                for (const std::string& b : kDomains[j]) {
                    ++wanted;
                    EXPECT_TRUE(covered.count({i, a, j, b}) == 1)
                        << "pair never tested together: " << a << " + " << b;
                }
            }
        }
    }
    EXPECT_EQ(covered.size(), wanted) << "a row uses a value that is not in kDomains";
    EXPECT_LT(std::size(kCombinations), 216u / 10) << "far fewer tests than all combinations";
}

TEST(PairwiseTable, EveryValueInTheTableIsAKnownValue) {
    for (const Combination& combination : kCombinations) {
        const auto row = combination.values();
        for (std::size_t i = 0; i < row.size(); ++i) {
            EXPECT_EQ(std::count(kDomains[i].begin(), kDomains[i].end(), row[i]), 1) << row[i];
        }
    }
}

// ---- The system test, one instance per row -----------------------------------

class PairwiseConfig : public MiniLbTest, public ::testing::WithParamInterface<Combination> {
protected:
    static constexpr int kRateLimit = 3;
    static constexpr int kRequests = 5;

    // Sends one request with the method under test.
    Reply send(const std::string& method) {
        if (method == "POST") {
            return httpPost(lbPort_, "/echo", "payload");
        }
        if (method == "HEAD") {
            return parseReply(rawExchange(lbPort_, "HEAD / HTTP/1.1\r\nHost: test.local\r\n\r\n"),
                              /*headRequest=*/true);
        }
        return httpGet(lbPort_, "/");
    }
};

TEST_P(PairwiseConfig, EveryReplyMatchesTheRequirements) {
    const Combination c = GetParam();
    const std::string method = c.method;
    const bool cacheOn = std::string(c.cache) == "on";
    const bool redisUp = std::string(c.redis) == "up";
    const bool limitOn = std::string(c.rateLimit) == "on";
    const std::string backends = c.backends;

    // ---- Arrange: build exactly this combination ---------------------------
    if (redisUp) {
        REQUIRE_REDIS();
    }
    if (backends == "all_up") {
        startBackends(2);
    } else if (backends == "one_down") {
        startBackends(1);
        addBackendPort(freePort());   // nothing listens there
    } else {
        addBackendPort(freePort());
        addBackendPort(freePort());
    }
    LbOptions options;
    options.algorithm = c.algorithm;
    options.weights = {1, 2};
    options.cache = cacheOn;
    options.rateLimit = limitOn ? kRateLimit : 0;
    options.healthIntervalMs = 60000;   // dead backends stay in rotation: the
    options.failThreshold = 100;        // request path has to cope by itself
    startLb(options);

    // ---- Act and assert: five requests against the oracle -------------------
    const bool hitPossible = cacheOn && redisUp && method == "GET";
    int expected200 = 0;
    int expected429 = 0;
    int expected502 = 0;
    int hits = 0;

    for (int i = 1; i <= kRequests; ++i) {
        SCOPED_TRACE("request " + std::to_string(i));
        const Reply reply = send(method);

        if (limitOn && redisUp && i > kRateLimit) {
            ++expected429;
            EXPECT_EQ(reply.status, 429);
            EXPECT_FALSE(reply.header("Retry-After").empty());
            continue;
        }
        if (backends == "all_down") {
            ++expected502;
            EXPECT_EQ(reply.status, 502);
            EXPECT_NE(reply.header("X-Cache"), "HIT");
            continue;
        }

        ++expected200;
        ASSERT_EQ(reply.status, 200);
        if (method == "POST") {
            EXPECT_EQ(reply.body, "payload");
        } else if (method == "HEAD") {
            EXPECT_EQ(reply.body, "") << "a reply to HEAD has no body";
            EXPECT_EQ(reply.header("Content-Length"), "22") << "but announces the GET body's length";
        } else {
            EXPECT_EQ(reply.body.rfind("Hello from backend-", 0), 0u) << reply.body;
        }

        const std::string cacheHeader = reply.header("X-Cache");
        if (!cacheOn || method != "GET") {
            EXPECT_EQ(cacheHeader, "BYPASS");
        } else if (!redisUp) {
            EXPECT_NE(cacheHeader, "HIT") << "nothing can be served from a cache that is down";
        } else if (i == 1) {
            EXPECT_EQ(cacheHeader, "MISS") << "the first request cannot be in the cache yet";
            // The entry is written in the background right after the reply.
            std::this_thread::sleep_for(300ms);
        } else {
            EXPECT_EQ(cacheHeader, "HIT");
        }

        if (cacheHeader == "HIT") {
            ++hits;
            EXPECT_EQ(reply.header("X-Backend"), "") << "a cache hit contacts no backend";
        } else {
            const std::string servedBy = reply.header("X-Backend");
            if (backends == "one_down") {
                EXPECT_EQ(servedBy, backendId(0)) << "only backend 0 is alive";
            } else {
                EXPECT_TRUE(servedBy == backendId(0) || servedBy == backendId(1)) << servedBy;
            }
        }
    }

    // ---- The counters in /stats tell the same story --------------------------
    const std::string json = stats();
    EXPECT_EQ(statNumber(json, "total_requests"), kRequests) << json;
    EXPECT_EQ(statNumber(json, "rate_limited"), expected429) << json;
    EXPECT_EQ(statNumber(json, "cache_hits"), hits) << json;
    if (!hitPossible) {
        EXPECT_EQ(hits, 0);
    }
    EXPECT_EQ(expected200 + expected429 + expected502, kRequests);
}

INSTANTIATE_TEST_SUITE_P(AllPairs, PairwiseConfig, ::testing::ValuesIn(kCombinations));

} // namespace
