#include "miniLB/core/EventLoop.hpp"

#include "miniLB/core/ThreadPool.hpp"

namespace minilb {

void EventLoop::run() {
    ThreadPool pool(threadCount_);
    for (std::size_t i = 0; i < threadCount_; ++i) {
        // io_context::run() returns when there are no pending operations left.
        pool.submit([this] { io_.run(); });
    }
    // ~ThreadPool joins the threads, i.e. waits for every io_.run() to return.
}

} // namespace minilb
