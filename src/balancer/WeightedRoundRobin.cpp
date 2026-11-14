#include "miniLB/balancer/WeightedRoundRobin.hpp"

#include "miniLB/util/Assert.hpp"

namespace minilb {

Backend* WeightedRoundRobin::select(const std::vector<Backend*>& healthy, const HttpRequest&) {
    if (healthy.empty()) {
        return nullptr;
    }

    // The total is recomputed on every call because the healthy set changes
    // as backends go down and come back.
    std::size_t totalWeight = 0;
    for (const Backend* backend : healthy) {
        totalWeight += static_cast<std::size_t>(backend->weight);
    }

    MINILB_ASSERT(totalWeight > 0, "every backend has weight >= 1, so the total cannot be zero");

    // Find the backend that owns this ticket.
    std::size_t ticket = next_.fetch_add(1, std::memory_order_relaxed) % totalWeight;
    for (Backend* backend : healthy) {
        const auto weight = static_cast<std::size_t>(backend->weight);
        if (ticket < weight) {
            return backend;
        }
        ticket -= weight;
    }
    MINILB_ASSERT(false, "a ticket below the total weight always belongs to some backend");
    return healthy.back();   // not reachable (weights are >= 1), but keeps the compiler happy
}

} // namespace minilb
