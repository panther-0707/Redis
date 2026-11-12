#pragma once

#include "miniLB/balancer/Strategy.hpp"

#include <atomic>
#include <cstddef>

namespace minilb {

// Round robin: hand out backends in turn (1, 2, 3, 1, 2, 3, ...).
//
// Thread-safety: the only state is one atomic counter. fetch_add gives every
// caller a distinct number without a lock.
class RoundRobin : public Strategy {
public:
    Backend* select(const std::vector<Backend*>& healthy, const HttpRequest& request) override;
    const char* name() const override { return "round_robin"; }

private:
    std::atomic<std::size_t> next_{0};
};

} // namespace minilb
