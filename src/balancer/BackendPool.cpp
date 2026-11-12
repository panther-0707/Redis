#include "miniLB/balancer/BackendPool.hpp"

namespace minilb {

void BackendPool::add(std::string host, unsigned short port, int weight) {
    backends_.push_back(std::make_unique<Backend>(std::move(host), port, weight));
}

std::vector<Backend*> BackendPool::healthy() const {
    std::vector<Backend*> result;
    result.reserve(backends_.size());
    for (const auto& backend : backends_) {
        if (backend->healthy.load()) {
            result.push_back(backend.get());
        }
    }
    return result;
}

} // namespace minilb
