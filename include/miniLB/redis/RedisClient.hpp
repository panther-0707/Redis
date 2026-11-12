#pragma once

#include "miniLB/util/Config.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Forward declaration so that only RedisClient.cpp needs the redis-plus-plus
// headers (faster builds, and the rest of the code cannot depend on them).
namespace sw::redis {
class Redis;
}

namespace minilb {

// The few key-value operations miniLB needs. Cache and RateLimiter depend on
// this small interface rather than on Redis directly, so unit tests can plug
// in an in-memory fake instead of a real Redis server.
//
// All calls are BLOCKING and must not run on an io_context thread.
// Implementations must be thread-safe and must never throw: "unavailable" is
// reported through the return value.
class KeyValueStore {
public:
    virtual ~KeyValueStore() = default;

    // Value for `key`; std::nullopt if the key does not exist or the store
    // is unavailable.
    virtual std::optional<std::string> get(const std::string& key) = 0;

    // Stores `value` under `key`, expiring after `ttlSeconds`.
    // Returns false if the store is unavailable.
    virtual bool setWithTtl(const std::string& key, const std::string& value, int ttlSeconds) = 0;

    // Adds 1 to the counter at `key` and returns the new value. When this
    // creates the counter it is also given a lifetime of `ttlSeconds`.
    // Returns std::nullopt if the store is unavailable.
    virtual std::optional<long long> incrementWithTtl(const std::string& key, int ttlSeconds) = 0;
};

// KeyValueStore backed by a real Redis server (via redis-plus-plus).
//
// Fail-open: if Redis is down or slow, every call reports "unavailable"
// instead of throwing, a warning is logged, and Redis is left alone for a
// short back-off period so that requests do not each pay a connect timeout.
// The load balancer then simply runs without cache and rate limiting.
//
// Thread-safety: redis-plus-plus keeps an internal connection pool, so one
// RedisClient can be shared by all threads. The back-off time is an atomic.
class RedisClient : public KeyValueStore {
public:
    // `poolSize`: number of Redis connections = number of threads that may
    // call concurrently. No connection is opened until the first call.
    RedisClient(const Config::Redis& settings, std::size_t poolSize);
    ~RedisClient() override;

    std::optional<std::string> get(const std::string& key) override;
    bool setWithTtl(const std::string& key, const std::string& value, int ttlSeconds) override;
    std::optional<long long> incrementWithTtl(const std::string& key, int ttlSeconds) override;

    // False while we are backing off after an error. For /stats.
    bool available() const;

private:
    // Runs `call`; converts any exception into std::nullopt plus back-off.
    template <typename Call>
    auto guarded(const char* operation, Call&& call) -> decltype(call());

    std::unique_ptr<sw::redis::Redis> redis_;

    // Steady-clock time (ms) before which Redis is not contacted. 0 = healthy.
    std::atomic<std::int64_t> retryAtMs_{0};
};

} // namespace minilb
