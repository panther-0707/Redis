// Unit tests for the HTTP request and response parsers.

#include "miniLB/http/HttpParser.hpp"

#include <gtest/gtest.h>

#include <string>

using minilb::HttpParser;
using minilb::HttpResponseParser;
using minilb::ParseStatus;

// ------------------------------ Requests: valid ----------------------------

TEST(HttpParser, ParsesSimpleGet) {
    HttpParser parser;
    ASSERT_EQ(parser.feed("GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n"),
              ParseStatus::Complete);

    const auto& request = parser.request();
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.target, "/index.html");
    EXPECT_EQ(request.path, "/index.html");
    EXPECT_EQ(request.query, "");
    EXPECT_EQ(request.version, "HTTP/1.1");
    ASSERT_NE(request.header("Host"), nullptr);
    EXPECT_EQ(*request.header("Host"), "example.com");
    EXPECT_TRUE(request.body.empty());
}

TEST(HttpParser, SplitsPathAndQuery) {
    HttpParser parser;
    ASSERT_EQ(parser.feed("GET /search?q=cats&page=2 HTTP/1.1\r\n\r\n"), ParseStatus::Complete);
    EXPECT_EQ(parser.request().target, "/search?q=cats&page=2");
    EXPECT_EQ(parser.request().path, "/search");
    EXPECT_EQ(parser.request().query, "q=cats&page=2");
}

TEST(HttpParser, HeaderLookupIgnoresCaseAndTrimsValue) {
    HttpParser parser;
    ASSERT_EQ(parser.feed("GET / HTTP/1.1\r\ncOnTeNt-TyPe:   text/plain  \r\n\r\n"),
              ParseStatus::Complete);
    ASSERT_NE(parser.request().header("Content-Type"), nullptr);
    EXPECT_EQ(*parser.request().header("content-type"), "text/plain");
    EXPECT_EQ(parser.request().header("Missing"), nullptr);
}

TEST(HttpParser, ParsesPostWithBody) {
    HttpParser parser;
    ASSERT_EQ(parser.feed("POST /echo HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world"),
              ParseStatus::Complete);
    EXPECT_EQ(parser.request().method, "POST");
    EXPECT_EQ(parser.request().body, "hello world");
}

TEST(HttpParser, AcceptsHttp10) {
    HttpParser parser;
    ASSERT_EQ(parser.feed("GET / HTTP/1.0\r\n\r\n"), ParseStatus::Complete);
    EXPECT_EQ(parser.request().version, "HTTP/1.0");
}

TEST(HttpParser, IgnoresBytesAfterTheFirstRequest) {
    HttpParser parser;
    ASSERT_EQ(parser.feed("GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n"), ParseStatus::Complete);
    EXPECT_EQ(parser.request().path, "/a");
    // Further input does not change the result.
    EXPECT_EQ(parser.feed("more"), ParseStatus::Complete);
}

TEST(HttpParser, SerializeRoundTrips) {
    const std::string wire = "POST /echo?x=1 HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\nhi";
    HttpParser parser;
    ASSERT_EQ(parser.feed(wire), ParseStatus::Complete);
    EXPECT_EQ(parser.request().serialize(), wire);
}

// --------------------------- Requests: partial reads -----------------------

TEST(HttpParser, NeedsMoreUntilHeadersEnd) {
    HttpParser parser;
    EXPECT_EQ(parser.feed("GET / HTTP/1.1\r\n"), ParseStatus::NeedMore);
    EXPECT_EQ(parser.feed("Host: example.com\r\n"), ParseStatus::NeedMore);
    EXPECT_EQ(parser.feed("\r\n"), ParseStatus::Complete);
}

TEST(HttpParser, HandlesOneByteAtATime) {
    const std::string wire =
        "POST /submit?a=1 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\nabcde";

    HttpParser parser;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        const ParseStatus status = parser.feed(std::string_view(&wire[i], 1));
        if (i + 1 < wire.size()) {
            ASSERT_EQ(status, ParseStatus::NeedMore) << "after byte " << i;
        } else {
            ASSERT_EQ(status, ParseStatus::Complete);
        }
    }
    EXPECT_EQ(parser.request().path, "/submit");
    EXPECT_EQ(parser.request().query, "a=1");
    EXPECT_EQ(parser.request().body, "abcde");
}

