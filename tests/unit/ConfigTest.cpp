// Unit tests for YAML config loading, Metrics JSON and RedisClient fail-open.

#include "miniLB/redis/RedisClient.hpp"
#include "miniLB/util/Config.hpp"
#include "miniLB/util/Metrics.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

using minilb::Config;

namespace {

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

const char* const kOneBackend = "backends:\n  - { host: 127.0.0.1, port: 8001 }\n";

} // namespace

TEST(Config, LoadsTheFullExampleFromTheSpec) {
    const Config config = loadYaml(R"(
listen_port: 9090
algorithm: weighted
threads: 8
backends:
  - { host: 127.0.0.1, port: 8001, weight: 1 }
  - { host: 127.0.0.1, port: 8002, weight: 1 }
  - { host: backend3, port: 8003, weight: 2 }
health_check:
  path: /healthz
  interval_ms: 500
  fail_threshold: 4
  success_threshold: 3
rate_limit:
  requests: 10
  window_seconds: 5
cache:
  enabled: false
  ttl_seconds: 7
timeouts_ms: { connect: 100, read: 200, write: 300 }
redis: { host: redis.local, port: 6380 }
)");

    EXPECT_EQ(config.listenPort, 9090);
    EXPECT_EQ(config.algorithm, "weighted");
    EXPECT_EQ(config.threads, 8u);
    ASSERT_EQ(config.backends.size(), 3u);
    EXPECT_EQ(config.backends[2].host, "backend3");
    EXPECT_EQ(config.backends[2].port, 8003);
    EXPECT_EQ(config.backends[2].weight, 2);
    EXPECT_EQ(config.healthCheck.path, "/healthz");
    EXPECT_EQ(config.healthCheck.intervalMs, 500);
    EXPECT_EQ(config.healthCheck.failThreshold, 4);
    EXPECT_EQ(config.healthCheck.successThreshold, 3);
    EXPECT_EQ(config.rateLimit.requests, 10);
    EXPECT_EQ(config.rateLimit.windowSeconds, 5);
    EXPECT_FALSE(config.cache.enabled);
    EXPECT_EQ(config.cache.ttlSeconds, 7);
    EXPECT_EQ(config.timeouts.connectMs, 100);
    EXPECT_EQ(config.timeouts.readMs, 200);
    EXPECT_EQ(config.timeouts.writeMs, 300);
    EXPECT_EQ(config.redis.host, "redis.local");
    EXPECT_EQ(config.redis.port, 6380);
}

TEST(Config, MissingKeysKeepTheirDefaults) {
    const Config config = loadYaml(kOneBackend);
    const Config defaults;

    EXPECT_EQ(config.listenPort, 8080);
    EXPECT_EQ(config.algorithm, "round_robin");
    EXPECT_EQ(config.threads, defaults.threads);
    EXPECT_EQ(config.backends[0].weight, 1);
    EXPECT_EQ(config.healthCheck.path, "/health");
    EXPECT_EQ(config.healthCheck.intervalMs, 2000);
    EXPECT_EQ(config.healthCheck.failThreshold, 3);
    EXPECT_EQ(config.healthCheck.successThreshold, 2);
    EXPECT_EQ(config.rateLimit.requests, 100);
    EXPECT_EQ(config.rateLimit.windowSeconds, 60);
    EXPECT_TRUE(config.cache.enabled);
    EXPECT_EQ(config.cache.ttlSeconds, 30);
    EXPECT_EQ(config.timeouts.connectMs, 1000);
    EXPECT_EQ(config.timeouts.readMs, 5000);
    EXPECT_EQ(config.timeouts.writeMs, 5000);
    EXPECT_EQ(config.redis.host, "127.0.0.1");
    EXPECT_EQ(config.redis.port, 6379);
}

TEST(Config, TheShippedConfigFilesAreValid) {
    // MINILB_SOURCE_DIR is set by CMake to the repository root.
    const std::string root = MINILB_SOURCE_DIR;
    const Config config = Config::load(root + "/config/config.yaml");
    EXPECT_EQ(config.listenPort, 8080);
    EXPECT_EQ(config.backends.size(), 3u);
    EXPECT_EQ(config.backends[2].weight, 2);

    const Config docker = Config::load(root + "/config/config.docker.yaml");
    EXPECT_EQ(docker.backends.size(), 3u);
    EXPECT_EQ(docker.backends[0].host, "backend1");
    EXPECT_EQ(docker.redis.host, "redis");
}

