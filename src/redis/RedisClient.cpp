#include "miniLB/redis/RedisClient.hpp"

#include "miniLB/util/Logger.hpp"

#include <sw/redis++/redis++.h>

#include <chrono>

namespace minilb {

namespace {

// How long to leave Redis alone after an error.
constexpr std::int64_t kBackoffMs = 1000;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

RedisClient::RedisClient(const Config::Redis& settings, std::size_t poolSize) {
    const std::chrono::milliseconds timeout(settings.timeoutMs);

    sw::redis::ConnectionOptions connection;
    connection.host = settings.host;
    connection.port = settings.port;
    connection.connect_timeout = timeout;   // never wait long for a dead Redis
    connection.socket_timeout = timeout;    // ... nor for a stuck one

    sw::redis::ConnectionPoolOptions pool;
    pool.size = poolSize == 0 ? 1 : poolSize;
    pool.wait_timeout = timeout;            // max wait for a free pooled connection

    // Connections are created lazily, so this succeeds even if Redis is down.
    redis_ = std::make_unique<sw::redis::Redis>(connection, pool);
}

// Defined here (not in the header) because sw::redis::Redis is only a
// forward declaration there.
RedisClient::~RedisClient() = default;

bool RedisClient::available() const {
    return nowMs() >= retryAtMs_.load();
}

template <typename Call>
auto RedisClient::guarded(const char* operation, Call&& call) -> decltype(call()) {
    if (!available()) {
        return std::nullopt;   // still backing off: fail open without trying
    }
    try {
        return call();
    } catch (const std::exception& e) {
        // sw::redis::Error derives from std::exception. Covers connection
        // refused, timeouts, protocol errors, ...
        retryAtMs_.store(nowMs() + kBackoffMs);
        spdlog::warn("redis {} failed ({}); continuing without Redis for {} ms", operation,
                     e.what(), kBackoffMs);
        return std::nullopt;
    }
}

std::optional<std::string> RedisClient::get(const std::string& key) {
    return guarded("GET", [&]() -> std::optional<std::string> {
        return redis_->get(key);   // already an optional: empty if the key is missing
    });
}

bool RedisClient::setWithTtl(const std::string& key, const std::string& value, int ttlSeconds) {
    const auto stored = guarded("SET", [&]() -> std::optional<bool> {
        // SETEX key ttl value  (same as: SET key value EX ttl)
        redis_->setex(key, ttlSeconds, value);
        return true;
    });
    return stored.value_or(false);
}

std::optional<long long> RedisClient::incrementWithTtl(const std::string& key, int ttlSeconds) {
    return guarded("INCR", [&]() -> std::optional<long long> {
        const long long count = redis_->incr(key);
        if (count == 1) {
            // First increment created the key: start its lifetime now.
            redis_->expire(key, std::chrono::seconds(ttlSeconds));
        }
        return count;
    });
}

} // namespace minilb
