// Coverage-gap integration tests.
//
// Technique: structural (line and branch) coverage used as a *gap finder*.
// The original suite was measured with gcovr (software-testing/scripts/
// coverage.sh): 94.3% line, 86.3% branch. Every test here targets code that
// suite never executed; the comment on each test names the file and the
// behaviour it was written for.

#include "TestSupport.hpp"

using namespace minilb_test;
using namespace std::chrono_literals;

namespace {

// ---- Backends given by host name (Connection.cpp, connectToBackend) ---------
//
// Every other test configures backends as "127.0.0.1", which takes the
// "already an IP address" fast path. docker-compose uses host names
// (backend1, backend2, ...), so the resolver path is what runs in the real
// deployment, and no test executed it.

TEST_F(MiniLbTest, ProxiesToABackendGivenByHostName) {
    startBackends(2);
    LbOptions options;
    options.backendHost = "localhost";
    startLb(options);

    std::set<std::string> seen;
    for (int i = 0; i < 4; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200) << "request " << i;
        seen.insert(reply.header("X-Backend"));
    }
    // The backend id keeps the configured name, and both were used.
    EXPECT_EQ(seen, (std::set<std::string>{"localhost:" + std::to_string(backendPorts_[0]),
                                           "localhost:" + std::to_string(backendPorts_[1])}));
}

TEST_F(MiniLbTest, Returns502WhenTheBackendNameDoesNotResolve) {
    addBackendPort(freePort());
    LbOptions options;
    options.backendHost = kUnresolvableHost;
    options.healthIntervalMs = 60000;
    startLb(options);

    const Reply reply = httpGet(lbPort_, "/");
    EXPECT_EQ(reply.status, 502);
    EXPECT_NE(lbLog().find("failed during 'connect'"), std::string::npos)
        << "a failed lookup is reported as a connect failure";

    // miniLB itself is unharmed.
    EXPECT_EQ(httpGet(lbPort_, "/stats").status, 200);
    EXPECT_EQ(statNumber(stats(), "errors"), 1);
}

TEST_F(MiniLbTest, FailsOverFromAnUnresolvableNameToAWorkingBackend) {
    // Two entries with the same (bad) host would both fail, so this needs one
    // good and one bad backend: write the config by hand.
    startBackends(1);
    const std::string config = writeFile(
        "mixed.yaml",
        "listen_port: " + std::to_string(lbPort_) + "\n" +
            "backends:\n"
            "  - { host: " + kUnresolvableHost + ", port: 9 }\n"
            "  - { host: 127.0.0.1, port: " + std::to_string(backendPorts_[0]) + " }\n"
            "health_check: { interval_ms: 60000, fail_threshold: 100 }\n"
            "rate_limit: { requests: 0 }\n"
            "cache: { enabled: false }\n"
            "redis: { host: 127.0.0.1, port: " + std::to_string(redisPort_) + ", timeout_ms: 500 }\n");
    ASSERT_TRUE(lb_.start({MINILB_BIN, "--config", config}, dir_ + "/lb.log"));
    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/stats").status == 200; }, 30s));

    // Round robin offers the bad backend every second request; the retry
    // must hide that from the client.
    for (int i = 0; i < 4; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200) << "request " << i;
        EXPECT_EQ(reply.header("X-Backend"), backendId(0));
    }
}

// ---- "Retry once" really means once (Connection.cpp, canRetry) --------------
//
// RetriesOnceOnAnotherBackend only shows that a retry happens. The branch
// that *refuses* a second retry never ran: it needs two backends to fail for
// the same request.

TEST_F(MiniLbTest, GivesUpAfterOneRetry) {
    for (int i = 0; i < 3; ++i) {
        addBackendPort(freePort());   // three dead backends
    }
    LbOptions options;
    options.healthIntervalMs = 60000;   // keep all three "healthy"
    options.failThreshold = 100;
    startLb(options);

    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);

    // First attempt + exactly one retry = two backends tried, the third
    // one was never contacted.
    EXPECT_EQ(requestsOn(0) + requestsOn(1) + requestsOn(2), 2) << stats();
    EXPECT_EQ(statNumber(stats(), "errors"), 1);
}

