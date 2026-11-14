// Integration tests: a real miniLB process in front of real demo_backend
// processes and (where needed) a real redis-server, all on free local ports.
//
// Every test starts its own set of processes and stops them afterwards. When
// miniLB is stopped, its exit code must be 0. Under ThreadSanitizer /
// AddressSanitizer a detected problem makes the process exit non-zero, so
// these tests double as the sanitizer run for the whole request path.
//
// Tests that need Redis are skipped if `redis-server` is not installed,
// unless MINILB_REQUIRE_REDIS=1 is set (CI sets it), in which case a missing
// Redis is a failure.

#include "TestSupport.hpp"

using namespace minilb_test;
using namespace std::chrono_literals;

namespace {

// =============================== Balancing ==================================

TEST_F(MiniLbTest, RoundRobinRotatesAcrossBackends) {
    startBackends(3);
    startLb();

    std::vector<std::string> bodies;
    std::map<std::string, int> perBackend;
    for (int i = 0; i < 9; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        EXPECT_EQ(reply.header("X-Cache"), "BYPASS") << "cache is disabled in this test";
        bodies.push_back(reply.body);
        ++perBackend[reply.header("X-Backend")];
    }

    // Strict rotation: 1, 2, 3, 1, 2, 3, ...
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(bodies[i], "Hello from backend-" + std::to_string(i % 3 + 1) + "!\n");
    }
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(perBackend[backendId(i)], 3);
    }
}

TEST_F(MiniLbTest, WeightedSendsProportionalTraffic) {
    startBackends(3);
    LbOptions options;
    options.algorithm = "weighted";
    options.weights = {1, 1, 2};
    startLb(options);

    std::map<std::string, int> perBackend;
    for (int i = 0; i < 16; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        ++perBackend[reply.header("X-Backend")];
    }
    EXPECT_EQ(perBackend[backendId(0)], 4);
    EXPECT_EQ(perBackend[backendId(1)], 4);
    EXPECT_EQ(perBackend[backendId(2)], 8);
}

TEST_F(MiniLbTest, LeastConnectionsAvoidsTheBusyBackend) {
    startBackends(2);
    LbOptions options;
    options.algorithm = "least_connections";
    startLb(options);

    // Occupy one backend with a slow (2 s) request...
    Reply slowReply;
    ScopedThread slow([&] { slowReply = httpGet(lbPort_, "/slow"); });
    ASSERT_TRUE(waitUntil([&] { return activeOn(0) + activeOn(1) == 1; })) << stats();
    const int busy = activeOn(0) == 1 ? 0 : 1;

    // ...then every fast request must go to the other one.
    for (int i = 0; i < 6; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        EXPECT_EQ(reply.header("X-Backend"), backendId(1 - busy));
    }

    slow.join();
    EXPECT_EQ(slowReply.status, 200);
    EXPECT_EQ(slowReply.header("X-Backend"), backendId(busy));
}

TEST_F(MiniLbTest, ForwardsPostBodies) {
    startBackends(1);
    startLb();

    const std::string body(50000, 'x');   // larger than one read buffer
    const Reply reply = httpPost(lbPort_, "/echo", body);
    EXPECT_EQ(reply.status, 200);
    EXPECT_EQ(reply.body, body);
}

TEST_F(MiniLbTest, PassesBackendStatusCodesThrough) {
    startBackends(1);
    startLb();

    const Reply reply = httpGet(lbPort_, "/does-not-exist");
    EXPECT_EQ(reply.status, 404);
    EXPECT_EQ(reply.header("X-Backend"), backendId(0));
}

TEST_F(MiniLbTest, HeadRequestGetsHeadersWithoutBody) {
    startBackends(1);
    startLb();

    const std::string raw = rawExchange(lbPort_, "HEAD / HTTP/1.1\r\nHost: test.local\r\n\r\n");
    EXPECT_EQ(raw.rfind("HTTP/1.1 200 OK\r\n", 0), 0u) << raw;
    EXPECT_NE(raw.find("Content-Length: 22\r\n"), std::string::npos) << raw;
    EXPECT_EQ(raw.substr(raw.size() - 4), "\r\n\r\n") << "no body after the headers";
}

