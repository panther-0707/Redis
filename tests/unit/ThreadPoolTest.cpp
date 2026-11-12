// Unit tests for the worker thread pool.

#include "miniLB/core/ThreadPool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using minilb::ThreadPool;

TEST(ThreadPool, RunsEveryTaskBeforeDestructionCompletes) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 1000; ++i) {
            pool.submit([&counter] { ++counter; });
        }
        // The destructor must drain the queue, not drop tasks.
    }
    EXPECT_EQ(counter.load(), 1000);
}

TEST(ThreadPool, ReportsItsSizeAndTreatsZeroAsOne) {
    EXPECT_EQ(ThreadPool(3).size(), 3u);
    EXPECT_EQ(ThreadPool(0).size(), 1u);
}

TEST(ThreadPool, RunsTasksInParallel) {
    constexpr int kThreads = 4;
    ThreadPool pool(kThreads);

    // Every task waits until all kThreads tasks have started. That can only
    // happen if the pool really runs them on kThreads different threads at
    // the same time.
    std::mutex mutex;
    std::condition_variable allStarted;
    int started = 0;
    std::set<std::thread::id> threadIds;
    std::atomic<int> finished{0};

    for (int i = 0; i < kThreads; ++i) {
        pool.submit([&] {
            std::unique_lock<std::mutex> lock(mutex);
            ++started;
            threadIds.insert(std::this_thread::get_id());
            allStarted.notify_all();
            const bool ok = allStarted.wait_for(lock, std::chrono::seconds(10),
                                                [&] { return started == kThreads; });
            if (ok) {
                ++finished;
            }
        });
    }

    // Wait for the tasks to finish (bounded, so a broken pool fails the test
    // instead of hanging it).
    for (int i = 0; i < 1000 && finished.load() < kThreads; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(finished.load(), kThreads);

    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(threadIds.size(), static_cast<std::size_t>(kThreads));
}

TEST(ThreadPool, SingleThreadRunsTasksInSubmissionOrder) {
    std::vector<int> order;
    {
        ThreadPool pool(1);
        for (int i = 0; i < 100; ++i) {
            pool.submit([&order, i] { order.push_back(i); });   // one worker: no race
        }
    }
    ASSERT_EQ(order.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(order[i], i);
    }
}

TEST(ThreadPool, SurvivesATaskThatThrows) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(2);
        pool.submit([] { throw std::runtime_error("boom"); });
        pool.submit([] { throw 42; });
        for (int i = 0; i < 10; ++i) {
            pool.submit([&counter] { ++counter; });
        }
    }
    EXPECT_EQ(counter.load(), 10);
}

TEST(ThreadPool, AcceptsSubmissionsFromManyThreads) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        std::vector<std::thread> producers;
        for (int t = 0; t < 8; ++t) {
            producers.emplace_back([&pool, &counter] {
                for (int i = 0; i < 250; ++i) {
                    pool.submit([&counter] { ++counter; });
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }
    }
    EXPECT_EQ(counter.load(), 2000);
}

TEST(ThreadPool, TasksCanSubmitMoreTasks) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(2);
        std::mutex mutex;
        std::condition_variable done;
        bool innerRan = false;

        pool.submit([&] {
            ++counter;
            pool.submit([&] {
                ++counter;
                std::lock_guard<std::mutex> lock(mutex);
                innerRan = true;
                done.notify_all();
            });
        });

        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(done.wait_for(lock, std::chrono::seconds(10), [&] { return innerRan; }));
    }
    EXPECT_EQ(counter.load(), 2);
}