// ---- Responses delimited by "connection close" (Connection.cpp) -------------
//
// The demo backend always sends Content-Length, so miniLB's handling of
// "the backend closed the connection: is the response whole?" was only unit
// tested in the parser, never through a real socket.

TEST_F(MiniLbTest, RelaysAResponseThatEndsWhenTheBackendCloses) {
    CannedServer backend("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nstreamed until close");
    addBackendPort(backend.port());
    LbOptions options;
    options.healthIntervalMs = 60000;
    startLb(options);

    const Reply reply = httpGet(lbPort_, "/");
    EXPECT_EQ(reply.status, 200);
    EXPECT_EQ(reply.body, "streamed until close");
    EXPECT_EQ(reply.header("X-Backend"), backendId(0));
}

TEST_F(MiniLbTest, Returns502WhenTheBackendClosesInTheMiddleOfTheBody) {
    // Promises 100 bytes, sends 5, closes.
    CannedServer backend("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
    addBackendPort(backend.port());
    LbOptions options;
    options.healthIntervalMs = 60000;
    startLb(options);

    const Reply reply = httpGet(lbPort_, "/");
    EXPECT_EQ(reply.status, 502) << "a truncated response must not reach the client";
    EXPECT_EQ(reply.body.find("short"), std::string::npos);
}

TEST_F(MiniLbTest, Returns502WhenTheBackendClosesWithoutAnswering) {
    CannedServer backend("");   // reads the request, says nothing, closes
    addBackendPort(backend.port());
    LbOptions options;
    options.healthIntervalMs = 60000;
    startLb(options);

    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);
}

// ---- Shutdown when a request outlives the grace period (Server.cpp) ---------
//
// GracefulShutdownLetsInFlightRequestsFinish covers "the request finishes in
// time". The other half of the requirement ("...but miniLB does not wait
// longer than shutdown_grace_ms") had no test.

TEST_F(MiniLbTest, ShutdownDropsRequestsThatOutliveTheGracePeriod) {
    startBackends(1);
    LbOptions options;
    options.shutdownGraceMs = 300;   // /slow takes 2 s
    startLb(options);

    Reply slowReply;
    ScopedThread slow([&] { slowReply = httpGet(lbPort_, "/slow"); });
    ASSERT_TRUE(waitUntil([&] { return activeOn(0) == 1; })) << stats();

    const auto start = std::chrono::steady_clock::now();
    const int exitCode = lb_.terminate();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    slow.join();

    EXPECT_EQ(exitCode, 0) << "running out of patience is still a clean exit";
    EXPECT_GE(elapsed, 300ms) << "the grace period was granted";
    EXPECT_LT(elapsed, 1500ms) << "miniLB did not wait for the 2 s request";
    EXPECT_EQ(slowReply.status, 0) << "the client got no (complete) response";
    EXPECT_NE(lbLog().find("grace period over, dropping 1 connection(s)"), std::string::npos);
}

// ---- Clients that never finish their request (Connection.cpp, readRequest) --
//
// README: "Deadlines everywhere". The read deadline on the *client* side (a
// "slowloris" client that opens a connection and then stalls) was never
// exercised, and neither was a client that hangs up half-way.

TEST_F(MiniLbTest, ClosesAClientThatStallsInTheMiddleOfItsRequest) {
    startBackends(1);
    LbOptions options;
    options.readTimeoutMs = 300;
    startLb(options);

    // Headers without the final blank line: the request is never complete.
    const auto start = std::chrono::steady_clock::now();
    const std::string received = rawExchange(lbPort_, "GET / HTTP/1.1\r\nHost: test.local\r\n");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(received, "") << "there is no request to answer, so the connection is just closed";
    EXPECT_GE(elapsed, 300ms) << "the client was given the whole read timeout";
    EXPECT_LT(elapsed, 3s) << "and was not allowed to hold the connection for long";

    // The stalled client cost nothing: it never reached a backend.
    EXPECT_EQ(requestsOn(0), 0);
    EXPECT_EQ(httpGet(lbPort_, "/").status, 200);
}