// ================================= Cache ====================================

TEST_F(MiniLbTest, CacheMissThenHit) {
    REQUIRE_REDIS();
    startBackends(3);
    LbOptions options;
    options.cache = true;
    startLb(options);

    const Reply first = httpGet(lbPort_, "/");
    ASSERT_EQ(first.status, 200);
    EXPECT_EQ(first.header("X-Cache"), "MISS");
    EXPECT_EQ(first.header("X-Backend"), backendId(0));

    // The response is written to Redis in the background right after the
    // first reply, so give that a moment before asking again.
    std::this_thread::sleep_for(300ms);

    for (int i = 0; i < 2; ++i) {
        const Reply cached = httpGet(lbPort_, "/");
        EXPECT_EQ(cached.status, 200);
        EXPECT_EQ(cached.header("X-Cache"), "HIT");
        // Round robin would have picked backend-2 and backend-3 next, so an
        // identical body proves the answer came from the cache.
        EXPECT_EQ(cached.body, first.body);
        EXPECT_EQ(cached.header("X-Backend"), "") << "no backend was contacted";
    }

    // A different query string is a different cache entry.
    EXPECT_EQ(httpGet(lbPort_, "/?page=2").header("X-Cache"), "MISS");

    const std::string json = stats();
    EXPECT_EQ(statNumber(json, "cache_hits"), 2);
    EXPECT_EQ(statNumber(json, "cache_misses"), 2);
}

TEST_F(MiniLbTest, CacheMakesSlowEndpointFast) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.cache = true;
    startLb(options);

    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(httpGet(lbPort_, "/slow").status, 200);
    const auto firstDuration = std::chrono::steady_clock::now() - start;
    EXPECT_GE(firstDuration, 1900ms) << "the backend sleeps 2 s";

    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/slow").header("X-Cache") == "HIT"; }));

    const auto again = std::chrono::steady_clock::now();
    const Reply cached = httpGet(lbPort_, "/slow");
    const auto cachedDuration = std::chrono::steady_clock::now() - again;
    EXPECT_EQ(cached.header("X-Cache"), "HIT");
    EXPECT_LT(cachedDuration, 1000ms);
}

TEST_F(MiniLbTest, CacheEntriesExpireAfterTheTtl) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.cache = true;
    options.cacheTtlSeconds = 1;
    startLb(options);

    EXPECT_EQ(httpGet(lbPort_, "/").header("X-Cache"), "MISS");
    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/").header("X-Cache") == "HIT"; }, 900ms));

    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(httpGet(lbPort_, "/").header("X-Cache"), "MISS");
}

TEST_F(MiniLbTest, OnlyCacheableRequestsUseTheCache) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.cache = true;
    startLb(options);

    // POST is never cached.
    EXPECT_EQ(httpPost(lbPort_, "/echo", "a").header("X-Cache"), "BYPASS");
    EXPECT_EQ(httpPost(lbPort_, "/echo", "b").body, "b");

    // Requests with credentials are never cached.
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(httpGet(lbPort_, "/", "Authorization: Bearer secret\r\n").header("X-Cache"),
                  "BYPASS");
    }

    // Error responses are not stored: a 404 stays a MISS.
    for (int i = 0; i < 3; ++i) {
        const Reply reply = httpGet(lbPort_, "/missing");
        EXPECT_EQ(reply.status, 404);
        EXPECT_EQ(reply.header("X-Cache"), "MISS");
        std::this_thread::sleep_for(50ms);
    }
}

// ============================== Rate limiting ===============================

