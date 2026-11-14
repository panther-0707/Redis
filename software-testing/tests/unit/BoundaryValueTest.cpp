// Boundary-value tests.
//
// Technique: boundary value analysis (functional testing). For every numeric
// limit in miniLB the input domain splits into "accepted" and "rejected", and
// off-by-one mistakes sit exactly on the border (`<` instead of `<=`). Each
// test below picks the values just below, on and just above one border.
//
// Borders that the original suite already tested on both sides (request body
// limit, health thresholds) are not repeated here.

#include "FakeStore.hpp"
#include "miniLB/http/HttpParser.hpp"
#include "miniLB/redis/Cache.hpp"
#include "miniLB/redis/RateLimiter.hpp"
#include "miniLB/util/Config.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

using minilb::Cache;
using minilb::Config;
using minilb::HttpParser;
using minilb::HttpResponseParser;
using minilb::ParseStatus;
using minilb::RateLimiter;

namespace {

// Loads `yaml` as a config file. Returns "" on success, the error message
// otherwise.
std::string loadError(const std::string& yaml) {
    char path[] = "/tmp/minilb-boundary-XXXXXX";
    const int fd = mkstemp(path);
    EXPECT_NE(fd, -1);
    close(fd);
    {
        std::ofstream file(path);
        file << yaml;
    }
    std::string error;
    try {
        Config::load(path);
    } catch (const std::runtime_error& e) {
        error = e.what();
    }
    std::remove(path);
    return error;
}

} // namespace

// ============================== Rate limiter ================================

// Border: the request count. Request number `limit` is the last one allowed.
class RateLimitBoundary : public ::testing::TestWithParam<int> {};

TEST_P(RateLimitBoundary, RequestNumberLimitPassesAndLimitPlusOneIsRejected) {
    const int limit = GetParam();
    FakeStore store;
    RateLimiter limiter(store, limit, 60);

    for (int i = 1; i < limit; ++i) {
        ASSERT_TRUE(limiter.allow("ip", 600).allowed) << "request " << i;
    }
    EXPECT_TRUE(limiter.allow("ip", 600).allowed) << "request number limit (" << limit << ")";
    EXPECT_FALSE(limiter.allow("ip", 600).allowed) << "request number limit + 1";
}

// 1 is the smallest limit that still limits (0 means "disabled").
INSTANTIATE_TEST_SUITE_P(Limits, RateLimitBoundary, ::testing::Values(1, 2, 100));

// Border: the limit itself. 1 limits, 0 and negative values switch it off.
TEST(RateLimitBoundaries, LimitOfZeroOrLessDisablesAndOneEnables) {
    FakeStore store;
    EXPECT_TRUE(RateLimiter(store, 1, 60).enabled());
    EXPECT_FALSE(RateLimiter(store, 0, 60).enabled());
    EXPECT_FALSE(RateLimiter(store, -1, 60).enabled());

    RateLimiter negative(store, -1, 60);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(negative.allow("ip", 600).allowed);
    }
}

// Border: the edge between two windows. With 60 s windows, second 659 is the
// last second of window 10 and second 660 the first of window 11.
TEST(RateLimitBoundaries, LastSecondOfAWindowAndFirstSecondOfTheNext) {
    FakeStore store;
    RateLimiter limiter(store, 1, 60);

    EXPECT_TRUE(limiter.allow("ip", 600).allowed);    // first second of window 10
    EXPECT_FALSE(limiter.allow("ip", 659).allowed);   // last second, same window: still counted
    EXPECT_TRUE(limiter.allow("ip", 660).allowed);    // first second of window 11: fresh budget
    EXPECT_FALSE(limiter.allow("ip", 660).allowed);
}

// Border: Retry-After. It is the time left in the window, so it ranges from
// the whole window (rejected in the first second) down to 1 (last second),
// and is never 0.
TEST(RateLimitBoundaries, RetryAfterRunsFromTheWholeWindowDownToOne) {
    FakeStore store;
    RateLimiter limiter(store, 1, 60);
    limiter.allow("ip", 600);   // uses up the budget of window 10

    EXPECT_EQ(limiter.allow("ip", 600).retryAfterSeconds, 60) << "first second of the window";
    EXPECT_EQ(limiter.allow("ip", 601).retryAfterSeconds, 59);
    EXPECT_EQ(limiter.allow("ip", 658).retryAfterSeconds, 2);
    EXPECT_EQ(limiter.allow("ip", 659).retryAfterSeconds, 1) << "last second of the window";
}

