#include "miniLB/balancer/LeastConnections.hpp"

#include "miniLB/util/Assert.hpp"

namespace minilb {

Backend* LeastConnections::select(const std::vector<Backend*>& healthy, const HttpRequest&) {
    if (healthy.empty()) {
        return nullptr;
    }

    const std::size_t count = healthy.size();
    const std::size_t start = next_.fetch_add(1, std::memory_order_relaxed) % count;

    Backend* best = nullptr;
    int bestLoad = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Backend* candidate = healthy[(start + i) % count];
        const int load = candidate->activeConnections.load();
        if (best == nullptr || load < bestLoad) {
            best = candidate;
            bestLoad = load;
        }
    }
    MINILB_ASSERT(best != nullptr, "a non-empty healthy list always yields a backend");
    MINILB_ASSERT(bestLoad >= 0, "the in-flight count of a backend is never negative");
    return best;
}

} // namespace minilb
