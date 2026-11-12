#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace minilb {

// One upstream server that requests can be forwarded to.
//
// Identity (host, port, weight, id) never changes after construction, so it
// can be read from any thread without locking. The mutable state is stored
// in std::atomic members so that several threads (the networking threads
// and the health-checker thread) can update it safely without a mutex.
struct Backend {
    Backend(std::string hostName, unsigned short portNumber, int weightValue = 1)
        : host(std::move(hostName)),
          port(portNumber),
          weight(weightValue < 1 ? 1 : weightValue),
          id(host + ":" + std::to_string(port)) {}

    const std::string host;
    const unsigned short port;
    const int weight;         // relative share of traffic (weighted strategy), >= 1
    const std::string id;     // "host:port", used in logs and the X-Backend header

    // Is this backend currently eligible to receive traffic?
    std::atomic<bool> healthy{true};

    // Number of requests currently being proxied to this backend.
    std::atomic<int> activeConnections{0};

    // Number of requests ever forwarded to this backend (for /stats).
    std::atomic<std::uint64_t> totalRequests{0};

    // Health bookkeeping. Results come from two sources:
    //   * the HealthChecker's periodic probes (successes and failures), and
    //   * the proxy, which reports a failure when a real request to this
    //     backend fails ("marking it suspect").
    // A backend becomes unhealthy after `failThreshold` failures in a row and
    // healthy again after `successThreshold` successes in a row.
    //
    // Both return true if the call changed `healthy` (so the caller can log
    // the transition exactly once).
    bool recordSuccess(int successThreshold);
    bool recordFailure(int failThreshold);

private:
    std::atomic<int> consecutiveSuccesses_{0};
    std::atomic<int> consecutiveFailures_{0};
};

// RAII helper: counts one in-flight request against a backend for as long as
// the lease object exists. Using a destructor means the counter can never
// be forgotten on an early return or error path.
class BackendLease {
public:
    explicit BackendLease(Backend& backend) : backend_(backend) { ++backend_.activeConnections; }
    ~BackendLease() { --backend_.activeConnections; }

    BackendLease(const BackendLease&) = delete;
    BackendLease& operator=(const BackendLease&) = delete;

private:
    Backend& backend_;
};

} // namespace minilb
