#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace minilb {

// A fixed-size pool of worker threads fed from one task queue.
//
// miniLB uses it in two places:
//   * EventLoop runs the Asio io_context on N of these threads.
//   * Blocking Redis calls run on a separate pool, so that they never stall
//     the networking threads.
//
// Thread-safety: `tasks_` and `stopping_` are guarded by `mutex_`. Tasks are
// executed *outside* the lock, so a slow task never blocks submit().
class ThreadPool {
public:
    explicit ThreadPool(std::size_t threadCount);

    // Runs every task that is still queued, then joins all threads.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Queues a task; some worker thread will run it. Safe to call from any
    // thread, including from inside a task.
    void submit(std::function<void()> task);

    std::size_t size() const { return workers_.size(); }

private:
    void workerLoop();

    std::mutex mutex_;
    std::condition_variable wakeUp_;            // signalled on new task / shutdown
    std::queue<std::function<void()>> tasks_;   // guarded by mutex_
    bool stopping_ = false;                     // guarded by mutex_
    std::vector<std::thread> workers_;
};

} // namespace minilb