TEST_F(MiniLbTest, SurvivesClientsThatHangUpMidRequest) {
    startBackends(1);
    startLb();

    for (int i = 0; i < 20; ++i) {
        const int fd = connectTo(lbPort_);
        ASSERT_GE(fd, 0);
        const std::string partial = "POST /echo HTTP/1.1\r\nHost: test.local\r\nContent-Length: 100\r\n\r\nonly a few bytes";
        (void)!::send(fd, partial.data(), partial.size(), MSG_NOSIGNAL);
        ::close(fd);   // gone before the body is complete
    }

    // All twenty connections are released again (the one that is left is
    // the /stats request itself), and nothing was forwarded or counted.
    ASSERT_TRUE(waitUntil([&] { return statNumber(stats(), "active_connections") == 1; })) << stats();
    EXPECT_EQ(requestsOn(0), 0);
    EXPECT_EQ(statNumber(stats(), "total_requests"), 0);
    EXPECT_EQ(httpGet(lbPort_, "/").status, 200);
}

// ---- Command line and start-up errors (main.cpp) ----------------------------
//
// Exit codes are part of the interface (scripts and Docker rely on them):
// 0 = help was asked for, 2 = wrong usage, 1 = could not start.

TEST_F(MiniLbTest, HelpPrintsUsageAndExitsWithZero) {
    for (const char* flag : {"--help", "-h"}) {
        std::string output;
        EXPECT_EQ(runMiniLb({flag}, output), 0) << flag;
        EXPECT_NE(output.find("usage:"), std::string::npos) << output;
        EXPECT_NE(output.find("--config <file>"), std::string::npos) << output;
    }
}

TEST_F(MiniLbTest, WrongUsageExitsWithTwo) {
    std::string output;
    EXPECT_EQ(runMiniLb({"--no-such-option"}, output), 2);
    EXPECT_NE(output.find("usage:"), std::string::npos) << output;

    // Boundary: "--config" as the last argument has no value to consume.
    EXPECT_EQ(runMiniLb({"--config"}, output), 2);
    EXPECT_NE(output.find("usage:"), std::string::npos) << output;
}

TEST_F(MiniLbTest, StartUpErrorsExitWithOneAndSayWhy) {
    std::string output;

    EXPECT_EQ(runMiniLb({"--config", dir_ + "/missing.yaml"}, output), 1);
    EXPECT_NE(output.find("fatal: "), std::string::npos) << output;
    EXPECT_NE(output.find("cannot read"), std::string::npos) << output;

    const std::string noBackends = writeFile("empty.yaml", "listen_port: " + std::to_string(lbPort_) + "\n");
    EXPECT_EQ(runMiniLb({"--config", noBackends}, output), 1);
    EXPECT_NE(output.find("fatal: "), std::string::npos) << output;
    EXPECT_NE(output.find("at least one backend"), std::string::npos) << output;
}

TEST_F(MiniLbTest, ExitsWithOneWhenThePortIsAlreadyTaken) {
    // Something else is already *listening* on the port miniLB is told to use.
    SilentServer squatter;
    const std::string config = writeFile(
        "taken.yaml", "listen_port: " + std::to_string(squatter.port()) + "\n" +
                          "backends:\n  - { host: 127.0.0.1, port: 9 }\n");

    std::string output;
    EXPECT_EQ(runMiniLb({"--config", config}, output), 1) << output;
    EXPECT_NE(output.find("fatal: "), std::string::npos) << output;
}

// ---- Health checking (HealthChecker.cpp) ------------------------------------

