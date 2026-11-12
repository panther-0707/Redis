#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace minilb {

// All runtime settings in one place. Normally filled from a YAML file by
// Config::load(); the defaults below are used for every key that is absent.
//
// The Config is created once at startup and only read afterwards, so it can
// be shared between threads without locking.
struct Config {
    // Parses and validates a YAML file. Throws std::runtime_error with a
    // readable message if the file is missing, malformed or has bad values.
    static Config load(const std::string& path);

    // Checks the values for consistency; throws std::runtime_error if not.
    void validate() const;

    struct BackendEntry {
        std::string host;
        unsigned short port = 0;
        int weight = 1;
    };

    struct HealthCheck {
        std::string path = "/health";
        int intervalMs = 2000;        // time between probe rounds
        int timeoutMs = 1000;         // deadline for a single probe
        int failThreshold = 3;        // failures in a row before "unhealthy"
        int successThreshold = 2;     // successes in a row before "healthy" again
    };

    struct RateLimit {
        int requests = 100;           // per client IP per window; <= 0 disables
        int windowSeconds = 60;
    };

    struct Cache {
        bool enabled = true;
        int ttlSeconds = 30;
        std::size_t maxEntryBytes = 1024 * 1024;   // larger bodies are not cached
    };

    struct Redis {
        std::string host = "127.0.0.1";
        unsigned short port = 6379;
        int timeoutMs = 200;          // connect / command timeout (then fail open)
        std::size_t threads = 4;      // threads (and connections) for blocking Redis calls
    };

    // Every network operation has a deadline so a stuck peer cannot hold a
    // connection (and its memory) forever.
    struct Timeouts {
        int connectMs = 1000;   // open a TCP connection to a backend
        int readMs = 5000;      // receive a full request / a full backend response
        int writeMs = 5000;     // send a full request / response
    };

    // Size caps protect against clients or backends that send endless data.
    struct Limits {
        std::size_t maxRequestHeaderBytes = 16 * 1024;
        std::size_t maxRequestBodyBytes = 1024 * 1024;
        std::size_t maxResponseBytes = 8 * 1024 * 1024;
    };

    unsigned short listenPort = 8080;
    std::string algorithm = "round_robin";   // round_robin | least_connections | weighted
    std::size_t threads = 4;              // io_context worker threads
    std::string logLevel = "info";        // trace | debug | info | warn | error | off
    int shutdownGraceMs = 5000;           // how long in-flight requests may finish on shutdown
    std::vector<BackendEntry> backends;
    HealthCheck healthCheck;
    RateLimit rateLimit;
    Cache cache;
    Redis redis;
    Timeouts timeouts;
    Limits limits;
};

} // namespace minilb