TEST(Config, RejectsBadInput) {
    expectError("listen_port: 8080\n", "at least one backend");
    expectError(std::string(kOneBackend) + "rate_limt: { requests: 5 }\n", "unknown key 'rate_limt'");
    expectError(std::string(kOneBackend) + "cache: { ttl: 5 }\n", "unknown key 'ttl' in cache");
    expectError(std::string(kOneBackend) + "listen_port: 70000\n", "'listen_port' must be between");
    expectError(std::string(kOneBackend) + "listen_port: 0\n", "'listen_port' must be between");
    expectError(std::string(kOneBackend) + "threads: many\n", "'threads' has an invalid value");
    expectError(std::string(kOneBackend) + "threads: 0\n", "'threads' must be at least 1");
    expectError(std::string(kOneBackend) + "algorithm: random\n", "'algorithm' must be");
    expectError(std::string(kOneBackend) + "health_check: { fail_threshold: 0 }\n",
                "'health_check.fail_threshold' must be at least 1");
    expectError(std::string(kOneBackend) + "health_check: { path: health }\n", "must start with '/'");
    expectError(std::string(kOneBackend) + "timeouts_ms: { connect: 0 }\n",
                "'timeouts_ms.connect' must be at least 1");
    expectError("backends:\n  - { host: a, port: 80, weight: 0 }\n", "'weight' must be at least 1");
    expectError("backends:\n  - { port: 80 }\n", "needs a 'host'");
    expectError("backends:\n  - { host: a }\n", "'port' must be between");
    expectError("backends: nope\n", "'backends' must be a list");
    expectError("just a string\n", "must be a mapping");
    expectError("backends: [unclosed\n", "cannot read");
}

TEST(Config, MissingFileIsAReadableError) {
    EXPECT_THROW(Config::load("/definitely/not/here.yaml"), std::runtime_error);
}

TEST(Metrics, JsonContainsCountersAndBackends) {
    minilb::BackendPool pool;
    pool.add("10.0.0.1", 8001, 1);
    pool.add("10.0.0.2", 8002, 2);
    pool.all()[1]->healthy = false;
    pool.all()[0]->totalRequests = 7;

    minilb::Metrics metrics;
    metrics.totalRequests = 12;
    metrics.cacheHits = 3;
    metrics.cacheMisses = 4;
    metrics.rateLimited = 2;
    metrics.errors = 1;
    metrics.activeConnections = 5;

    const std::string json = metrics.toJson(pool, "weighted", /*redisAvailable=*/false);
    for (const char* expected : {
             "\"total_requests\": 12",
             "\"active_connections\": 5",
             "\"cache_hits\": 3",
             "\"cache_misses\": 4",
             "\"rate_limited\": 2",
             "\"errors\": 1",
             "\"algorithm\": \"weighted\"",
             "\"redis_available\": false",
             "{\"id\": \"10.0.0.1:8001\", \"healthy\": true, \"weight\": 1, "
             "\"active_connections\": 0, \"requests\": 7}",
             "{\"id\": \"10.0.0.2:8002\", \"healthy\": false, \"weight\": 2, "
             "\"active_connections\": 0, \"requests\": 0}",
         }) {
        EXPECT_NE(json.find(expected), std::string::npos) << "missing: " << expected << "\n" << json;
    }
}

TEST(RedisClient, FailsOpenWhenRedisIsUnreachable) {
    Config::Redis settings;
    settings.host = "127.0.0.1";
    settings.port = 1;         // nothing listens on port 1
    settings.timeoutMs = 100;

    minilb::RedisClient redis(settings, 2);
    EXPECT_TRUE(redis.available()) << "no connection is attempted before the first call";

    // None of these may throw; they just report "unavailable".
    EXPECT_FALSE(redis.get("key").has_value());
    EXPECT_FALSE(redis.available()) << "after a failure the client backs off";
    EXPECT_FALSE(redis.setWithTtl("key", "value", 10));
    EXPECT_FALSE(redis.incrementWithTtl("counter", 10).has_value());
}