TEST(HttpParser, HandlesBlankLineSplitAcrossReads) {
    // The "\r\n\r\n" terminator is cut at every possible position.
    for (std::size_t cut = 1; cut < 4; ++cut) {
        const std::string head = "GET / HTTP/1.1\r\nHost: a";
        const std::string terminator = "\r\n\r\n";

        HttpParser parser;
        EXPECT_EQ(parser.feed(head + terminator.substr(0, cut)), ParseStatus::NeedMore);
        EXPECT_EQ(parser.feed(terminator.substr(cut)), ParseStatus::Complete) << "cut=" << cut;
    }
}

TEST(HttpParser, NeedsMoreUntilBodyIsComplete) {
    HttpParser parser;
    EXPECT_EQ(parser.feed("POST / HTTP/1.1\r\nContent-Length: 6\r\n\r\nabc"), ParseStatus::NeedMore);
    EXPECT_EQ(parser.feed("de"), ParseStatus::NeedMore);
    EXPECT_EQ(parser.feed("f"), ParseStatus::Complete);
    EXPECT_EQ(parser.request().body, "abcdef");
}

// ----------------------------- Requests: malformed -------------------------

// Each of these must be rejected with 400 Bad Request.
class MalformedRequest : public ::testing::TestWithParam<const char*> {};

TEST_P(MalformedRequest, IsRejectedWith400) {
    HttpParser parser;
    EXPECT_EQ(parser.feed(GetParam()), ParseStatus::Error);
    EXPECT_EQ(parser.errorStatus(), 400);
    EXPECT_FALSE(parser.errorMessage().empty());
}

INSTANTIATE_TEST_SUITE_P(
    HttpParser, MalformedRequest,
    ::testing::Values(
        "GARBAGE\r\n\r\n",                                        // no spaces at all
        "GET /\r\n\r\n",                                          // missing version
        "GET / HTTP/2.0\r\n\r\n",                                 // unsupported version
        "GET / http/1.1\r\n\r\n",                                 // version is case-sensitive
        "get / HTTP/1.1\r\n\r\n",                                 // lower-case method
        "G3T / HTTP/1.1\r\n\r\n",                                 // digit in method
        " / HTTP/1.1\r\n\r\n",                                    // empty method
        "GET index.html HTTP/1.1\r\n\r\n",                        // target without leading '/'
        "GET /a b HTTP/1.1\r\n\r\n",                              // space inside target
        "GET / HTTP/1.1\r\nNoColonHere\r\n\r\n",                  // header without ':'
        "GET / HTTP/1.1\r\n: value\r\n\r\n",                      // empty header name
        "GET / HTTP/1.1\r\nBad Name: x\r\n\r\n",                  // space in header name
        "GET / HTTP/1.1\r\nHost : x\r\n\r\n",                     // space before ':'
        "GET / HTTP/1.1\r\nA: 1\r\n folded\r\n\r\n",              // obsolete line folding
        "GET / HTTP/1.1\r\nA: bad\x01value\r\n\r\n",              // control char in value
        "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n",         // non-numeric length
        "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\n",          // negative length
        "POST / HTTP/1.1\r\nContent-Length: 5x\r\n\r\n",          // trailing junk
        "POST / HTTP/1.1\r\nContent-Length: \r\n\r\n",            // empty length
        "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n",   // overflow
        "POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd"  // conflicting
        ));

TEST(HttpParser, AcceptsRepeatedIdenticalContentLength) {
    HttpParser parser;
    EXPECT_EQ(parser.feed("POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nhi"),
              ParseStatus::Complete);
}

TEST(HttpParser, RejectsChunkedRequestBodiesWith501) {
    HttpParser parser;
    EXPECT_EQ(parser.feed("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"),
              ParseStatus::Error);
    EXPECT_EQ(parser.errorStatus(), 501);
}