// Border: the window length. Anything below 1 s is treated as 1 s (a length
// of 0 would otherwise divide by zero).
TEST(RateLimitBoundaries, WindowLengthBelowOneSecondIsClampedToOne) {
    for (const int configured : {0, -5}) {
        FakeStore store;
        RateLimiter limiter(store, 1, configured);

        EXPECT_TRUE(limiter.allow("ip", 1000).allowed) << configured;
        const RateLimiter::Decision rejected = limiter.allow("ip", 1000);
        EXPECT_FALSE(rejected.allowed) << configured;
        EXPECT_EQ(rejected.retryAfterSeconds, 1) << configured;
        EXPECT_TRUE(limiter.allow("ip", 1001).allowed) << "one second later is a new window";
        EXPECT_EQ(store.ttls["ratelimit:ip:1000"], 1) << "the counter expires with its 1 s window";
    }
}

// Not a pass/fail requirement but a documented limitation, pinned down so
// that it cannot change unnoticed: README, "Fixed-window rate limiting" says
// a client "can send up to twice the limit across a window boundary".
TEST(RateLimitBoundaries, KnownLimitation_TwiceTheLimitFitsAcrossAWindowEdge) {
    FakeStore store;
    RateLimiter limiter(store, 5, 60);

    int allowed = 0;
    for (int i = 0; i < 5; ++i) {
        allowed += limiter.allow("ip", 659).allowed ? 1 : 0;   // last second of window 10
    }
    for (int i = 0; i < 5; ++i) {
        allowed += limiter.allow("ip", 660).allowed ? 1 : 0;   // first second of window 11
    }
    EXPECT_EQ(allowed, 10) << "10 requests within two seconds, although the limit is 5 per minute";
}

// ============================== Request parser ==============================

// Border: the size of the request head. HttpParser.hpp defines the limit as
// "request line + all headers", i.e. everything before the blank line that
// ends the head. The existing tests use heads far above the limit; these sit
// exactly on it.
namespace {

// A syntactically valid request head of exactly `size` bytes (without the
// final blank line).
std::string headOfSize(std::size_t size) {
    const std::string start = "GET / HTTP/1.1\r\nHost: h\r\nX-Pad: ";
    return start + std::string(size - start.size(), 'a');
}

} // namespace

TEST(ParserBoundaries, RequestHeadOfExactlyTheLimitIsAccepted) {
    HttpParser::Limits limits;
    limits.maxHeaderBytes = 64;
    HttpParser parser(limits);
    EXPECT_EQ(parser.feed(headOfSize(64) + "\r\n\r\n"), ParseStatus::Complete);
}

TEST(ParserBoundaries, RequestHeadOneByteOverTheLimitIs431) {
    HttpParser::Limits limits;
    limits.maxHeaderBytes = 64;
    HttpParser parser(limits);
    EXPECT_EQ(parser.feed(headOfSize(65) + "\r\n\r\n"), ParseStatus::Error);
    EXPECT_EQ(parser.errorStatus(), 431);
}

// DEFECT FOUND BY THIS TEST (still open, hence DISABLED_):
//
// Requirement F14 says the parser gives the same answer however TCP splits
// the bytes. At this border it does not. A head of exactly the limit is
// accepted when it arrives in one piece (test above), but rejected with 431
// when a read happens to end inside the final "\r\n\r\n": at that moment the
// buffer holds limit + 1..3 bytes and no complete terminator yet, and
// HttpParser::feed() compares the *whole buffer* with the limit.
//
// So the same valid request is accepted or rejected depending on packet
// boundaries. Run it with:
//   ./unit_tests --gtest_also_run_disabled_tests --gtest_filter='*Fragmented*'
// Remove the DISABLED_ prefix once HttpParser.cpp is fixed.
TEST(ParserBoundaries, DISABLED_RequestHeadOfExactlyTheLimitIsAcceptedHoweverItIsFragmented) {
    const std::string wire = headOfSize(64) + "\r\n\r\n";

    for (std::size_t split = 1; split < wire.size(); ++split) {
        HttpParser::Limits limits;
        limits.maxHeaderBytes = 64;
        HttpParser parser(limits);

        ASSERT_EQ(parser.feed(wire.substr(0, split)), ParseStatus::NeedMore)
            << "first " << split << " of " << wire.size() << " bytes";
        EXPECT_EQ(parser.feed(wire.substr(split)), ParseStatus::Complete) << "split after byte " << split;
    }
}