TEST_F(MiniLbTest, RateLimitReturns429AfterTheLimit) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.rateLimit = 5;
    startLb(options);

    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(httpGet(lbPort_, "/").status, 200) << "request " << i + 1;
    }
    for (int i = 0; i < 3; ++i) {
        const Reply limited = httpGet(lbPort_, "/");
        EXPECT_EQ(limited.status, 429);
        EXPECT_FALSE(limited.header("Retry-After").empty());
    }

    // /stats is exempt from rate limiting and reports the rejections.
    const Reply statsReply = httpGet(lbPort_, "/stats");
    EXPECT_EQ(statsReply.status, 200);
    EXPECT_EQ(statNumber(statsReply.body, "rate_limited"), 3);
    EXPECT_EQ(statNumber(statsReply.body, "total_requests"), 8);
}

// ========================= Health checks and failover =======================

TEST_F(MiniLbTest, FailoverWhenABackendDiesAndRecovers) {
    startBackends(3);
    LbOptions options;
    options.failThreshold = 2;   // probes every 100 ms: 2 failures -> unhealthy,
                                 // 2 successes -> healthy again
    startLb(options);
    ASSERT_TRUE(reportedHealthy(1));

    // Crash backend 2.
    backends_[1]->kill();

    // Clients must not notice: a request that hits the dead backend is
    // retried on another one.
    for (int i = 0; i < 12; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200) << "request " << i;
        EXPECT_NE(reply.header("X-Backend"), backendId(1));
    }

    // It is taken out of rotation...
    ASSERT_TRUE(waitUntil([&] { return !reportedHealthy(1); })) << stats();
    EXPECT_TRUE(reportedHealthy(0));
    EXPECT_TRUE(reportedHealthy(2));

    // ...and comes back once it answers health checks again.
    startBackend(1);
    ASSERT_TRUE(waitUntil([&] { return reportedHealthy(1); })) << stats();

    std::set<std::string> seen;
    for (int i = 0; i < 9; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        seen.insert(reply.header("X-Backend"));
    }
    EXPECT_EQ(seen.count(backendId(1)), 1u) << "recovered backend receives traffic again";
    EXPECT_EQ(seen.size(), 3u);
}

TEST_F(MiniLbTest, Returns503WhenNoBackendIsHealthy) {
    addBackendPort(freePort());   // nothing listens there
    LbOptions options;
    options.failThreshold = 2;
    startLb(options);

    // Until the health checker has seen enough failures the backend still
    // counts as healthy and requests fail with 502; afterwards it is 503.
    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/").status == 503; }));
    EXPECT_FALSE(reportedHealthy(0));
    EXPECT_GE(statNumber(stats(), "errors"), 1);
}

TEST_F(MiniLbTest, Returns502WhenTheOnlyBackendIsUnreachable) {
    addBackendPort(freePort());   // nothing listens there
    LbOptions options;
    options.healthIntervalMs = 60000;   // keep the health checker out of the picture
    options.failThreshold = 100;
    startLb(options);

    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);
}

TEST_F(MiniLbTest, Returns502OnInvalidBackendResponse) {
    GarbageServer garbage;
    addBackendPort(garbage.port());
    LbOptions options;
    options.healthIntervalMs = 60000;
    options.failThreshold = 100;
    startLb(options);

    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);
}

TEST_F(MiniLbTest, Returns504WhenTheBackendIsTooSlow) {
    startBackends(1);
    LbOptions options;
    options.readTimeoutMs = 300;   // /slow takes 2 s
    options.failThreshold = 100;
    startLb(options);

    const auto start = std::chrono::steady_clock::now();
    const Reply reply = httpGet(lbPort_, "/slow");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(reply.status, 504);
    EXPECT_GE(elapsed, 300ms);
    EXPECT_LT(elapsed, 1900ms) << "the timeout must fire long before the backend answers";

    // Fast requests still work afterwards.
    EXPECT_EQ(httpGet(lbPort_, "/").status, 200);
}

TEST_F(MiniLbTest, RetriesOnceOnAnotherBackend) {
    startBackends(1);
    addBackendPort(freePort());   // second backend is dead from the start
    LbOptions options;
    options.healthIntervalMs = 60000;   // only the retry logic may save us
    options.failThreshold = 100;
    startLb(options);

    // Round robin alternates between the live and the dead backend; every
    // request must succeed anyway.
    for (int i = 0; i < 6; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200) << "request " << i;
        EXPECT_EQ(reply.header("X-Backend"), backendId(0));
    }
}

