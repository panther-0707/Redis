// Tests written in response to surviving mutants.
//
// Technique: mutation testing as a test-improvement loop. The first full
// mutation run (software-testing/docs/04-mutation-report.md, 24 survivors)
// pointed at behaviour that no test pinned down. Each test below names the
// survivor it was written for; all of them are killed in the second run.
//
// Survivors that are *equivalent* (the change cannot be observed) are not
// "fixed" here: no test can kill them. They are listed in the report.

#include "miniLB/balancer/BackendPool.hpp"
#include "miniLB/http/Headers.hpp"
#include "miniLB/http/HttpParser.hpp"
#include "miniLB/util/Metrics.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <string_view>

using minilb::HttpParser;
using minilb::HttpResponseParser;
using minilb::ParseStatus;

namespace {

// ---- A minimal JSON syntax checker -------------------------------------------
//
// The Metrics tests only searched the /stats output for substrings, so a
// document that is not JSON at all (a trailing comma, survivor at
// Metrics.cpp "i + 1 < backends.size()") still passed. Requirement F11 says
// "valid JSON", so the oracle has to check exactly that.
class JsonChecker {
public:
    explicit JsonChecker(std::string_view text) : text_(text) {}

    bool valid() {
        skipSpace();
        if (!value()) {
            return false;
        }
        skipSpace();
        return position_ == text_.size();
    }

private:
    void skipSpace() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
    }
    bool eat(char c) {
        if (position_ < text_.size() && text_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }
    bool literal(std::string_view word) {
        if (text_.substr(position_, word.size()) == word) {
            position_ += word.size();
            return true;
        }
        return false;
    }
    bool string() {
        if (!eat('"')) {
            return false;
        }
        while (position_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') {
                return true;
            }
            if (c < 0x20) {
                return false;   // raw control character inside a string
            }
            if (c == '\\') {
                if (position_ >= text_.size() ||
                    std::string_view("\"\\/bfnrtu").find(text_[position_]) == std::string_view::npos) {
                    return false;
                }
                ++position_;
            }
        }
        return false;   // unterminated
    }
    bool number() {
        const std::size_t start = position_;
        eat('-');
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        return position_ > start && text_[position_ - 1] != '-';
    }
    bool members(char close, bool isObject) {
        skipSpace();
        if (eat(close)) {
            return true;   // empty object or array
        }
        for (;;) {
            skipSpace();
            if (isObject) {
                if (!string()) {
                    return false;
                }
                skipSpace();
                if (!eat(':')) {
                    return false;
                }
                skipSpace();
            }
            if (!value()) {
                return false;
            }
            skipSpace();
            if (eat(close)) {
                return true;
            }
            if (!eat(',')) {
                return false;
            }
        }
    }
    bool value() {
        if (eat('{')) {
            return members('}', true);
        }
        if (eat('[')) {
            return members(']', false);
        }
        if (position_ < text_.size() && text_[position_] == '"') {
            return string();
        }
        return literal("true") || literal("false") || literal("null") || number();
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

bool isValidJson(std::string_view text) {
    return JsonChecker(text).valid();
}

} // namespace

// The checker itself must reject what it is there to catch.
TEST(MutationKill, JsonCheckerAcceptsJsonAndRejectsNearJson) {
    EXPECT_TRUE(isValidJson(R"({"a": 1, "b": [true, false, null], "c": {"d": "x\"y"}})"));
    EXPECT_TRUE(isValidJson("[]"));
    EXPECT_FALSE(isValidJson(R"({"a": 1,})")) << "trailing comma in an object";
    EXPECT_FALSE(isValidJson(R"([1, 2,])")) << "trailing comma in an array";
    EXPECT_FALSE(isValidJson(R"({"a": 1} x)")) << "text after the document";
    EXPECT_FALSE(isValidJson("{\"a\": \"line\nbreak\"}")) << "raw control character in a string";
    EXPECT_FALSE(isValidJson(R"({"a" 1})")) << "missing colon";
}

// Survivor: Metrics.cpp, `i + 1 < backends.size()` -> `<=` (a comma after the
// last backend). Checked for 0, 1 and 3 backends: the separator logic has a
// different shape in each.
TEST(MutationKill, StatsIsValidJsonForAnyNumberOfBackends) {
    for (const int count : {0, 1, 3}) {
        minilb::BackendPool pool;
        for (int i = 0; i < count; ++i) {
            pool.add("10.0.0." + std::to_string(i + 1), 8000, 1);
        }
        const minilb::Metrics metrics;
        const std::string json = metrics.toJson(pool, "round_robin", true);
        EXPECT_TRUE(isValidJson(json)) << count << " backends:\n" << json;
    }
}

// The same oracle for the escaping test: a backend id with quotes,
// backslashes and control characters must still give valid JSON.
TEST(MutationKill, StatsIsValidJsonEvenWithHostileBackendNames) {
    minilb::BackendPool pool;
    pool.add("we\"ird\\host\n\t", 80, 1);
    const minilb::Metrics metrics;
    const std::string json = metrics.toJson(pool, "al\"go", false);
    EXPECT_TRUE(isValidJson(json)) << json;
}

