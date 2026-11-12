#pragma once

// In-memory stand-in for Redis, used by the Cache and RateLimiter unit tests
// so that they need no running Redis server.

#include "miniLB/redis/RedisClient.hpp"

#include <map>
#include <mutex>
#include <string>

class FakeStore : public minilb::KeyValueStore {
public:
    // When false, every call behaves as if Redis were down.
    bool available = true;

    std::optional<std::string> get(const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = values.find(key);
        if (!available || it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool setWithTtl(const std::string& key, const std::string& value, int ttlSeconds) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available) {
            return false;
        }
        values[key] = value;
        ttls[key] = ttlSeconds;
        return true;
    }

    std::optional<long long> incrementWithTtl(const std::string& key, int ttlSeconds) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available) {
            return std::nullopt;
        }
        const long long count = ++counters[key];
        if (count == 1) {
            ttls[key] = ttlSeconds;
        }
        return count;
    }

    // Public so tests can inspect what was written.
    std::map<std::string, std::string> values;
    std::map<std::string, long long> counters;
    std::map<std::string, int> ttls;

private:
    std::mutex mutex_;
};
