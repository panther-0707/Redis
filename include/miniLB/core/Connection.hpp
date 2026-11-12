#pragma once

#include "miniLB/balancer/BackendPool.hpp"
#include "miniLB/balancer/Strategy.hpp"
#include "miniLB/core/ThreadPool.hpp"
#include "miniLB/http/HttpParser.hpp"
#include "miniLB/redis/Cache.hpp"
#include "miniLB/redis/RateLimiter.hpp"
#include "miniLB/util/Config.hpp"
#include "miniLB/util/Metrics.hpp"

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace minilb {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// The shared objects every connection needs. main() owns them and they
// outlive all connections, so plain references are safe.
struct Context {
    const Config& config;
    BackendPool& pool;
    Strategy& strategy;
    RateLimiter& rateLimiter;
    Cache& cache;
    const RedisClient& redis;      // only for reporting its state in /stats
    ThreadPool& blockingPool;      // runs the blocking Redis calls
    Metrics& metrics;
};

// One client connection = one proxied HTTP request, handled asynchronously.
// (GET /stats is the one request answered by miniLB itself.)
//
// The steps (each "->" is an asynchronous operation with a deadline):
//
//   start -> read request -> rate limit + cache lookup (Redis)
//         -> pick backend -> connect -> send request -> read response
//         -> store in cache (Redis) + send reply to client -> close
//
// The Redis client is blocking, so those calls are handed to a separate
// thread pool and the result is posted back to this connection's strand.
// The networking threads therefore never wait for Redis.
//
// If the backend fails or times out, it is reported to the health logic
// ("marked suspect") and the request is retried once on a different backend.
// If that is not possible the client gets 502 (failure) or 504 (timeout).
//
// Lifetime: the object is held by a shared_ptr. Every pending asynchronous
// operation captures a copy of that pointer (`self`), so the Connection
// stays alive exactly as long as something is still in flight, and is
// destroyed automatically afterwards. Nobody deletes it by hand.
//
// Thread-safety: the io_context runs on several threads, but all handlers of
// one Connection run on its own "strand" (an Asio executor that never runs
// two of its handlers at the same time). The members below are therefore
// only ever touched by one thread at a time and need no mutex.
class Connection : public std::enable_shared_from_this<Connection> {
public:
    // `client` must have been accepted onto its own strand (see Server).
    Connection(tcp::socket client, Context context);
    ~Connection();

    // Begins reading the request. Call once, right after construction.
    void start();

private:
    // Which socket a deadline applies to (the one to close when it expires).
    enum class Side { Client, Backend };

    // ---- The steps, in the order they happen ------------------------------
    void readRequest();
    void onRequestComplete();
    void onRedisResult(RateLimiter::Decision decision, std::optional<HttpResponse> cached);
    void forwardRequest();
    void connectToBackend(Backend& backend);
    void onBackendConnected(const boost::system::error_code& ec);
    void readBackendResponse();
    void onBackendResponse();
    void onBackendFailure(const std::string& step, const std::string& detail);
    void reply(const HttpResponse& response);
    void replyError(int status, const std::string& message);   // counts it in metrics.errors

    // ---- Deadlines ----------------------------------------------------------
    void startTimer(int milliseconds, Side side);
    void stopTimer();

    bool canRetry() const;
    void closeBackendSocket();
    void logRequest() const;

    Context ctx_;

    tcp::socket client_;
    tcp::socket backendSocket_;
    tcp::resolver resolver_;        // only used when a backend host is a DNS name
    asio::steady_timer timer_;      // one deadline at a time
    unsigned timerGeneration_ = 0;  // see startTimer()
    bool timedOut_ = false;         // set when the current deadline expired

    std::array<char, 8192> readBuffer_{};
    HttpParser requestParser_;
    std::optional<HttpResponseParser> responseParser_;   // created per backend attempt
    std::string backendRequest_;    // bytes sent to the backend (must outlive the write)
    std::string clientReply_;       // bytes sent to the client   (must outlive the write)

    Backend* backend_ = nullptr;    // the backend currently being tried
    std::optional<BackendLease> lease_;   // counts us in backend_->activeConnections
    bool requestSent_ = false;      // may the backend have seen (part of) the request?
    bool retried_ = false;          // the one allowed retry has been used

    bool cacheable_ = false;        // was the cache consulted for this request?
    std::string cacheKey_;
    const char* cacheStatus_ = "BYPASS";   // value of the X-Cache header: HIT, MISS or BYPASS
    bool statsRequest_ = false;     // GET /stats: answered locally, not counted as traffic

    // For the access log.
    std::string clientIp_;
    std::chrono::steady_clock::time_point startTime_;
    int replyStatus_ = 0;
};

} // namespace minilb
