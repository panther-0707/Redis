#pragma once

#include "miniLB/core/Connection.hpp"

#include <boost/asio.hpp>

#include <chrono>

namespace minilb {

// Accepts client connections and starts a Connection for each one.
//
// Graceful shutdown: on SIGINT (Ctrl-C) or SIGTERM the server stops
// accepting, lets in-flight requests finish for up to
// `config.shutdownGraceMs`, and then lets the event loop end.
//
// Thread-safety: the acceptor, the signal set and the timers all use one
// strand, so their handlers never run concurrently even though the
// io_context runs on several threads.
class Server {
public:
    // Binds and listens immediately; throws if the port is unavailable.
    Server(asio::io_context& io, Context context);

    // Starts accepting connections and watching for shutdown signals.
    // Returns immediately; the work happens on the io_context threads.
    void start();

    // The port actually bound (useful when listenPort is 0 = "any free port").
    unsigned short port() const { return acceptor_.local_endpoint().port(); }

private:
    void acceptNext();
    void beginShutdown();
    void waitForConnectionsToFinish();

    asio::io_context& io_;
    Context ctx_;
    asio::strand<asio::io_context::executor_type> strand_;
    tcp::acceptor acceptor_;
    asio::signal_set signals_;
    asio::steady_timer retryTimer_;   // pause after a failed accept
    asio::steady_timer drainTimer_;   // polls for "all connections done" on shutdown
    std::chrono::steady_clock::time_point drainDeadline_;
};

} // namespace minilb
