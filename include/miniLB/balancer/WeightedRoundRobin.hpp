#pragma once

#include "miniLB/balancer/Strategy.hpp"

#include <atomic>
#include <cstddef>

namespace minilb {

// Weighted round robin: each backend receives traffic in proportion to its
// weight. With weights 1, 1, 2 a cycle of four requests goes A, B, C, C.
//
// How: imagine one numbered ticket per unit of weight (A:0, B:1, C:2,3). A
// shared counter walks through the tickets; each request takes the next one.
//
// Thread-safety: one atomic counter, same as RoundRobin.
class WeightedRoundRobin : public Strategy {
public:
    Backend* select(const std::vector<Backend*>& healthy, const HttpRequest& request) override;
    const char* name() const override { return "weighted"; }

private:
    std::atomic<std::size_t> next_{0};
};

} // namespace minilb
