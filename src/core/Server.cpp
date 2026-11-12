#include "miniLB/core/Server.hpp"

#include "miniLB/util/Logger.hpp"

#include <csignal>
#include <memory>

namespace minilb {

Server::Server(asio::io_context& io, Context context)
    : io_(io),
      ctx_(context),
      strand_(asio::make_strand(io)),
      // Opening the acceptor this way binds to 0.0.0.0:<port>, sets
      // SO_REUSEADDR (so a restart does not fail with "address already in
      // use") and starts listening.
      acceptor_(strand_, tcp::endpoint(tcp::v4(), ctx_.config.listenPort)),
      signals_(strand_, SIGINT, SIGTERM),
      retryTimer_(strand_),
      drainTimer_(strand_) {}

void Server::start() {
    signals_.async_wait([this](const boost::system::error_code& ec, int signal) {
        if (!ec) {
            spdlog::info("received signal {}, shutting down", signal);
            beginShutdown();
        }
    });
    acceptNext();
}

void Server::acceptNext() {
    // make_strand(io_) gives every accepted socket its *own* strand. All
    // handlers of that connection then run one at a time (see Connection),
    // while different connections still run in parallel on different threads.
    acceptor_.async_accept(
        asio::make_strand(io_),
        [this](const boost::system::error_code& ec, tcp::socket socket) {
            if (!acceptor_.is_open()) {
                return;   // shutting down
            }
            if (ec) {
                // Typically "too many open files". Pause briefly instead of
                // spinning, then try again.
                spdlog::warn("accept failed: {}", ec.message());
                retryTimer_.expires_after(std::chrono::milliseconds(100));
                retryTimer_.async_wait([this](const boost::system::error_code& timerEc) {
                    if (!timerEc && acceptor_.is_open()) {
                        acceptNext();
                    }
                });
                return;
            }

            std::make_shared<Connection>(std::move(socket), ctx_)->start();
            acceptNext();
        });
}

void Server::beginShutdown() {
    boost::system::error_code ignored;
    acceptor_.close(ignored);   // stop taking new clients
    retryTimer_.cancel();

    drainDeadline_ = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(ctx_.config.shutdownGraceMs);
    waitForConnectionsToFinish();
}

void Server::waitForConnectionsToFinish() {
    const int live = ctx_.metrics.activeConnections.load();
    if (live == 0) {
        // Nothing is pending any more, so the io_context runs out of work
        // and EventLoop::run() returns on its own.
        spdlog::info("all connections finished");
        return;
    }
    if (std::chrono::steady_clock::now() >= drainDeadline_) {
        spdlog::warn("grace period over, dropping {} connection(s)", live);
        io_.stop();
        return;
    }
    drainTimer_.expires_after(std::chrono::milliseconds(50));
    drainTimer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
            waitForConnectionsToFinish();
        }
    });
}

} // namespace minilb