// Survivor: Metrics.cpp, `redisAvailable ? "true" : "false"` -> always
// "false". No unit test looked at the value.
TEST(MutationKill, StatsReportsWhetherRedisIsAvailable) {
    const minilb::BackendPool pool;
    const minilb::Metrics metrics;
    EXPECT_NE(metrics.toJson(pool, "round_robin", true).find("\"redis_available\": true"), std::string::npos);
    EXPECT_NE(metrics.toJson(pool, "round_robin", false).find("\"redis_available\": false"), std::string::npos);
}

// Survivors: HttpParser.cpp isTokenChar(), `<= 'Z'` -> `< 'Z'`, `<= '9'` ->
// `< '9'`, `>= '0'` -> `> '0'`. No test used a header name containing the
// last letter or the first or last digit. Both ends of each range, and the
// characters just outside them, are covered here.
TEST(MutationKill, HeaderNamesMayUseTheWholeLetterAndDigitRanges) {
    for (const char* name : {"a", "z", "A", "Z", "0", "9", "X-az-AZ-09"}) {
        HttpParser parser;
        ASSERT_EQ(parser.feed(std::string("GET / HTTP/1.1\r\n") + name + ": v\r\n\r\n"), ParseStatus::Complete)
            << "header name '" << name << "'";
        EXPECT_NE(parser.request().header(name), nullptr) << name;
    }
    // Neighbours of the ranges in ASCII that are not token characters:
    // '@' (before 'A'), '[' (after 'Z'), '/' (before '0'), ':' handled as the
    // separator, '{' (after 'z').
    for (const char* name : {"@", "[", "/", "{", "a@b", "a[b"}) {
        HttpParser parser;
        EXPECT_EQ(parser.feed(std::string("GET / HTTP/1.1\r\n") + name + ": v\r\n\r\n"), ParseStatus::Error)
            << "header name '" << name << "'";
        EXPECT_EQ(parser.errorStatus(), 400) << name;
    }
}

// Survivor: HttpParser.cpp, method check `c > 'Z'` -> `c >= 'Z'`. No test
// used a method containing 'Z' (or 'A' on its own).
TEST(MutationKill, MethodsMayUseTheWholeUpperCaseRange) {
    for (const char* method : {"A", "Z", "AZ", "ZAP"}) {
        HttpParser parser;
        ASSERT_EQ(parser.feed(std::string(method) + " / HTTP/1.1\r\n\r\n"), ParseStatus::Complete) << method;
        EXPECT_EQ(parser.request().method, method);
    }
    for (const char* method : {"@", "[", "A@", "Z["}) {   // just outside 'A'..'Z'
        HttpParser parser;
        EXPECT_EQ(parser.feed(std::string(method) + " / HTTP/1.1\r\n\r\n"), ParseStatus::Error) << method;
    }
}

// Survivor: HttpParser.cpp (response), `buffer_.size() > maxBytes_` -> `>=`.
// A response whose total size is exactly the cap, with no Content-Length,
// must still be accepted; one byte more must not.
TEST(MutationKill, StreamedResponseOfExactlyTheCapIsAcceptedAndOneByteMoreIsNot) {
    const std::string head = "HTTP/1.1 200 OK\r\n\r\n";
    const std::size_t cap = head.size() + 30;

    HttpResponseParser atCap(cap);
    EXPECT_EQ(atCap.feed(head + std::string(30, 'x')), ParseStatus::NeedMore) << "exactly the cap";
    EXPECT_EQ(atCap.finish(), ParseStatus::Complete);
    EXPECT_EQ(atCap.response().body.size(), 30u);

    HttpResponseParser over(cap);
    EXPECT_EQ(over.feed(head + std::string(31, 'x')), ParseStatus::Error) << "one byte over the cap";
}

// Survivor: HttpParser.cpp (response), `status >= 100` -> `status > 100`.
// Status 100 is the lower edge of "1xx responses never have a body".
TEST(MutationKill, InformationalResponsesHaveNoBodyFrom100To199) {
    for (const char* code : {"100", "101", "199"}) {
        HttpResponseParser parser;
        EXPECT_EQ(parser.feed(std::string("HTTP/1.1 ") + code + " Info\r\n\r\n"), ParseStatus::Complete)
            << code << " is complete at the end of its headers";
        EXPECT_EQ(parser.response().body, "");
    }
    // 200 is the first status that may have a body: without a length it runs
    // until the connection closes.
    HttpResponseParser ok;
    EXPECT_EQ(ok.feed("HTTP/1.1 200 OK\r\n\r\n"), ParseStatus::NeedMore);
}

// Survivor: Headers.cpp iequals(), `i < a.size()` -> `i <= a.size()`, which
// compares one character past the end. It survived because every caller
// passed whole strings (the extra character was the terminating zero on
// both sides). Views into the middle of a buffer expose it.
TEST(MutationKill, IequalsComparesOnlyTheCharactersInsideTheViews) {
    const std::string left = "Host: a";
    const std::string right = "hOST=b";
    EXPECT_TRUE(minilb::iequals(std::string_view(left).substr(0, 4), std::string_view(right).substr(0, 4)))
        << "what follows the views differs, the views themselves do not";
    EXPECT_FALSE(minilb::iequals(std::string_view(left).substr(0, 5), std::string_view(right).substr(0, 5)));
    EXPECT_TRUE(minilb::iequals("", ""));
    EXPECT_FALSE(minilb::iequals("a", ""));
}
