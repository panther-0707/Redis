#include "miniLB/redis/RateLimiter.hpp"

#include <chrono>

namespace minilb {

RateLimiter::RateLimiter(KeyValueStore& store, int maxRequests, int windowSeconds)
    : store_(store), maxRequests_(maxRequests), windowSeconds_(windowSeconds < 1 ? 1 : windowSeconds) {}

std::string RateLimiter::makeKey(const std::string& clientIp, std::int64_t window) {
    return "ratelimit:" + clientIp + ":" + std::to_string(window);
}

RateLimiter::Decision RateLimiter::allow(const std::string& clientIp) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return allow(clientIp, now);
}

RateLimiter::Decision RateLimiter::allow(const std::string& clientIp, std::int64_t nowSeconds) {
    Decision decision;
    if (!enabled()) {
        return decision;
    }

    const std::int64_t window = nowSeconds / windowSeconds_;
    const auto count = store_.incrementWithTtl(makeKey(clientIp, window), windowSeconds_);
    if (!count) {
        return decision;   // Redis unavailable: fail open
    }

    if (*count > maxRequests_) {
        decision.allowed = false;
        // Seconds left until the next window begins.
        decision.retryAfterSeconds = static_cast<int>((window + 1) * windowSeconds_ - nowSeconds);
    }
    return decision;
}

} // namespace minilb
