#include "miniLB/core/ThreadPool.hpp"

#include "miniLB/util/Logger.hpp"

#include <exception>

namespace minilb {

ThreadPool::ThreadPool(std::size_t threadCount) {
    if (threadCount == 0) {
        threadCount = 1;
    }
    workers_.reserve(threadCount);
    for (std::size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wakeUp_.notify_all();
    for (std::thread& worker : workers_) {
        worker.join();
    }
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    // Notify after unlocking so the woken thread can take the lock at once.
    wakeUp_.notify_one();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Sleep until there is work or we are shutting down. The predicate
            // also protects against spurious wake-ups.
            wakeUp_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                return;   // stopping_ is set and the queue is drained
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // Run the task without holding the lock.
        try {
            task();
        } catch (const std::exception& e) {
            // An exception escaping a thread would terminate the process.
            spdlog::error("task threw an exception: {}", e.what());
        } catch (...) {
            spdlog::error("task threw an unknown exception");
        }
    }
}

} // namespace minilb
