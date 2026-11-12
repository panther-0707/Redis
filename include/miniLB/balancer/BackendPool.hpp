#pragma once

#include "miniLB/balancer/Backend.hpp"

#include <memory>
#include <string>
#include <vector>

namespace minilb {

// The set of all configured backends.
//
// Thread-safety: the list itself is filled once at startup (add()) and never
// changes afterwards, so reading it concurrently needs no lock. The only
// state that changes at runtime lives in each Backend's atomics. healthy()
// therefore builds a fresh snapshot on every call; a snapshot can be slightly
// stale (a backend may fail right after), which the proxy handles by
// retrying on another backend.
class BackendPool {
public:
    // Startup only: must not be called once other threads use the pool.
    void add(std::string host, unsigned short port, int weight = 1);

    // Snapshot of the backends that are healthy right now.
    std::vector<Backend*> healthy() const;

    // Every backend, healthy or not (health checker, /stats).
    const std::vector<std::unique_ptr<Backend>>& all() const { return backends_; }

private:
    // unique_ptr because Backend holds atomics and therefore cannot be moved
    // when the vector grows. It also keeps Backend* pointers stable.
    std::vector<std::unique_ptr<Backend>> backends_;
};

} // namespace minilb
