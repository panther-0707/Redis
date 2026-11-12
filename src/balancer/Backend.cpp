#include "miniLB/balancer/Backend.hpp"

namespace minilb {

bool Backend::recordSuccess(int successThreshold) {
    consecutiveFailures_.store(0);
    const int successes = consecutiveSuccesses_.fetch_add(1) + 1;
    if (successes < successThreshold) {
        return false;
    }
    // exchange() returns the previous value, so even if two threads get here
    // together only one of them sees "was unhealthy" and reports the change.
    return !healthy.exchange(true);
}

bool Backend::recordFailure(int failThreshold) {
    consecutiveSuccesses_.store(0);
    const int failures = consecutiveFailures_.fetch_add(1) + 1;
    if (failures < failThreshold) {
        return false;
    }
    return healthy.exchange(false);
}

} // namespace minilb
