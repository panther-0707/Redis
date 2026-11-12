#pragma once

#include "miniLB/http/HttpRequest.hpp"
#include "miniLB/http/HttpResponse.hpp"
#include "miniLB/redis/RedisClient.hpp"
#include "miniLB/util/Config.hpp"

#include <optional>
#include <string>

namespace minilb {

// Response cache stored in Redis.
//
//   key   = "cache:<method>:<host>:<path>?<query>"
//   value = the backend's response in wire format (HttpResponse::serialize)
//   TTL   = cache.ttl_seconds; Redis deletes expired entries by itself
//
// What is cached is decided conservatively (see the two isCacheable*
// functions) so that one user's personalised page is never served to
// another user.
//
// get() and put() are BLOCKING (one Redis round trip); do not call them on
// an io_context thread. Thread-safe (no mutable state of its own). If Redis
// is unavailable, get() reports a miss and put() does nothing.
class Cache {
public:
    Cache(KeyValueStore& store, const Config::Cache& settings);

    bool enabled() const { return settings_.enabled; }

    // Only plain GETs without credentials are looked up in the cache.
    static bool isCacheableRequest(const HttpRequest& request);

    // Only successful, shareable, reasonably small responses are stored.
    bool isCacheableResponse(const HttpResponse& response) const;

    static std::string makeKey(const HttpRequest& request);

    std::optional<HttpResponse> get(const std::string& key);
    void put(const std::string& key, const HttpResponse& response);

private:
    KeyValueStore& store_;
    Config::Cache settings_;
};

} // namespace minilb
