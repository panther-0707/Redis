#include "miniLB/health/HealthChecker.hpp"

#include "miniLB/util/Logger.hpp"

#include <boost/asio.hpp>

namespace minilb {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

HealthChecker::HealthChecker(BackendPool& pool, Config::HealthCheck settings)
    : pool_(pool), settings_(std::move(settings)) {}

HealthChecker::~HealthChecker() {
    stop();
}

void HealthChecker::start() {
    if (thread_.joinable()) {
        return;   // already running
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
    thread_ = std::thread([this] { run(); });
}

void HealthChecker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    stopSignal_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HealthChecker::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        // Never hold a lock while doing network I/O.
        lock.unlock();
        checkAll();
        lock.lock();

        // Sleep for one interval, but wake up at once if stop() is called.
        stopSignal_.wait_for(lock, std::chrono::milliseconds(settings_.intervalMs),
                             [this] { return stopping_; });
    }
}

void HealthChecker::checkAll() {
    for (const auto& backend : pool_.all()) {
        const bool ok = probe(backend->host, backend->port, settings_.path,
                              std::chrono::milliseconds(settings_.timeoutMs));
        if (ok) {
            if (backend->recordSuccess(settings_.successThreshold)) {
                spdlog::info("health: backend {} is healthy again", backend->id);
            }
        } else {
            if (backend->recordFailure(settings_.failThreshold)) {
                spdlog::warn("health: backend {} is now UNHEALTHY", backend->id);
            }
        }
    }
}

bool HealthChecker::probe(const std::string& host, unsigned short port, const std::string& path,
                          std::chrono::milliseconds timeout) {
    try {
        // A private io_context just for this probe. The operations below are
        // chained asynchronously, and run_for() drives them for at most
        // `timeout`. Whatever has not completed by then counts as a failure.
        // That gives one overall deadline without any timer bookkeeping.
        asio::io_context io;
        tcp::socket socket(io);

        boost::system::error_code resolveError;
        tcp::resolver resolver(io);
        const auto endpoints = resolver.resolve(host, std::to_string(port), resolveError);
        if (resolveError) {
            return false;
        }

        const std::string request =
            "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        std::string reply;
        bool healthy = false;

        // The lambdas capture by reference: everything they use lives on this
        // stack frame, which outlives io.run_for() below.
        asio::async_connect(
            socket, endpoints, [&](const boost::system::error_code& ec, const tcp::endpoint&) {
                if (ec) {
                    return;
                }
                asio::async_write(
                    socket, asio::buffer(request),
                    [&](const boost::system::error_code& writeEc, std::size_t) {
                        if (writeEc) {
                            return;
                        }
                        // The status line is all we need: "HTTP/1.1 200 OK".
                        asio::async_read_until(
                            socket, asio::dynamic_buffer(reply), "\r\n",
                            [&](const boost::system::error_code& readEc, std::size_t) {
                                if (readEc) {
                                    return;
                                }
                                // Characters 9..11 hold the status code; any 2xx is fine.
                                healthy = reply.size() >= 12 && reply.compare(0, 7, "HTTP/1.") == 0 &&
                                          reply[9] == '2';
                            });
                    });
            });

        io.run_for(timeout);
        return healthy;
    } catch (const std::exception& e) {
        spdlog::warn("health: probe of {}:{} threw: {}", host, port, e.what());
        return false;
    }
}

} // namespace minilb
