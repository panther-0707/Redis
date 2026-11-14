// Tests written from code review findings (software-testing/docs/05-code-review.md).
//
// A review finding is a claim about behaviour. Before anything is changed,
// the claim is turned into a test that fails if the finding is real. The
// test then stays as the regression test for the fix.

#include "TestSupport.hpp"

using namespace minilb_test;
using namespace std::chrono_literals;

namespace {

const std::string kOk = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
const std::string kHangUp;   // read the request, answer nothing, close

// ---- R2: successful requests never reset the failure streak -----------------
//
// README, "Health checks": a backend is removed after `fail_threshold`
// CONSECUTIVE failures. The review found that only the health checker's
// probes ended a streak; a successful proxied request did not. So failures
// scattered between successes still added up, and a backend that answered
// half of its requests was removed as if it had failed three times in a row.
//
// The backend here fails every second connection. With fail_threshold = 3 it
// must never be removed, because no two failures are adjacent.
TEST_F(MiniLbTest, R2_FailuresSeparatedBySuccessesDoNotRemoveABackend) {
    CannedServer flaky({kOk, kHangUp});   // connection 0 ok, 1 fails, 2 ok, 3 fails, ...
    addBackendPort(flaky.port());
    LbOptions options;
    options.healthIntervalMs = 60000;   // one probe at start-up, none during the test
    options.failThreshold = 3;
    startLb(options);
    // Connection 0 is the start-up health probe; wait until it has happened
    // so that the pattern below is deterministic.
    ASSERT_TRUE(waitUntil([&] { return flaky.requests().size() == 1; }));

    int succeeded = 0;
    int failed = 0;
    for (int i = 0; i < 10; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        if (reply.status == 200) {
            ++succeeded;
        } else if (reply.status == 502) {
            ++failed;   // the only backend hung up, and there is no other to retry on
        } else {
            ADD_FAILURE() << "request " << i + 1 << ": unexpected status " << reply.status
                          << " (503 means the backend was removed)";
        }
    }

    EXPECT_EQ(failed, 5) << "every second request hits a connection that hangs up";
    EXPECT_EQ(succeeded, 5);
    EXPECT_TRUE(reportedHealthy(0)) << "five failures, but never two in a row";
    EXPECT_EQ(lbLog().find("is now UNHEALTHY"), std::string::npos);
}

// The other side: the fix must not stop real streaks from being counted.
// Three failures in a row still remove the backend, and the next request is
// refused with 503 instead of being sent to it.
TEST_F(MiniLbTest, R2_ThreeFailuresInARowStillRemoveABackend) {
    CannedServer dying({kOk, kOk, kHangUp, kHangUp, kHangUp});   // probe, 1 success, then 3 failures
    addBackendPort(dying.port());
    LbOptions options;
    options.healthIntervalMs = 60000;
    options.failThreshold = 3;
    startLb(options);
    ASSERT_TRUE(waitUntil([&] { return dying.requests().size() == 1; }));

    EXPECT_EQ(httpGet(lbPort_, "/").status, 200);
    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);
    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);
    EXPECT_TRUE(reportedHealthy(0)) << "two in a row is still below the threshold";
    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);
    EXPECT_FALSE(reportedHealthy(0)) << "three in a row";
    EXPECT_EQ(httpGet(lbPort_, "/").status, 503) << "no healthy backend is left";
}

} // namespace
