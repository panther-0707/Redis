#include "miniLB/balancer/Strategy.hpp"

#include "miniLB/balancer/LeastConnections.hpp"
#include "miniLB/balancer/RoundRobin.hpp"
#include "miniLB/balancer/WeightedRoundRobin.hpp"

#include <stdexcept>

namespace minilb {

std::unique_ptr<Strategy> makeStrategy(const std::string& name) {
    if (name == "round_robin") {
        return std::make_unique<RoundRobin>();
    }
    if (name == "least_connections") {
        return std::make_unique<LeastConnections>();
    }
    if (name == "weighted") {
        return std::make_unique<WeightedRoundRobin>();
    }
    throw std::invalid_argument("unknown algorithm '" + name +
                                "' (expected round_robin, least_connections or weighted)");
}

} // namespace minilb
