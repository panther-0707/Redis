#include "miniLB/balancer/Backend.hpp"

namespace minilb {

bool Backend::recordSuccess(int successThreshold) {
    MINILB_ASSERT(successThreshold >= 1, "the success threshold must be at least 1 (Config::validate)");
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
    MINILB_ASSERT(failThreshold >= 1, "the failure threshold must be at least 1 (Config::validate)");
    consecutiveSuccesses_.store(0);
    const int failures = consecutiveFailures_.fetch_add(1) + 1;
    if (failures < failThreshold) {
        return false;
    }
    return healthy.exchange(false);
}

void Backend::recordRequestSuccess() {
    consecutiveFailures_.store(0);
}

} // namespace minilb
