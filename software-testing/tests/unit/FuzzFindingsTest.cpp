// Regression tests for defects found by the fuzzer
// (software-testing/fuzz/http_parser_fuzz.cpp).
//
// A fuzzer finding is an unreadable blob of bytes. Each one is reduced here
// to the smallest example that shows the same fault, so that the reason is
// clear and the ordinary unit-test run protects against it.

#include "miniLB/http/HttpParser.hpp"

#include <gtest/gtest.h>

#include <string>

using minilb::HttpResponseParser;
using minilb::ParseStatus;

// DEFECT D2 (see software-testing/docs/03-test-results.md):
//
// `max_response_bytes` caps the size of a backend response. The parser
// applied the cap to everything it had buffered, including bytes that arrive
// AFTER a complete response. Such bytes are not part of the response (the
// parser ignores them otherwise), but when they came in the same read as the
// end of the response they pushed the buffer over the cap and the response
// was rejected. The same bytes in a later read were ignored. So whether a
// valid response was accepted depended on how TCP happened to split it.
TEST(FuzzFindings, D2_BytesAfterACompleteResponseDoNotCountTowardsTheSizeCap) {
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    const std::string trailing(100, '!');
    const std::size_t cap = response.size() + 10;   // the response fits, response + trailing does not

    // Trailing bytes in a later read: was already fine.
    HttpResponseParser later(cap);
    ASSERT_EQ(later.feed(response), ParseStatus::Complete);
    EXPECT_EQ(later.feed(trailing), ParseStatus::Complete);

    // Trailing bytes in the same read: must give the same answer.
    HttpResponseParser together(cap);
    EXPECT_EQ(together.feed(response + trailing), ParseStatus::Complete);
    EXPECT_EQ(together.response().status, 200);
    EXPECT_EQ(together.response().body, "ok");
}

// The same for a response that has no body by definition (the fuzzer's own
// input was a "100" response followed by other bytes).
TEST(FuzzFindings, D2_BytesAfterABodilessResponseDoNotCountEither) {
    const std::string response = "HTTP/1.1 204 No Content\r\n\r\n";
    HttpResponseParser parser(response.size());
    EXPECT_EQ(parser.feed(response + std::string(50, '!')), ParseStatus::Complete);
    EXPECT_EQ(parser.response().status, 204);
}

// The fix must not weaken the cap: a response that really is too large is
// still refused, in one piece and in two.
TEST(FuzzFindings, D2_AResponseOverTheCapIsStillRefused) {
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n";
    const std::size_t cap = head.size() + 49;   // one byte short

    HttpResponseParser together(cap);
    EXPECT_EQ(together.feed(head + std::string(50, 'x')), ParseStatus::Error);

    HttpResponseParser split(cap);
    EXPECT_EQ(split.feed(head), ParseStatus::Error) << "refused as soon as the declared size is known";
}
