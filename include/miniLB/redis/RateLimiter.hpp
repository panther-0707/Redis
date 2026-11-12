#pragma once

#include "miniLB/redis/RedisClient.hpp"

#include <cstdint>
#include <string>

namespace minilb {

// Fixed-window rate limiter: at most `maxRequests` per client IP in each
// window of `windowSeconds`.
//
// Time is cut into windows (window number = now / windowSeconds). Each
// (ip, window) pair has a counter in Redis under "ratelimit:<ip>:<window>".
// A request does INCR on the current counter and is rejected if the result
// exceeds the limit. EXPIRE lets Redis delete old counters by itself.
//
// Because the count lives in Redis, several miniLB instances share one limit.
//
// Trade-off of a fixed window: a client can send `maxRequests` at the very
// end of one window and again at the start of the next, i.e. up to twice the
// limit in a short burst. Sliding windows or a token bucket avoid that.
//
// Fail-open: if Redis is unavailable every request is allowed.
// allow() is BLOCKING (one Redis round trip); do not call it on an
// io_context thread. Thread-safe (no mutable state of its own).
class RateLimiter {
public:
    struct Decision {
        bool allowed = true;
        int retryAfterSeconds = 0;   // when rejected: time until the next window
    };

    // maxRequests <= 0 disables rate limiting.
    RateLimiter(KeyValueStore& store, int maxRequests, int windowSeconds);

    bool enabled() const { return maxRequests_ > 0; }

    // Counts one request from `clientIp` and says whether it may proceed.
    Decision allow(const std::string& clientIp);

    // Same, with the current Unix time passed in (lets tests control time).
    Decision allow(const std::string& clientIp, std::int64_t nowSeconds);

    // "ratelimit:<ip>:<window>"
    static std::string makeKey(const std::string& clientIp, std::int64_t window);

private:
    KeyValueStore& store_;
    int maxRequests_;
    int windowSeconds_;
};

} // namespace minilb
