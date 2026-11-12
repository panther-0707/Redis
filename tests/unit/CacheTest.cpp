// Unit tests for the response cache (against an in-memory store).

#include "FakeStore.hpp"
#include "miniLB/http/HttpParser.hpp"
#include "miniLB/redis/Cache.hpp"

#include <gtest/gtest.h>

using namespace minilb;

namespace {

HttpRequest parseRequest(const std::string& wire) {
    HttpParser parser;
    EXPECT_EQ(parser.feed(wire), ParseStatus::Complete);
    return parser.request();
}

Config::Cache settings(int ttlSeconds = 30, std::size_t maxEntryBytes = 1024) {
    Config::Cache cache;
    cache.enabled = true;
    cache.ttlSeconds = ttlSeconds;
    cache.maxEntryBytes = maxEntryBytes;
    return cache;
}

} // namespace

TEST(Cache, KeyContainsMethodHostPathAndQuery) {
    EXPECT_EQ(Cache::makeKey(parseRequest("GET /a/b?x=1&y=2 HTTP/1.1\r\nHost: Example.COM\r\n\r\n")),
              "cache:GET:example.com:/a/b?x=1&y=2");
    EXPECT_EQ(Cache::makeKey(parseRequest("GET /plain HTTP/1.1\r\nHost: h:8080\r\n\r\n")),
              "cache:GET:h:8080:/plain");
    EXPECT_EQ(Cache::makeKey(parseRequest("GET / HTTP/1.0\r\n\r\n")), "cache:GET:-:/");
}

TEST(Cache, DifferentQueriesGetDifferentKeys) {
    const auto one = Cache::makeKey(parseRequest("GET /p?id=1 HTTP/1.1\r\nHost: h\r\n\r\n"));
    const auto two = Cache::makeKey(parseRequest("GET /p?id=2 HTTP/1.1\r\nHost: h\r\n\r\n"));
    EXPECT_NE(one, two);
}

TEST(Cache, OnlyPlainGetRequestsAreCacheable) {
    EXPECT_TRUE(Cache::isCacheableRequest(parseRequest("GET / HTTP/1.1\r\nHost: h\r\n\r\n")));

    EXPECT_FALSE(Cache::isCacheableRequest(
        parseRequest("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n")));
    EXPECT_FALSE(Cache::isCacheableRequest(parseRequest("HEAD / HTTP/1.1\r\n\r\n")));
    EXPECT_FALSE(Cache::isCacheableRequest(
        parseRequest("GET / HTTP/1.1\r\nAuthorization: Bearer token\r\n\r\n")));
    EXPECT_FALSE(
        Cache::isCacheableRequest(parseRequest("GET / HTTP/1.1\r\nCookie: session=1\r\n\r\n")));
}

TEST(Cache, OnlyShareable200ResponsesAreCacheable) {
    FakeStore store;
    Cache cache(store, settings(30, /*maxEntryBytes=*/10));

    EXPECT_TRUE(cache.isCacheableResponse(HttpResponse::make(200, "ok")));
    EXPECT_FALSE(cache.isCacheableResponse(HttpResponse::make(404, "no")));
    EXPECT_FALSE(cache.isCacheableResponse(HttpResponse::make(502, "bad")));

    HttpResponse cookie = HttpResponse::make(200, "ok");
    cookie.setHeader("Set-Cookie", "session=abc");
    EXPECT_FALSE(cache.isCacheableResponse(cookie));

    HttpResponse vary = HttpResponse::make(200, "ok");
    vary.setHeader("Vary", "Accept-Language");
    EXPECT_FALSE(cache.isCacheableResponse(vary));

    for (const char* directive : {"no-store", "private, max-age=60", "No-Cache"}) {
        HttpResponse response = HttpResponse::make(200, "ok");
        response.setHeader("Cache-Control", directive);
        EXPECT_FALSE(cache.isCacheableResponse(response)) << directive;
    }

    HttpResponse publicResponse = HttpResponse::make(200, "ok");
    publicResponse.setHeader("Cache-Control", "public, max-age=60");
    EXPECT_TRUE(cache.isCacheableResponse(publicResponse));

    EXPECT_TRUE(cache.isCacheableResponse(HttpResponse::make(200, "0123456789")));
    EXPECT_FALSE(cache.isCacheableResponse(HttpResponse::make(200, "0123456789+")))
        << "body larger than max_entry_bytes";
}

TEST(Cache, PutThenGetReturnsTheSameResponse) {
    FakeStore store;
    Cache cache(store, settings(/*ttlSeconds=*/45));

    HttpResponse original = HttpResponse::make(200, "Hello from backend-1!\n");
    original.setHeader("X-Custom", "abc");
    cache.put("cache:GET:h:/", original);

    EXPECT_EQ(store.ttls["cache:GET:h:/"], 45);
    EXPECT_EQ(store.values["cache:GET:h:/"], original.serialize());

    const std::optional<HttpResponse> cached = cache.get("cache:GET:h:/");
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->status, 200);
    EXPECT_EQ(cached->body, "Hello from backend-1!\n");
    ASSERT_NE(cached->header("X-Custom"), nullptr);
    EXPECT_EQ(*cached->header("X-Custom"), "abc");
    EXPECT_EQ(cached->serialize(), original.serialize());
}

TEST(Cache, ChunkedResponseSurvivesTheRoundTrip) {
    FakeStore store;
    Cache cache(store, settings());

    HttpResponse original;
    original.setHeader("Transfer-Encoding", "chunked");
    original.body = "5\r\nhello\r\n0\r\n\r\n";
    cache.put("k", original);

    const auto cached = cache.get("k");
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->body, original.body);
}

TEST(Cache, MissingKeyIsAMiss) {
    FakeStore store;
    Cache cache(store, settings());
    EXPECT_FALSE(cache.get("cache:GET:h:/nothing").has_value());
}

TEST(Cache, CorruptEntryIsTreatedAsAMiss) {
    FakeStore store;
    store.values["k"] = "this is not an HTTP response";
    Cache cache(store, settings());
    EXPECT_FALSE(cache.get("k").has_value());
}

TEST(Cache, UnavailableStoreMeansMissAndNoCrash) {
    FakeStore store;
    Cache cache(store, settings());
    cache.put("k", HttpResponse::make(200, "ok"));

    store.available = false;
    EXPECT_FALSE(cache.get("k").has_value());
    cache.put("other", HttpResponse::make(200, "ok"));   // must simply do nothing
    EXPECT_EQ(store.values.count("other"), 0u);
}

TEST(Cache, EnabledFollowsTheSettings) {
    FakeStore store;
    Config::Cache off;
    off.enabled = false;
    EXPECT_FALSE(Cache(store, off).enabled());
    EXPECT_TRUE(Cache(store, settings()).enabled());
}
