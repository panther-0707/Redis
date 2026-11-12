#pragma once

#include "miniLB/balancer/BackendPool.hpp"
#include "miniLB/util/Config.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace minilb {

// Active health checking on a background thread.
//
// Every `intervalMs` it sends "GET <path>" to each backend. A backend is
// taken out of rotation after `failThreshold` failed probes in a row and put
// back after `successThreshold` successful probes in a row (the counting
// lives in Backend::recordFailure / recordSuccess). Requiring several
// results in a row avoids "flapping" on a single lost packet.
//
// Thread-safety: the thread only touches the backends' atomics. `stopping_`
// is guarded by `mutex_`; the condition variable lets stop() interrupt the
// sleep between rounds immediately.
class HealthChecker {
public:
    HealthChecker(BackendPool& pool, Config::HealthCheck settings);
    ~HealthChecker();   // calls stop()

    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    void start();   // launches the thread; the first round runs immediately
    void stop();    // asks the thread to finish and waits for it

    // Performs one probe: true if "GET <path>" on host:port returned a 2xx
    // status within `timeout`. Blocking; never throws.
    static bool probe(const std::string& host, unsigned short port, const std::string& path,
                      std::chrono::milliseconds timeout);

private:
    void run();        // thread body
    void checkAll();   // one round over every backend

    BackendPool& pool_;
    Config::HealthCheck settings_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable stopSignal_;
    bool stopping_ = false;   // guarded by mutex_
};

} // namespace minilb