TEST(HttpParser, ErrorIsSticky) {
    HttpParser parser;
    ASSERT_EQ(parser.feed("GARBAGE\r\n\r\n"), ParseStatus::Error);
    EXPECT_EQ(parser.feed("GET / HTTP/1.1\r\n\r\n"), ParseStatus::Error);
}

// ----------------------------- Requests: size caps -------------------------

TEST(HttpParser, RejectsOversizedHeadersWith431) {
    HttpParser::Limits limits;
    limits.maxHeaderBytes = 64;
    HttpParser parser(limits);

    // No blank line yet, but already over the cap: must not wait for more.
    const std::string big = "GET / HTTP/1.1\r\nX-Padding: " + std::string(100, 'a');
    EXPECT_EQ(parser.feed(big), ParseStatus::Error);
    EXPECT_EQ(parser.errorStatus(), 431);
}

TEST(HttpParser, RejectsOversizedCompleteHeadersWith431) {
    HttpParser::Limits limits;
    limits.maxHeaderBytes = 64;
    HttpParser parser(limits);

    const std::string big = "GET / HTTP/1.1\r\nX-Padding: " + std::string(100, 'a') + "\r\n\r\n";
    EXPECT_EQ(parser.feed(big), ParseStatus::Error);
    EXPECT_EQ(parser.errorStatus(), 431);
}

TEST(HttpParser, RejectsOversizedBodyWith413BeforeReadingIt) {
    HttpParser::Limits limits;
    limits.maxBodyBytes = 10;
    HttpParser parser(limits);

    // Only the headers are sent; the declared length alone is enough to refuse.
    EXPECT_EQ(parser.feed("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n"), ParseStatus::Error);
    EXPECT_EQ(parser.errorStatus(), 413);
}

TEST(HttpParser, AcceptsBodyExactlyAtTheLimit) {
    HttpParser::Limits limits;
    limits.maxBodyBytes = 10;
    HttpParser parser(limits);
    EXPECT_EQ(parser.feed("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n0123456789"),
              ParseStatus::Complete);
}

// --------------------------------- Responses -------------------------------

TEST(HttpResponseParser, ParsesContentLengthResponse) {
    HttpResponseParser parser;
    ASSERT_EQ(parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Test: yes\r\n\r\nhello"),
              ParseStatus::Complete);
    EXPECT_EQ(parser.response().status, 200);
    EXPECT_EQ(parser.response().reason, "OK");
    EXPECT_EQ(parser.response().body, "hello");
    ASSERT_NE(parser.response().header("x-test"), nullptr);
    EXPECT_EQ(*parser.response().header("x-test"), "yes");
}

TEST(HttpResponseParser, HandlesOneByteAtATime) {
    const std::string wire = "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nabc";
    HttpResponseParser parser;
    ParseStatus status = ParseStatus::NeedMore;
    for (const char c : wire) {
        ASSERT_EQ(status, ParseStatus::NeedMore);
        status = parser.feed(std::string_view(&c, 1));
    }
    ASSERT_EQ(status, ParseStatus::Complete);
    EXPECT_EQ(parser.response().status, 404);
    EXPECT_EQ(parser.response().reason, "Not Found");
    EXPECT_EQ(parser.response().body, "abc");
}

TEST(HttpResponseParser, BodyWithoutLengthEndsWhenConnectionCloses) {
    HttpResponseParser parser;
    EXPECT_EQ(parser.feed("HTTP/1.0 200 OK\r\n\r\npart one, "), ParseStatus::NeedMore);
    EXPECT_EQ(parser.feed("part two"), ParseStatus::NeedMore);
    EXPECT_EQ(parser.finish(), ParseStatus::Complete);
    EXPECT_EQ(parser.response().body, "part one, part two");
}

TEST(HttpResponseParser, ChunkedBodyIsPassedThroughUntouched) {
    const std::string chunked = "5\r\nhello\r\n0\r\n\r\n";
    HttpResponseParser parser;
    EXPECT_EQ(parser.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunked),
              ParseStatus::NeedMore);
    EXPECT_EQ(parser.finish(), ParseStatus::Complete);
    EXPECT_EQ(parser.response().body, chunked);
    EXPECT_NE(parser.response().header("Transfer-Encoding"), nullptr);
}