// The same property away from the border holds today, and stays enabled as a
// regression test: a head well below the limit parses for every split point.
TEST(ParserBoundaries, RequestBelowTheLimitParsesForEverySplitPoint) {
    const std::string wire = headOfSize(60) + "\r\n\r\n";

    for (std::size_t split = 1; split < wire.size(); ++split) {
        HttpParser::Limits limits;
        limits.maxHeaderBytes = 64;
        HttpParser parser(limits);

        ASSERT_EQ(parser.feed(wire.substr(0, split)), ParseStatus::NeedMore) << "split after byte " << split;
        EXPECT_EQ(parser.feed(wire.substr(split)), ParseStatus::Complete) << "split after byte " << split;
    }
}

// The head limit must not count the body: a small head with a large body is
// fine as long as the body is within its own limit.
TEST(ParserBoundaries, HeadLimitDoesNotCountTheBody) {
    const std::string head = "POST / HTTP/1.1\r\nContent-Length: 500\r\n\r\n";
    HttpParser::Limits limits;
    limits.maxHeaderBytes = head.size();
    limits.maxBodyBytes = 500;
    HttpParser parser(limits);
    EXPECT_EQ(parser.feed(head + std::string(500, 'b')), ParseStatus::Complete);
}

// Border: the request body limit, one above (the existing suite has "exactly
// at the limit" and "far above").
TEST(ParserBoundaries, BodyOneByteOverTheLimitIs413) {
    HttpParser::Limits limits;
    limits.maxBodyBytes = 10;
    HttpParser parser(limits);
    EXPECT_EQ(parser.feed("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n"), ParseStatus::Error);
    EXPECT_EQ(parser.errorStatus(), 413);
}

// Border: Content-Length values around zero. "0" is the smallest valid
// length; a sign of either kind is not a valid length (RFC 9110: 1*DIGIT).
TEST(ParserBoundaries, ContentLengthAroundZero) {
    HttpParser zero;
    ASSERT_EQ(zero.feed("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n"), ParseStatus::Complete);
    EXPECT_EQ(zero.request().body, "");

    HttpParser one;
    EXPECT_EQ(one.feed("POST / HTTP/1.1\r\nContent-Length: 1\r\n\r\n"), ParseStatus::NeedMore);
    EXPECT_EQ(one.feed("x"), ParseStatus::Complete);

    for (const char* value : {"+0", "+1", "-0", "-1"}) {
        HttpParser parser;
        EXPECT_EQ(parser.feed(std::string("POST / HTTP/1.1\r\nContent-Length: ") + value + "\r\n\r\n"),
                  ParseStatus::Error)
            << "Content-Length: " << value;
        EXPECT_EQ(parser.errorStatus(), 400) << "Content-Length: " << value;
    }
}

// ============================== Response parser =============================

// Border: valid status codes are 100..599.
TEST(ParserBoundaries, ResponseStatusCodeRange) {
    for (const char* code : {"100", "101", "598", "599"}) {
        HttpResponseParser parser;
        EXPECT_NE(parser.feed(std::string("HTTP/1.1 ") + code + " X\r\nContent-Length: 0\r\n\r\n"),
                  ParseStatus::Error)
            << code;
    }
    for (const char* code : {"099", "600", "999"}) {
        HttpResponseParser parser;
        EXPECT_EQ(parser.feed(std::string("HTTP/1.1 ") + code + " X\r\nContent-Length: 0\r\n\r\n"),
                  ParseStatus::Error)
            << code;
    }
}

// Border: the cap on a backend response whose length is declared up front.
// A declared body of exactly the cap must not be refused.
TEST(ParserBoundaries, DeclaredResponseBodyAtTheCapAndOneOver) {
    HttpResponseParser atCap(100);
    EXPECT_NE(atCap.feed("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"), ParseStatus::Error)
        << "a body of exactly the cap is allowed to start";

    HttpResponseParser over(100);
    EXPECT_EQ(over.feed("HTTP/1.1 200 OK\r\nContent-Length: 101\r\n\r\n"), ParseStatus::Error)
        << "one byte over the cap is refused before the body arrives";
}

