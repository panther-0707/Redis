#pragma once

#include <boost/asio.hpp>

#include <cstddef>

namespace minilb {

namespace asio = boost::asio;

// Owns the Asio io_context and runs it on N threads.
//
// The io_context is a queue of "completion handlers": callbacks that run
// when an asynchronous operation (accept, read, write, timer) finishes. Any
// thread that calls io_context::run() takes handlers from that queue, so
// running it on N threads lets N handlers execute in parallel.
class EventLoop {
public:
    explicit EventLoop(std::size_t threadCount) : threadCount_(threadCount == 0 ? 1 : threadCount) {}

    asio::io_context& context() { return io_; }

    // Runs the io_context on threadCount threads. Blocks until the loop has
    // no work left (see Server: graceful shutdown) or stop() is called.
    void run();

    // Makes run() return as soon as possible, abandoning unfinished work.
    void stop() { io_.stop(); }

private:
    std::size_t threadCount_;
    asio::io_context io_;
};

} // namespace minilb