TEST_F(MiniLbTest, HealthProbeDistinguishesGoodAndBadBackends) {
    startBackends(1);
    using minilb::HealthChecker;

    EXPECT_TRUE(HealthChecker::probe("127.0.0.1", static_cast<unsigned short>(backendPorts_[0]),
                                     "/health", 2s));
    EXPECT_TRUE(HealthChecker::probe("localhost", static_cast<unsigned short>(backendPorts_[0]),
                                     "/health", 2s))
        << "host names are resolved";
    EXPECT_FALSE(HealthChecker::probe("127.0.0.1", static_cast<unsigned short>(backendPorts_[0]),
                                      "/does-not-exist", 2s))
        << "a 404 is not healthy";
    EXPECT_FALSE(
        HealthChecker::probe("127.0.0.1", static_cast<unsigned short>(freePort()), "/health", 500ms))
        << "connection refused is not healthy";

    GarbageServer garbage;
    EXPECT_FALSE(HealthChecker::probe("127.0.0.1", static_cast<unsigned short>(garbage.port()),
                                      "/health", 500ms));
}

// ============================== Bad requests ================================

TEST_F(MiniLbTest, MalformedRequestsGet400) {
    startBackends(1);
    startLb();

    EXPECT_EQ(parseReply(rawExchange(lbPort_, "THIS IS NOT HTTP\r\n\r\n")).status, 400);
    EXPECT_EQ(parseReply(rawExchange(lbPort_, "GET / HTTP/9.9\r\n\r\n")).status, 400);
    EXPECT_EQ(parseReply(rawExchange(lbPort_, "GET / HTTP/1.1\r\nBroken Header\r\n\r\n")).status, 400);

    // The server is still fine afterwards and counted the errors.
    EXPECT_EQ(httpGet(lbPort_, "/").status, 200);
    EXPECT_EQ(statNumber(stats(), "errors"), 3);
}

TEST_F(MiniLbTest, OversizedRequestsAreRejected) {
    startBackends(1);
    startLb();

    // Declared body larger than the 1 MiB cap: refused from the headers alone.
    EXPECT_EQ(parseReply(rawExchange(lbPort_,
                                     "POST /echo HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n"))
                  .status,
              413);

    // Headers larger than the 16 KiB cap.
    const std::string hugeHeader =
        "GET / HTTP/1.1\r\nX-Padding: " + std::string(17000, 'a') + "\r\n\r\n";
    EXPECT_EQ(parseReply(rawExchange(lbPort_, hugeHeader)).status, 431);

    // Chunked request bodies are not implemented.
    EXPECT_EQ(parseReply(rawExchange(lbPort_,
                                     "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"))
                  .status,
              501);
}

// ============================ Redis unavailable =============================

TEST_F(MiniLbTest, FailsOpenWhenRedisIsDown) {
    startBackends(2);
    LbOptions options;
    options.cache = true;
    options.rateLimit = 3;   // would reject the 4th request if Redis were up
    startLb(options);        // note: no startRedis()

    for (int i = 0; i < 10; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200) << "request " << i;
        EXPECT_EQ(reply.header("X-Cache"), "MISS");
    }

    const std::string json = stats();
    EXPECT_NE(json.find("\"redis_available\": false"), std::string::npos) << json;
    EXPECT_EQ(statNumber(json, "rate_limited"), 0);
    EXPECT_EQ(statNumber(json, "cache_hits"), 0);
}

TEST_F(MiniLbTest, SurvivesRedisGoingAwayAndComingBack) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.cache = true;
    startLb(options);

    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/").header("X-Cache") == "HIT"; }));

    redis_.kill();
    for (int i = 0; i < 5; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        EXPECT_EQ(reply.header("X-Cache"), "MISS");
    }

    // A fresh (empty) Redis on the same port: caching resumes by itself.
    ASSERT_TRUE(startRedis());
    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/").header("X-Cache") == "HIT"; }));
}