TEST(HttpResponseParser, TruncatedBodyIsAnError) {
    HttpResponseParser parser;
    EXPECT_EQ(parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"),
              ParseStatus::NeedMore);
    EXPECT_EQ(parser.finish(), ParseStatus::Error);
}

TEST(HttpResponseParser, ClosingBeforeTheHeadIsAnError) {
    HttpResponseParser empty;
    EXPECT_EQ(empty.finish(), ParseStatus::Error);

    HttpResponseParser partial;
    EXPECT_EQ(partial.feed("HTTP/1.1 200 OK\r\nContent-Le"), ParseStatus::NeedMore);
    EXPECT_EQ(partial.finish(), ParseStatus::Error);
}

TEST(HttpResponseParser, HeadResponseHasNoBodyDespiteContentLength) {
    HttpResponseParser parser(1024, /*headRequest=*/true);
    EXPECT_EQ(parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n"), ParseStatus::Complete);
    EXPECT_TRUE(parser.response().body.empty());
}

TEST(HttpResponseParser, Status204And304HaveNoBody) {
    HttpResponseParser noContent;
    EXPECT_EQ(noContent.feed("HTTP/1.1 204 No Content\r\n\r\n"), ParseStatus::Complete);

    HttpResponseParser notModified;
    EXPECT_EQ(notModified.feed("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n"),
              ParseStatus::Complete);
}

TEST(HttpResponseParser, RejectsInvalidStatusLines) {
    for (const char* wire : {
             "garbage\r\n\r\n",
             "HTTP/2 200 OK\r\n\r\n",
             "HTTP/1.1 abc OK\r\n\r\n",
             "HTTP/1.1 20 OK\r\n\r\n",
             "HTTP/1.1 2000 OK\r\n\r\n",
             "HTTP/1.1 099 Low\r\n\r\n",
             "HTTP/1.1\r\n\r\n",
         }) {
        HttpResponseParser parser;
        EXPECT_EQ(parser.feed(wire), ParseStatus::Error) << wire;
        EXPECT_FALSE(parser.errorMessage().empty());
    }
}

TEST(HttpResponseParser, AcceptsStatusLineWithoutReason) {
    HttpResponseParser parser;
    EXPECT_EQ(parser.feed("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n"), ParseStatus::Complete);
    EXPECT_EQ(parser.response().status, 200);
    EXPECT_EQ(parser.response().reason, "");
}

TEST(HttpResponseParser, EnforcesSizeCap) {
    // Declared length over the cap: refused as soon as the headers are in.
    HttpResponseParser declared(100);
    EXPECT_EQ(declared.feed("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n"), ParseStatus::Error);

    // No declared length: refused once the received bytes exceed the cap.
    HttpResponseParser streamed(100);
    EXPECT_EQ(streamed.feed("HTTP/1.1 200 OK\r\n\r\n"), ParseStatus::NeedMore);
    EXPECT_EQ(streamed.feed(std::string(200, 'x')), ParseStatus::Error);
}

TEST(HttpResponse, MakeBuildsCompleteResponse) {
    const minilb::HttpResponse response = minilb::HttpResponse::make(503, "down\n");
    EXPECT_EQ(response.serialize(),
              "HTTP/1.1 503 Service Unavailable\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: 5\r\n"
              "Connection: close\r\n"
              "\r\n"
              "down\n");
}

TEST(Headers, SetReplacesAllAndRemoveHopByHop) {
    minilb::HeaderList headers = {{"Connection", "keep-alive"},
                                  {"X-A", "1"},
                                  {"x-a", "2"},
                                  {"Keep-Alive", "timeout=5"},
                                  {"Host", "h"}};
    minilb::setHeader(headers, "X-A", "3");
    minilb::removeHopByHopHeaders(headers);

    ASSERT_EQ(headers.size(), 2u);
    EXPECT_EQ(*minilb::findHeader(headers, "x-a"), "3");
    EXPECT_EQ(*minilb::findHeader(headers, "host"), "h");
    EXPECT_EQ(minilb::findHeader(headers, "connection"), nullptr);
}
