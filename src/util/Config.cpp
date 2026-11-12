#include "miniLB/util/Config.hpp"

#include <yaml-cpp/yaml.h>

#include <initializer_list>
#include <stdexcept>

namespace minilb {

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("config: " + message);
}

// Rejects keys we do not know. A typo such as "rate_limt:" would otherwise
// be ignored silently and the default used instead.
void checkKeys(const YAML::Node& node, const std::string& where,
               std::initializer_list<const char*> allowed) {
    if (!node.IsMap()) {
        fail("'" + where + "' must be a mapping (key: value pairs)");
    }
    for (const auto& entry : node) {
        const std::string key = entry.first.as<std::string>();
        bool known = false;
        for (const char* name : allowed) {
            known = known || key == name;
        }
        if (!known) {
            fail("unknown key '" + key + "' in " + where);
        }
    }
}

// Reads parent[key] into `out` if the key is present; otherwise `out` keeps
// its default. A value of the wrong type becomes a readable error.
template <typename T>
void read(const YAML::Node& parent, const char* key, T& out) {
    const YAML::Node node = parent[key];
    if (!node) {
        return;
    }
    try {
        out = node.as<T>();
    } catch (const YAML::Exception&) {
        fail(std::string("'") + key + "' has an invalid value");
    }
}

// Ports are read as int first so that out-of-range values are reported
// instead of silently wrapping around.
void readPort(const YAML::Node& parent, const char* key, unsigned short& out) {
    int port = out;
    read(parent, key, port);
    if (port < 1 || port > 65535) {
        fail(std::string("'") + key + "' must be between 1 and 65535");
    }
    out = static_cast<unsigned short>(port);
}

void requirePositive(long long value, const char* name) {
    if (value < 1) {
        fail(std::string("'") + name + "' must be at least 1");
    }
}

} // namespace

Config Config::load(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        fail("cannot read '" + path + "': " + e.what());
    }

    Config config;
    checkKeys(root, "the top level",
              {"listen_port", "algorithm", "threads", "log_level", "shutdown_grace_ms", "backends",
               "health_check", "rate_limit", "cache", "timeouts_ms", "limits", "redis"});

    readPort(root, "listen_port", config.listenPort);
    read(root, "algorithm", config.algorithm);
    read(root, "threads", config.threads);
    read(root, "log_level", config.logLevel);
    read(root, "shutdown_grace_ms", config.shutdownGraceMs);

    if (const YAML::Node backends = root["backends"]) {
        if (!backends.IsSequence()) {
            fail("'backends' must be a list");
        }
        for (const YAML::Node& node : backends) {
            checkKeys(node, "a backends entry", {"host", "port", "weight"});
            BackendEntry entry;
            read(node, "host", entry.host);
            readPort(node, "port", entry.port);
            read(node, "weight", entry.weight);
            if (entry.host.empty()) {
                fail("every backend needs a 'host'");
            }
            requirePositive(entry.weight, "weight");
            config.backends.push_back(entry);
        }
    }

    if (const YAML::Node node = root["health_check"]) {
        checkKeys(node, "health_check",
                  {"path", "interval_ms", "timeout_ms", "fail_threshold", "success_threshold"});
        read(node, "path", config.healthCheck.path);
        read(node, "interval_ms", config.healthCheck.intervalMs);
        read(node, "timeout_ms", config.healthCheck.timeoutMs);
        read(node, "fail_threshold", config.healthCheck.failThreshold);
        read(node, "success_threshold", config.healthCheck.successThreshold);
    }

    if (const YAML::Node node = root["rate_limit"]) {
        checkKeys(node, "rate_limit", {"requests", "window_seconds"});
        read(node, "requests", config.rateLimit.requests);
        read(node, "window_seconds", config.rateLimit.windowSeconds);
    }

    if (const YAML::Node node = root["cache"]) {
        checkKeys(node, "cache", {"enabled", "ttl_seconds", "max_entry_bytes"});
        read(node, "enabled", config.cache.enabled);
        read(node, "ttl_seconds", config.cache.ttlSeconds);
        read(node, "max_entry_bytes", config.cache.maxEntryBytes);
    }

    if (const YAML::Node node = root["timeouts_ms"]) {
        checkKeys(node, "timeouts_ms", {"connect", "read", "write"});
        read(node, "connect", config.timeouts.connectMs);
        read(node, "read", config.timeouts.readMs);
        read(node, "write", config.timeouts.writeMs);
    }

    if (const YAML::Node node = root["limits"]) {
        checkKeys(node, "limits",
                  {"max_request_header_bytes", "max_request_body_bytes", "max_response_bytes"});
        read(node, "max_request_header_bytes", config.limits.maxRequestHeaderBytes);
        read(node, "max_request_body_bytes", config.limits.maxRequestBodyBytes);
        read(node, "max_response_bytes", config.limits.maxResponseBytes);
    }

    if (const YAML::Node node = root["redis"]) {
        checkKeys(node, "redis", {"host", "port", "timeout_ms", "threads"});
        read(node, "host", config.redis.host);
        readPort(node, "port", config.redis.port);
        read(node, "timeout_ms", config.redis.timeoutMs);
        read(node, "threads", config.redis.threads);
    }

    config.validate();
    return config;
}

void Config::validate() const {
    if (backends.empty()) {
        fail("at least one backend is required");
    }
    if (algorithm != "round_robin" && algorithm != "least_connections" && algorithm != "weighted") {
        fail("'algorithm' must be round_robin, least_connections or weighted");
    }
    if (healthCheck.path.empty() || healthCheck.path.front() != '/') {
        fail("'health_check.path' must start with '/'");
    }

    requirePositive(static_cast<long long>(threads), "threads");
    requirePositive(static_cast<long long>(redis.threads), "redis.threads");
    requirePositive(redis.timeoutMs, "redis.timeout_ms");
    requirePositive(healthCheck.intervalMs, "health_check.interval_ms");
    requirePositive(healthCheck.timeoutMs, "health_check.timeout_ms");
    requirePositive(healthCheck.failThreshold, "health_check.fail_threshold");
    requirePositive(healthCheck.successThreshold, "health_check.success_threshold");
    requirePositive(rateLimit.windowSeconds, "rate_limit.window_seconds");
    requirePositive(cache.ttlSeconds, "cache.ttl_seconds");
    requirePositive(timeouts.connectMs, "timeouts_ms.connect");
    requirePositive(timeouts.readMs, "timeouts_ms.read");
    requirePositive(timeouts.writeMs, "timeouts_ms.write");
    requirePositive(static_cast<long long>(limits.maxRequestHeaderBytes),
                    "limits.max_request_header_bytes");
    requirePositive(static_cast<long long>(limits.maxResponseBytes), "limits.max_response_bytes");
    if (shutdownGraceMs < 0) {
        fail("'shutdown_grace_ms' must not be negative");
    }
}

} // namespace minilb
