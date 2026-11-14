// Requirements-driven system tests.
//
// Technique: requirements analysis. Writing out the requirements
// (software-testing/docs/01-requirements.md) showed two promises in the
// README that no test checked:
//
//   F5b  "If the request may already have reached the first backend, it is
//         only retried for idempotent methods (GET, HEAD, OPTIONS, PUT,
//         DELETE); a POST is not repeated."
//   F10  "Request headers sent to backends: X-Forwarded-For (client IP
//         appended) and Connection: close."
//
// Both are invisible to line coverage: the code was executed by other tests,
// but nothing asserted on the behaviour.

#include "TestSupport.hpp"

#include "miniLB/http/HttpParser.hpp"

using namespace minilb_test;
using namespace std::chrono_literals;

namespace {

// Sends "<METHOD> <target>" with an empty body through miniLB.
Reply sendMethod(int port, const std::string& method, const std::string& target) {
    return parseReply(rawExchange(port, method + " " + target +
                                            " HTTP/1.1\r\nHost: test.local\r\nContent-Length: 0\r\n\r\n"),
                      /*headRequest=*/method == "HEAD");
}

// Did `server` receive a request line starting with "<METHOD> <target> "?
bool received(const CannedServer& server, const std::string& method, const std::string& target) {
    for (const std::string& request : server.requests()) {
        if (request.rfind(method + " " + target + " ", 0) == 0) {
            return true;
        }
    }
    return false;
}

// ============================ F5b: safe retries ==============================
//
// Equivalence classes of the method: idempotent (retried) and not idempotent
// (not retried). One test per member of each class, because the code lists
// the methods one by one and any of them could be missing.

struct RetryCase {
    const char* method;
    bool idempotent;
};

void PrintTo(const RetryCase& retryCase, std::ostream* out) {
    *out << retryCase.method;
}

class RetryByMethod : public MiniLbTest, public ::testing::WithParamInterface<RetryCase> {};

TEST_P(RetryByMethod, ARequestThatReachedABackendIsRepeatedOnlyIfIdempotent) {
    const std::string method = GetParam().method;

    // Backend 0 is a real demo backend. Backend 1 accepts the connection,
    // reads the request and hangs up without answering: from miniLB's point
    // of view the request "may already have been acted on".
    startBackends(1);
    CannedServer hangsUp("");
    addBackendPort(hangsUp.port());
    LbOptions options;
    options.healthIntervalMs = 60000;   // keep the broken backend in rotation
    options.failThreshold = 100;
    startLb(options);

    // Round robin: the first request goes to backend 0, the second to the
    // broken backend 1.
    ASSERT_EQ(httpGet(lbPort_, "/warm-up").header("X-Backend"), backendId(0));
    ASSERT_EQ(requestsOn(0), 1);

    const Reply reply = sendMethod(lbPort_, method, "/under-test");
    ASSERT_TRUE(received(hangsUp, method, "/under-test"))
        << "the test is only meaningful if the broken backend got the request";

    if (GetParam().idempotent) {
        EXPECT_EQ(reply.header("X-Backend"), backendId(0)) << "repeated on the healthy backend";
        EXPECT_NE(reply.status, 502);
        EXPECT_EQ(requestsOn(0), 2);
    } else {
        EXPECT_EQ(reply.status, 502) << "the client is told, the request is not repeated";
        EXPECT_EQ(requestsOn(0), 1) << "the healthy backend must not see a second copy";
    }
}

INSTANTIATE_TEST_SUITE_P(Methods, RetryByMethod,
                         ::testing::Values(RetryCase{"GET", true}, RetryCase{"HEAD", true},
                                           RetryCase{"OPTIONS", true}, RetryCase{"PUT", true},
                                           RetryCase{"DELETE", true}, RetryCase{"POST", false},
                                           RetryCase{"PATCH", false}));

// The other half of the rule: "if nothing was sent yet (connect failed),
// always". A POST that could not even be delivered is safe to send elsewhere.
TEST_F(MiniLbTest, APostIsRetriedWhenTheFirstBackendCouldNotBeReached) {
    startBackends(1);
    addBackendPort(freePort());   // nothing listens: connection refused
    LbOptions options;
    options.healthIntervalMs = 60000;
    options.failThreshold = 100;
    startLb(options);

    ASSERT_EQ(httpGet(lbPort_, "/warm-up").header("X-Backend"), backendId(0));

    // Second request: round robin picks the dead backend first.
    const Reply reply = httpPost(lbPort_, "/echo", "order #1");
    EXPECT_EQ(reply.status, 200);
    EXPECT_EQ(reply.body, "order #1");
    EXPECT_EQ(reply.header("X-Backend"), backendId(0));
    EXPECT_EQ(requestsOn(1), 1) << "the dead backend was tried first";
}

// ========================= F10: what the backend sees =======================

class ForwardedRequest : public MiniLbTest {
protected:
    // Sends `rawRequest` through miniLB to a recording backend and returns
    // the request as that backend received it.
    minilb::HttpRequest forward(const std::string& rawRequest) {
        CannedServer backend("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        addBackendPort(backend.port());
        LbOptions options;
        options.healthIntervalMs = 60000;
        startLb(options);

        const Reply reply = parseReply(rawExchange(lbPort_, rawRequest));
        EXPECT_EQ(reply.status, 200);

        // Skip the health probe; the other request is ours.
        for (const std::string& seen : backend.requests()) {
            if (seen.rfind("GET /health ", 0) == 0) {
                continue;
            }
            minilb::HttpParser parser;
            EXPECT_EQ(parser.feed(seen), minilb::ParseStatus::Complete) << seen;
            return parser.request();
        }
        ADD_FAILURE() << "the backend never received the request";
        return {};
    }

    static int count(const minilb::HttpRequest& request, const std::string& name) {
        int n = 0;
        for (const auto& header : request.headers) {
            if (minilb::iequals(header.first, name)) {
                ++n;
            }
        }
        return n;
    }
};

TEST_F(ForwardedRequest, GetsTheClientAddressInXForwardedFor) {
    const minilb::HttpRequest seen = forward("GET /page?x=1 HTTP/1.1\r\nHost: shop.example\r\n\r\n");

    ASSERT_NE(seen.header("X-Forwarded-For"), nullptr);
    EXPECT_EQ(*seen.header("X-Forwarded-For"), "127.0.0.1");
    EXPECT_EQ(count(seen, "X-Forwarded-For"), 1);

    // The rest of the request is passed on untouched.
    EXPECT_EQ(seen.method, "GET");
    EXPECT_EQ(seen.path, "/page");
    EXPECT_EQ(seen.query, "x=1");
    ASSERT_NE(seen.header("Host"), nullptr);
    EXPECT_EQ(*seen.header("Host"), "shop.example");
}

TEST_F(ForwardedRequest, AppendsToAnExistingXForwardedFor) {
    const minilb::HttpRequest seen = forward(
        "GET / HTTP/1.1\r\nHost: test.local\r\nX-Forwarded-For: 203.0.113.7, 10.0.0.1\r\n\r\n");

    ASSERT_NE(seen.header("X-Forwarded-For"), nullptr);
    EXPECT_EQ(*seen.header("X-Forwarded-For"), "203.0.113.7, 10.0.0.1, 127.0.0.1")
        << "earlier hops are kept, our client is added at the end";
    EXPECT_EQ(count(seen, "X-Forwarded-For"), 1);
}

TEST_F(ForwardedRequest, ReplacesHopByHopHeadersWithConnectionClose) {
    const minilb::HttpRequest seen = forward("GET / HTTP/1.1\r\n"
                                             "Host: test.local\r\n"
                                             "Connection: keep-alive\r\n"
                                             "Keep-Alive: timeout=5\r\n"
                                             "Proxy-Connection: keep-alive\r\n"
                                             "Upgrade: websocket\r\n"
                                             "X-Custom: stays\r\n\r\n");

    ASSERT_NE(seen.header("Connection"), nullptr);
    EXPECT_EQ(*seen.header("Connection"), "close");
    EXPECT_EQ(count(seen, "Connection"), 1);
    EXPECT_EQ(seen.header("Keep-Alive"), nullptr);
    EXPECT_EQ(seen.header("Proxy-Connection"), nullptr);
    EXPECT_EQ(seen.header("Upgrade"), nullptr);

    ASSERT_NE(seen.header("X-Custom"), nullptr) << "end-to-end headers are not touched";
    EXPECT_EQ(*seen.header("X-Custom"), "stays");
}

TEST_F(ForwardedRequest, PassesThePostBodyOnUnchanged) {
    const std::string body = "name=miniLB&note=line1%0Aline2";
    const minilb::HttpRequest seen =
        forward("POST /submit HTTP/1.1\r\nHost: test.local\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);

    EXPECT_EQ(seen.method, "POST");
    EXPECT_EQ(seen.path, "/submit");
    EXPECT_EQ(seen.body, body);
}

} // namespace