// The existing failover test sends traffic while the backend is dead, so the
// backend is always removed by *failed requests* first. The active checker
// taking a backend out on its own ("is now UNHEALTHY") never happened.
TEST_F(MiniLbTest, HealthCheckerAloneRemovesADeadBackend) {
    startBackends(2);
    LbOptions options;
    options.healthIntervalMs = 50;
    options.failThreshold = 2;
    startLb(options);
    ASSERT_TRUE(reportedHealthy(0));
    ASSERT_TRUE(reportedHealthy(1));

    backends_[1]->kill();   // no client request is sent while it is down

    ASSERT_TRUE(waitUntil([&] { return !reportedHealthy(1); })) << stats();
    EXPECT_TRUE(reportedHealthy(0));
    EXPECT_EQ(requestsOn(1), 0) << "removed by probes, not by a failed request";
    EXPECT_EQ(lbLog().find("(request failures)"), std::string::npos);

    // From now on clients never see the dead backend, not even as a retry.
    for (int i = 0; i < 4; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        EXPECT_EQ(reply.header("X-Backend"), backendId(0));
    }
    EXPECT_EQ(requestsOn(1), 0);
}

// Probe failure classes that were missing: (1) the name does not resolve,
// (2) the backend accepts the connection but never answers (only the timeout
// ends the probe), (3) it closes without sending a status line.
TEST(HealthProbe, FailsForAHostNameThatDoesNotResolve) {
    EXPECT_FALSE(minilb::HealthChecker::probe(kUnresolvableHost, 80, "/health", 500ms));
}

TEST(HealthProbe, GivesUpAtTheTimeoutWhenTheBackendNeverAnswers) {
    SilentServer silent;
    const auto start = std::chrono::steady_clock::now();
    const bool healthy = minilb::HealthChecker::probe(
        "127.0.0.1", static_cast<unsigned short>(silent.port()), "/health", 300ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(healthy);
    EXPECT_GE(elapsed, 300ms) << "it waited for the whole timeout";
    EXPECT_LT(elapsed, 2s) << "and not noticeably longer";
}

TEST(HealthProbe, FailsWhenTheBackendClosesWithoutAnswering) {
    CannedServer closer("");
    EXPECT_FALSE(minilb::HealthChecker::probe(
        "127.0.0.1", static_cast<unsigned short>(closer.port()), "/health", 500ms));
}

// Boundary of "any 2xx is healthy": 199 and 300 are not, 200 and 299 are.
TEST(HealthProbe, OnlyStatus2xxIsHealthy) {
    const std::pair<const char*, bool> cases[] = {
        {"199", false}, {"200", true}, {"204", true}, {"299", true}, {"300", false}, {"500", false},
    };
    for (const auto& [code, expected] : cases) {
        CannedServer server(std::string("HTTP/1.1 ") + code + " X\r\nContent-Length: 0\r\n\r\n");
        EXPECT_EQ(minilb::HealthChecker::probe("127.0.0.1",
                                               static_cast<unsigned short>(server.port()),
                                               "/health", 500ms),
                  expected)
            << "status " << code;
    }
}

// HealthChecker::start() has an "already running" guard that no test hit, and
// stop() must be safe to call twice (the destructor calls it again).
TEST(HealthChecker, StartAndStopAreIdempotent) {
    minilb::BackendPool pool;
    pool.add("127.0.0.1", static_cast<unsigned short>(freePort()), 1);   // nothing listens there

    minilb::Config::HealthCheck settings;
    settings.intervalMs = 20;
    settings.timeoutMs = 200;
    settings.failThreshold = 1;

    minilb::HealthChecker checker(pool, settings);
    checker.start();
    checker.start();   // must not start a second thread (std::terminate if it did)
    EXPECT_TRUE(waitUntil([&] { return !pool.all()[0]->healthy.load(); }))
        << "the checker is running and found the dead backend";
    checker.stop();
    checker.stop();
}

} // namespace
