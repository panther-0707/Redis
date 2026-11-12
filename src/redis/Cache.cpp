#include "miniLB/redis/Cache.hpp"

#include "miniLB/http/HttpParser.hpp"

#include <algorithm>
#include <cctype>

namespace minilb {

namespace {

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

Cache::Cache(KeyValueStore& store, const Config::Cache& settings)
    : store_(store), settings_(settings) {}

bool Cache::isCacheableRequest(const HttpRequest& request) {
    if (request.method != "GET") {
        return false;
    }
    // Requests carrying credentials usually get a personalised answer.
    return request.header("Authorization") == nullptr && request.header("Cookie") == nullptr;
}

bool Cache::isCacheableResponse(const HttpResponse& response) const {
    if (response.status != 200) {
        return false;
    }
    // Set-Cookie is meant for one user. Vary means the response depends on
    // request headers that are not part of our cache key.
    if (response.header("Set-Cookie") != nullptr || response.header("Vary") != nullptr) {
        return false;
    }
    // Respect the backend's wish not to be cached.
    if (const std::string* cacheControl = response.header("Cache-Control")) {
        const std::string value = toLower(*cacheControl);
        for (const char* directive : {"no-store", "no-cache", "private"}) {
            if (value.find(directive) != std::string::npos) {
                return false;
            }
        }
    }
    return response.body.size() <= settings_.maxEntryBytes;
}

std::string Cache::makeKey(const HttpRequest& request) {
    // Host names are case-insensitive, so normalise them.
    const std::string* host = request.header("Host");

    std::string key = "cache:" + request.method + ":" + (host ? toLower(*host) : "-") + ":" +
                      request.path;
    if (!request.query.empty()) {
        key += "?" + request.query;
    }
    return key;
}

std::optional<HttpResponse> Cache::get(const std::string& key) {
    const std::optional<std::string> stored = store_.get(key);
    if (!stored) {
        return std::nullopt;   // miss, or Redis unavailable
    }

    // Turn the stored bytes back into an HttpResponse. finish() tells the
    // parser that there are no more bytes coming.
    HttpResponseParser parser(stored->size() + 1);
    parser.feed(*stored);
    if (parser.finish() != ParseStatus::Complete) {
        return std::nullopt;   // corrupt entry: treat as a miss
    }
    return std::move(parser.response());
}

void Cache::put(const std::string& key, const HttpResponse& response) {
    store_.setWithTtl(key, response.serialize(), settings_.ttlSeconds);
}

} // namespace minilb
