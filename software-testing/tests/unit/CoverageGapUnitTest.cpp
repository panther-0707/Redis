// Coverage-gap unit tests.
//
// Technique: structural (line and branch) coverage used as a *gap finder*.
// Each test targets code that the original unit suite never executed (found
// with gcovr, see software-testing/scripts/coverage.sh).

#include "miniLB/balancer/BackendPool.hpp"
#include "miniLB/http/HttpParser.hpp"
#include "miniLB/http/HttpResponse.hpp"
#include "miniLB/util/Config.hpp"
#include "miniLB/util/Metrics.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <unistd.h>

using minilb::Config;
using minilb::HttpResponseParser;
using minilb::ParseStatus;

namespace {

const char* const kOneBackend = "backends:\n  - { host: 127.0.0.1, port: 8001 }\n";

// Writes `yaml` to a temporary file and loads it.
Config loadYaml(const std::string& yaml) {
    char path[] = "/tmp/minilb-config-XXXXXX";
    const int fd = mkstemp(path);
    EXPECT_NE(fd, -1);
    close(fd);
    {
        std::ofstream file(path);
        file << yaml;
    }
    try {
        Config config = Config::load(path);
        std::remove(path);
        return config;
    } catch (...) {
        std::remove(path);
        throw;
    }
}

// Expects loading `yaml` to fail with a message containing `fragment`.
void expectError(const std::string& yaml, const std::string& fragment) {
    try {
        loadYaml(yaml);
        ADD_FAILURE() << "expected an error containing '" << fragment << "'";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos)
            << "actual message: " << e.what();
    }
}

} // namespace

// ================================ Config ====================================

// Config.cpp: the whole `limits:` block was never loaded by any test.
TEST(ConfigGaps, ReadsTheLimitsBlock) {
    const Config config = loadYaml(std::string(kOneBackend) +
                                   "limits:\n"
                                   "  max_request_header_bytes: 2048\n"
                                   "  max_request_body_bytes: 4096\n"
                                   "  max_response_bytes: 8192\n");
    EXPECT_EQ(config.limits.maxRequestHeaderBytes, 2048u);
    EXPECT_EQ(config.limits.maxRequestBodyBytes, 4096u);
    EXPECT_EQ(config.limits.maxResponseBytes, 8192u);
}

// Boundary values of the limits: 1 is the smallest accepted size, 0 is
// rejected (a body cap of 0 is allowed: it means "no request bodies").
TEST(ConfigGaps, LimitsBoundaries) {
    const Config smallest = loadYaml(std::string(kOneBackend) +
                                     "limits: { max_request_header_bytes: 1, "
                                     "max_request_body_bytes: 0, max_response_bytes: 1 }\n");
    EXPECT_EQ(smallest.limits.maxRequestHeaderBytes, 1u);
    EXPECT_EQ(smallest.limits.maxRequestBodyBytes, 0u);
    EXPECT_EQ(smallest.limits.maxResponseBytes, 1u);

    expectError(std::string(kOneBackend) + "limits: { max_request_header_bytes: 0 }\n",
                "'limits.max_request_header_bytes' must be at least 1");
    expectError(std::string(kOneBackend) + "limits: { max_response_bytes: 0 }\n",
                "'limits.max_response_bytes' must be at least 1");
    expectError(std::string(kOneBackend) + "limits: { max_body: 5 }\n",
                "unknown key 'max_body' in limits");
}

// Config.cpp: the "must not be negative" check was never reached.
// Boundary: -1 is rejected, 0 ("do not wait at all") is accepted.
TEST(ConfigGaps, ShutdownGraceBoundaries) {
    expectError(std::string(kOneBackend) + "shutdown_grace_ms: -1\n",
                "'shutdown_grace_ms' must not be negative");
    EXPECT_EQ(loadYaml(std::string(kOneBackend) + "shutdown_grace_ms: 0\n").shutdownGraceMs, 0);
    EXPECT_EQ(loadYaml(std::string(kOneBackend) + "shutdown_grace_ms: 250\n").shutdownGraceMs, 250);
}

