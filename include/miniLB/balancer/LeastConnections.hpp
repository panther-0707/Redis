#pragma once

#include "miniLB/balancer/Strategy.hpp"

#include <atomic>
#include <cstddef>

namespace minilb {

// Least connections: pick the backend with the fewest in-flight requests.
// Good when requests differ a lot in duration, because slow backends
// naturally accumulate connections and receive less new work.
//
// Thread-safety: reads each backend's atomic activeConnections. Two threads
// can read the same counts and pick the same backend at the same moment;
// that is harmless (the choice is a heuristic, not an invariant).
class LeastConnections : public Strategy {
public:
    Backend* select(const std::vector<Backend*>& healthy, const HttpRequest& request) override;
    const char* name() const override { return "least_connections"; }

private:
    // Rotates where the scan starts so that ties (e.g. every backend idle)
    // are spread across backends instead of always going to the first one.
    std::atomic<std::size_t> next_{0};
};

} // namespace minilb
