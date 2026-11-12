#include "miniLB/balancer/RoundRobin.hpp"

namespace minilb {

Backend* RoundRobin::select(const std::vector<Backend*>& healthy, const HttpRequest&) {
    if (healthy.empty()) {
        return nullptr;
    }
    // Unsigned overflow simply wraps around, which is fine here.
    const std::size_t turn = next_.fetch_add(1, std::memory_order_relaxed);
    return healthy[turn % healthy.size()];
}

} // namespace minilb