// Metrics.cpp: jsonEscape() only ever saw plain text, so its escaping
// branches never ran. A backend id comes from the config file, so it can
// contain anything; /stats must stay valid JSON regardless.
TEST(MetricsGaps, JsonEscapesQuotesBackslashesAndControlCharacters) {
    minilb::BackendPool pool;
    pool.add("we\"ird\\host\n", 80, 1);   // id: we"ird\host<newline>:80

    const minilb::Metrics metrics;
    const std::string json = metrics.toJson(pool, "al\"go\tname", true);

    // Quote and backslash get a backslash in front; control characters
    // (newline, tab) become a space.
    EXPECT_NE(json.find(R"("id": "we\"ird\\host :80")"), std::string::npos) << json;
    EXPECT_NE(json.find(R"("algorithm": "al\"go name")"), std::string::npos) << json;

    // No raw control character survives inside the id (the document's own
    // line breaks are fine, so only look at the id's line).
    const std::size_t idAt = json.find("\"id\"");
    ASSERT_NE(idAt, std::string::npos);
    const std::string idLine = json.substr(idAt, json.find('\n', idAt) - idAt);
    EXPECT_EQ(idLine.find('\t'), std::string::npos);
}

// ============================ Response parser ===============================

// HttpParser.cpp: feed() after the parser has finished returned early on a
// line no test reached. The request parser had this test ("ErrorIsSticky",
// "IgnoresBytesAfterTheFirstRequest"); the response parser did not.
TEST(HttpResponseParserGaps, IgnoresBytesAfterACompleteResponse) {
    HttpResponseParser parser;
    ASSERT_EQ(parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), ParseStatus::Complete);
    EXPECT_EQ(parser.feed("HTTP/1.1 500 Oops\r\n\r\n"), ParseStatus::Complete);
    EXPECT_EQ(parser.response().status, 200);
    EXPECT_EQ(parser.response().body, "ok");
}

TEST(HttpResponseParserGaps, ErrorIsSticky) {
    HttpResponseParser parser;
    ASSERT_EQ(parser.feed("garbage\r\n\r\n"), ParseStatus::Error);
    EXPECT_EQ(parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"), ParseStatus::Error);
    EXPECT_EQ(parser.finish(), ParseStatus::Error);
}

// HttpParser.cpp: a valid status line followed by broken *headers* was never
// tried (RejectsInvalidStatusLines only breaks the first line).
TEST(HttpResponseParserGaps, RejectsMalformedHeaderLines) {
    for (const char* wire : {
             "HTTP/1.1 200 OK\r\nNoColonHere\r\n\r\n",
             "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n",
             "HTTP/1.1 200 OK\r\nA: 1\r\n folded\r\n\r\n",
         }) {
        HttpResponseParser parser;
        EXPECT_EQ(parser.feed(wire), ParseStatus::Error) << wire;
        EXPECT_FALSE(parser.errorMessage().empty()) << wire;
    }
}

// HttpParser.cpp: the response-side Content-Length checks never failed in a
// test. Equivalence classes of a bad length: not a number, negative, two
// different values.
TEST(HttpResponseParserGaps, RejectsInvalidOrConflictingContentLength) {
    for (const char* wire : {
             "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc",
         }) {
        HttpResponseParser parser;
        EXPECT_EQ(parser.feed(wire), ParseStatus::Error) << wire;
        EXPECT_NE(parser.errorMessage().find("Content-Length"), std::string::npos)
            << wire << " -> " << parser.errorMessage();
    }
}

// HttpResponse.cpp: reasonPhrase() has eleven cases; only 503 was checked and
// the default branch never ran. One row per case.
TEST(HttpResponseGaps, ReasonPhraseForEveryStatusMiniLbGenerates) {
    const std::pair<int, const char*> expected[] = {
        {200, "OK"},
        {400, "Bad Request"},
        {404, "Not Found"},
        {413, "Content Too Large"},
        {429, "Too Many Requests"},
        {431, "Request Header Fields Too Large"},
        {501, "Not Implemented"},
        {502, "Bad Gateway"},
        {503, "Service Unavailable"},
        {504, "Gateway Timeout"},
        {418, "Unknown"},   // anything else
    };
    for (const auto& [status, reason] : expected) {
        EXPECT_EQ(minilb::HttpResponse::reasonPhrase(status), reason) << status;
        EXPECT_EQ(minilb::HttpResponse::make(status, "").reason, reason) << status;
    }
}
