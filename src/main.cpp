#include "miniLB/balancer/BackendPool.hpp"
#include "miniLB/balancer/Strategy.hpp"
#include "miniLB/core/EventLoop.hpp"
#include "miniLB/core/Server.hpp"
#include "miniLB/core/ThreadPool.hpp"
#include "miniLB/health/HealthChecker.hpp"
#include "miniLB/redis/Cache.hpp"
#include "miniLB/redis/RateLimiter.hpp"
#include "miniLB/redis/RedisClient.hpp"
#include "miniLB/util/Config.hpp"
#include "miniLB/util/Logger.hpp"
#include "miniLB/util/Metrics.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace {

void printUsage(const char* program) {
    std::cerr << "usage: " << program << " [--config <file>]\n"
              << "  --config <file>   YAML configuration (default: config/config.yaml)\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string configPath = "config/config.yaml";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else {
            printUsage(argv[0]);
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }

    try {
        const minilb::Config config = minilb::Config::load(configPath);
        minilb::initLogger(config.logLevel);

        // ---- Backends and the algorithm that chooses between them ----------
        minilb::BackendPool pool;
        for (const auto& backend : config.backends) {
            pool.add(backend.host, backend.port, backend.weight);
        }
        const auto strategy = minilb::makeStrategy(config.algorithm);

        // ---- Redis: cache + rate limiting (both fail open if Redis is down) -
        minilb::RedisClient redis(config.redis, config.redis.threads);
        minilb::RateLimiter rateLimiter(redis, config.rateLimit.requests,
                                        config.rateLimit.windowSeconds);
        minilb::Cache cache(redis, config.cache);

        minilb::Metrics metrics;

        // ---- Threads ---------------------------------------------------------
        // Declaration order matters here: objects are destroyed in reverse
        // order, so the blocking pool (whose queued tasks may still refer to
        // connections) goes away before the io_context inside `loop`.
        minilb::EventLoop loop(config.threads);
        minilb::ThreadPool blockingPool(config.redis.threads);

        minilb::Server server(loop.context(),
                              minilb::Context{config, pool, *strategy, rateLimiter, cache, redis,
                                              blockingPool, metrics});
        server.start();

        // Background thread that probes the backends and flips their
        // `healthy` flags.
        minilb::HealthChecker healthChecker(pool, config.healthCheck);
        healthChecker.start();

        spdlog::info("miniLB listening on port {} ({} over {} backends, {} threads)", server.port(),
                     strategy->name(), pool.all().size(), config.threads);

        loop.run();   // blocks until a shutdown signal has been handled
        healthChecker.stop();
        spdlog::info("bye");
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