// ================================ /stats ====================================

TEST_F(MiniLbTest, StatsReportsCountersAndBackends) {
    startBackends(2);
    startLb();

    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(httpGet(lbPort_, "/").status, 200);
    }

    const Reply reply = httpGet(lbPort_, "/stats");
    ASSERT_EQ(reply.status, 200);
    EXPECT_EQ(reply.header("Content-Type"), "application/json");

    const std::string& json = reply.body;
    EXPECT_EQ(statNumber(json, "total_requests"), 4) << "/stats itself is not counted";
    EXPECT_GE(statNumber(json, "active_connections"), 1) << "at least the /stats request itself";
    EXPECT_EQ(statNumber(json, "errors"), 0);
    EXPECT_NE(json.find("\"algorithm\": \"round_robin\""), std::string::npos);
    for (int i = 0; i < 2; ++i) {
        EXPECT_NE(json.find("{\"id\": \"" + backendId(i) +
                            "\", \"healthy\": true, \"weight\": 1, \"active_connections\": 0, "
                            "\"requests\": 2}"),
                  std::string::npos)
            << json;
    }
}

// ========================= Concurrency and shutdown =========================

TEST_F(MiniLbTest, HandlesManyConcurrentClients) {
    REQUIRE_REDIS();
    startBackends(3);
    LbOptions options;
    options.cache = true;         // exercises the Redis thread hand-off too
    options.rateLimit = 1000000;
    startLb(options);

    constexpr int kThreads = 8;
    constexpr int kRequestsPerThread = 40;
    std::atomic<int> ok{0};
    std::atomic<int> wrongBody{0};

    std::vector<std::thread> clients;
    for (int t = 0; t < kThreads; ++t) {
        clients.emplace_back([&, t] {
            for (int i = 0; i < kRequestsPerThread; ++i) {
                if (i % 2 == 0) {
                    // 10 distinct URLs shared by all threads: a mix of hits and misses.
                    const Reply reply = httpGet(lbPort_, "/?item=" + std::to_string(i % 20));
                    if (reply.status == 200) {
                        ++ok;
                    }
                } else {
                    const std::string body = "client-" + std::to_string(t) + "-" + std::to_string(i);
                    const Reply reply = httpPost(lbPort_, "/echo", body);
                    if (reply.status == 200) {
                        ++ok;
                    }
                    if (reply.body != body) {
                        ++wrongBody;   // responses must never be mixed up between clients
                    }
                }
            }
        });
    }
    for (auto& client : clients) {
        client.join();
    }

    EXPECT_EQ(ok.load(), kThreads * kRequestsPerThread);
    EXPECT_EQ(wrongBody.load(), 0);

    const std::string json = stats();
    EXPECT_EQ(statNumber(json, "total_requests"), kThreads * kRequestsPerThread);
    EXPECT_EQ(statNumber(json, "cache_hits") + statNumber(json, "cache_misses"),
              kThreads * kRequestsPerThread / 2)
        << "every GET is either a hit or a miss";
    EXPECT_EQ(statNumber(json, "errors"), 0);
}

TEST_F(MiniLbTest, GracefulShutdownLetsInFlightRequestsFinish) {
    startBackends(1);
    startLb();

    Reply slowReply;
    ScopedThread slow([&] { slowReply = httpGet(lbPort_, "/slow"); });
    ASSERT_TRUE(waitUntil([&] { return activeOn(0) == 1; })) << stats();

    // SIGTERM while the request is in flight.
    const auto start = std::chrono::steady_clock::now();
    const int exitCode = lb_.terminate();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    slow.join();

    EXPECT_EQ(exitCode, 0);
    EXPECT_EQ(slowReply.status, 200) << "the in-flight request was allowed to finish";
    EXPECT_EQ(slowReply.body, "Slow hello from backend-1!\n");
    EXPECT_GE(elapsed, 1000ms) << "shutdown waited for the request";

    // New connections are refused once shutdown has begun.
    EXPECT_FALSE(canConnect(lbPort_));
}

} // namespace
