#pragma once

#include "miniLB/balancer/Backend.hpp"
#include "miniLB/http/HttpRequest.hpp"

#include <memory>
#include <string>
#include <vector>

namespace minilb {

// A load-balancing algorithm: picks which backend serves a request.
//
// This is the classic Strategy pattern: the proxy code only knows this
// interface, so algorithms can be swapped via configuration.
//
// select() is called concurrently from many threads, so implementations
// must be thread-safe.
class Strategy {
public:
    virtual ~Strategy() = default;

    // Picks one backend from `healthy`. Returns nullptr if the list is empty.
    // The request is passed in so that future strategies can route on it
    // (e.g. consistent hashing on the client IP or path).
    virtual Backend* select(const std::vector<Backend*>& healthy, const HttpRequest& request) = 0;

    // Name as used in the config file, e.g. "round_robin".
    virtual const char* name() const = 0;
};

// Creates the strategy for a config name: "round_robin", "least_connections"
// or "weighted". Throws std::invalid_argument for anything else.
std::unique_ptr<Strategy> makeStrategy(const std::string& name);

} // namespace minilb