// ================================== Cache ===================================

// Border: cache.max_entry_bytes. A body of exactly the limit is stored, one
// byte more is not.
TEST(CacheBoundaries, BodyOfExactlyMaxEntryBytesIsCacheableAndOneMoreIsNot) {
    FakeStore store;
    Config::Cache settings;
    settings.enabled = true;
    settings.ttlSeconds = 30;
    settings.maxEntryBytes = 10;
    const Cache cache(store, settings);

    EXPECT_TRUE(cache.isCacheableResponse(minilb::HttpResponse::make(200, std::string(9, 'x'))));
    EXPECT_TRUE(cache.isCacheableResponse(minilb::HttpResponse::make(200, std::string(10, 'x'))))
        << "exactly max_entry_bytes";
    EXPECT_FALSE(cache.isCacheableResponse(minilb::HttpResponse::make(200, std::string(11, 'x'))))
        << "max_entry_bytes + 1";
}

// Border: only status 200 is cached; its neighbours are not.
TEST(CacheBoundaries, OnlyStatus200IsCacheable) {
    FakeStore store;
    Config::Cache settings;
    settings.enabled = true;
    const Cache cache(store, settings);

    EXPECT_FALSE(cache.isCacheableResponse(minilb::HttpResponse::make(199, "x")));
    EXPECT_TRUE(cache.isCacheableResponse(minilb::HttpResponse::make(200, "x")));
    EXPECT_FALSE(cache.isCacheableResponse(minilb::HttpResponse::make(201, "x")));
}

// ================================== Config ==================================

// Border: TCP port numbers are 1..65535, for the listen port and for backends.
TEST(ConfigBoundaries, PortRange) {
    const std::string backend = "backends:\n  - { host: a, port: 80 }\n";
    EXPECT_NE(loadError(backend + "listen_port: 0\n"), "");
    EXPECT_EQ(loadError(backend + "listen_port: 1\n"), "");
    EXPECT_EQ(loadError(backend + "listen_port: 65535\n"), "");
    EXPECT_NE(loadError(backend + "listen_port: 65536\n"), "");
    EXPECT_NE(loadError(backend + "listen_port: -1\n"), "");

    EXPECT_NE(loadError("backends:\n  - { host: a, port: 0 }\n"), "");
    EXPECT_EQ(loadError("backends:\n  - { host: a, port: 1 }\n"), "");
    EXPECT_EQ(loadError("backends:\n  - { host: a, port: 65535 }\n"), "");
    EXPECT_NE(loadError("backends:\n  - { host: a, port: 65536 }\n"), "");
}

// Border: every "must be at least 1" setting accepts 1 and rejects 0.
TEST(ConfigBoundaries, SettingsThatMustBeAtLeastOne) {
    const std::string backend = "backends:\n  - { host: a, port: 80 }\n";
    for (const char* setting : {
             "threads: %d",
             "redis: { threads: %d }",
             "redis: { timeout_ms: %d }",
             "health_check: { interval_ms: %d }",
             "health_check: { timeout_ms: %d }",
             "health_check: { fail_threshold: %d }",
             "health_check: { success_threshold: %d }",
             "rate_limit: { window_seconds: %d }",
             "cache: { ttl_seconds: %d }",
             "timeouts_ms: { connect: %d }",
             "timeouts_ms: { read: %d }",
             "timeouts_ms: { write: %d }",
         }) {
        char line[128];
        std::snprintf(line, sizeof(line), setting, 1);
        EXPECT_EQ(loadError(backend + line + "\n"), "") << line;
        std::snprintf(line, sizeof(line), setting, 0);
        EXPECT_NE(loadError(backend + line + "\n"), "") << line;
    }
}

// Border: rate_limit.requests has no lower limit of 1, because 0 means "off".
TEST(ConfigBoundaries, RateLimitOfZeroIsAccepted) {
    EXPECT_EQ(loadError("backends:\n  - { host: a, port: 80 }\nrate_limit: { requests: 0 }\n"), "");
}
