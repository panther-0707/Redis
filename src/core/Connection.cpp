#include "miniLB/core/Connection.hpp"

#include "miniLB/util/Logger.hpp"

namespace minilb {

Connection::Connection(tcp::socket client, Context context)
    : ctx_(context),
      client_(std::move(client)),
      // Sharing the client's executor (a strand) puts every handler of this
      // connection on the same strand.
      backendSocket_(client_.get_executor()),
      resolver_(client_.get_executor()),
      timer_(client_.get_executor()),
      requestParser_(HttpParser::Limits{ctx_.config.limits.maxRequestHeaderBytes,
                                        ctx_.config.limits.maxRequestBodyBytes}),
      startTime_(std::chrono::steady_clock::now()) {
    ++ctx_.metrics.activeConnections;

    boost::system::error_code ec;
    const tcp::endpoint remote = client_.remote_endpoint(ec);
    clientIp_ = ec ? "unknown" : remote.address().to_string();
}

Connection::~Connection() {
    --ctx_.metrics.activeConnections;
}

// ============================ 1. Read the request ===========================

void Connection::start() {
    // One deadline for receiving the *whole* request. Without it a client
    // could send one byte every few seconds and hold the connection forever.
    startTimer(ctx_.config.timeouts.readMs, Side::Client);
    readRequest();
}

void Connection::readRequest() {
    auto self = shared_from_this();
    client_.async_read_some(
        asio::buffer(readBuffer_),
        [this, self](const boost::system::error_code& ec, std::size_t bytes) {
            if (ec) {
                // The client left, or the deadline closed the socket. Either
                // way there is nobody to answer.
                stopTimer();
                return;
            }
            switch (requestParser_.feed(std::string_view(readBuffer_.data(), bytes))) {
                case ParseStatus::NeedMore:
                    readRequest();
                    break;
                case ParseStatus::Error:
                    stopTimer();
                    replyError(requestParser_.errorStatus(),
                               requestParser_.errorMessage() + "\n");
                    break;
                case ParseStatus::Complete:
                    stopTimer();
                    onRequestComplete();
                    break;
            }
        });
}

// ===================== 2. Rate limit and cache lookup =======================

void Connection::onRequestComplete() {
    const HttpRequest& request = requestParser_.request();

    // Built-in endpoint: live metrics as JSON. Handled before rate limiting
    // so that monitoring keeps working while a client is being throttled.
    if (request.method == "GET" && request.path == "/stats") {
        statsRequest_ = true;
        reply(HttpResponse::make(200,
                                 ctx_.metrics.toJson(ctx_.pool, ctx_.strategy.name(),
                                                     ctx_.redis.available()),
                                 "application/json"));
        return;
    }

    cacheable_ = ctx_.cache.enabled() && Cache::isCacheableRequest(request);
    if (cacheable_) {
        cacheKey_ = Cache::makeKey(request);
    }
    if (!ctx_.rateLimiter.enabled() && !cacheable_) {
        forwardRequest();   // nothing to ask Redis
        return;
    }

    // Redis calls block, so they must not run on an io_context thread. Hand
    // them to the blocking pool, then hop back onto this connection's strand.
    //
    // The pool task runs on another thread, so it gets its own copies of the
    // data it needs and only uses thread-safe objects (RateLimiter, Cache).
    // Both lambdas hold `self`, which keeps the Connection alive meanwhile.
    auto self = shared_from_this();
    ctx_.blockingPool.submit([this, self, strand = client_.get_executor(), clientIp = clientIp_,
                              key = cacheKey_, lookup = cacheable_] {
        const RateLimiter::Decision decision = ctx_.rateLimiter.allow(clientIp);

        std::optional<HttpResponse> cached;
        if (decision.allowed && lookup) {
            cached = ctx_.cache.get(key);
        }

        asio::post(strand, [this, self, decision, cached = std::move(cached)]() mutable {
            onRedisResult(decision, std::move(cached));
        });
    });
}

// Back on the strand with the answers from Redis.
void Connection::onRedisResult(RateLimiter::Decision decision, std::optional<HttpResponse> cached) {
    if (!decision.allowed) {
        ++ctx_.metrics.rateLimited;
        HttpResponse response = HttpResponse::make(429, "Too many requests: rate limit exceeded\n");
        response.setHeader("Retry-After", std::to_string(decision.retryAfterSeconds));
        reply(response);
        return;
    }

    if (cached) {
        ++ctx_.metrics.cacheHits;
        cacheStatus_ = "HIT";
        cached->setHeader("Connection", "close");
        cached->setHeader("X-Cache", cacheStatus_);
        reply(*cached);
        return;
    }

    if (cacheable_) {
        ++ctx_.metrics.cacheMisses;
        cacheStatus_ = "MISS";
    }
    forwardRequest();
}

// ============================ 3. Pick a backend =============================

void Connection::forwardRequest() {
    HttpRequest& request = requestParser_.request();

    Backend* backend = ctx_.strategy.select(ctx_.pool.healthy(), request);
    if (backend == nullptr) {
        replyError(503, "Service unavailable: no healthy backends\n");
        return;
    }

    // Prepare the request for the backend. We use one request per
    // connection on both sides, hence "Connection: close".
    removeHopByHopHeaders(request.headers);
    setHeader(request.headers, "Connection", "close");

    // Tell the backend who the real client is (it only sees our address).
    const std::string* forwardedFor = request.header("X-Forwarded-For");
    setHeader(request.headers, "X-Forwarded-For",
              forwardedFor ? *forwardedFor + ", " + clientIp_ : clientIp_);

    backendRequest_ = request.serialize();
    connectToBackend(*backend);
}

// ====================== 4. Connect and send the request =====================

void Connection::connectToBackend(Backend& backend) {
    backend_ = &backend;
    lease_.emplace(backend);   // activeConnections +1 until the lease is reset
    ++backend.totalRequests;
    requestSent_ = false;

    startTimer(ctx_.config.timeouts.connectMs, Side::Backend);
    auto self = shared_from_this();

    // Fast path: the host is already an IP address, no DNS lookup needed.
    boost::system::error_code notAnIp;
    const asio::ip::address address = asio::ip::make_address(backend_->host, notAnIp);
    if (!notAnIp) {
        backendSocket_.async_connect(
            tcp::endpoint(address, backend_->port),
            [this, self](const boost::system::error_code& ec) { onBackendConnected(ec); });
        return;
    }

    // Otherwise resolve the name first (e.g. "backend1" under docker-compose).
    resolver_.async_resolve(
        backend_->host, std::to_string(backend_->port),
        [this, self](const boost::system::error_code& ec, tcp::resolver::results_type endpoints) {
            if (ec || timedOut_) {
                onBackendConnected(ec ? ec : asio::error::timed_out);
                return;
            }
            asio::async_connect(
                backendSocket_, endpoints,
                [this, self](const boost::system::error_code& connectEc, const tcp::endpoint&) {
                    onBackendConnected(connectEc);
                });
        });
}

void Connection::onBackendConnected(const boost::system::error_code& ec) {
    if (ec || timedOut_) {
        onBackendFailure("connect", ec.message());
        return;
    }

    requestSent_ = true;   // from here on the backend may have acted on the request
    startTimer(ctx_.config.timeouts.writeMs, Side::Backend);
    auto self = shared_from_this();
    asio::async_write(
        backendSocket_, asio::buffer(backendRequest_),
        [this, self](const boost::system::error_code& writeEc, std::size_t) {
            if (writeEc || timedOut_) {
                onBackendFailure("send request", writeEc.message());
                return;
            }
            // One deadline for receiving the whole response.
            responseParser_.emplace(ctx_.config.limits.maxResponseBytes,
                                    requestParser_.request().method == "HEAD");
            startTimer(ctx_.config.timeouts.readMs, Side::Backend);
            readBackendResponse();
        });
}

// ========================= 5. Read the response =============================

void Connection::readBackendResponse() {
    auto self = shared_from_this();
    backendSocket_.async_read_some(
        asio::buffer(readBuffer_),
        [this, self](const boost::system::error_code& ec, std::size_t bytes) {
            ParseStatus status;
            if (!ec) {
                status = responseParser_->feed(std::string_view(readBuffer_.data(), bytes));
            } else if (ec == asio::error::eof) {
                // The backend closed the connection: is the response whole?
                status = responseParser_->finish();
            } else {
                onBackendFailure("read response", ec.message());
                return;
            }

            switch (status) {
                case ParseStatus::NeedMore:
                    readBackendResponse();
                    break;
                case ParseStatus::Error:
                    onBackendFailure("parse response", responseParser_->errorMessage());
                    break;
                case ParseStatus::Complete:
                    stopTimer();
                    closeBackendSocket();
                    lease_.reset();   // the backend's part is done
                    onBackendResponse();
                    break;
            }
        });
}

void Connection::onBackendFailure(const std::string& step, const std::string& detail) {
    const bool timedOut = timedOut_;   // read before stopTimer() changes anything
    stopTimer();
    closeBackendSocket();
    lease_.reset();

    spdlog::warn("backend {} failed during '{}': {}", backend_->id, step,
                 timedOut ? "timed out" : detail);

    // "Mark it suspect": a failed real request counts like a failed health
    // probe, so a dead backend is taken out of rotation without waiting for
    // several probe intervals.
    if (backend_->recordFailure(ctx_.config.healthCheck.failThreshold)) {
        spdlog::warn("health: backend {} is now UNHEALTHY (request failures)", backend_->id);
    }

    // Retry once, on a *different* healthy backend.
    if (canRetry()) {
        std::vector<Backend*> others;
        for (Backend* candidate : ctx_.pool.healthy()) {
            if (candidate != backend_) {
                others.push_back(candidate);
            }
        }
        if (Backend* next = ctx_.strategy.select(others, requestParser_.request())) {
            retried_ = true;
            spdlog::info("retrying request on backend {}", next->id);
            connectToBackend(*next);
            return;
        }
    }

    if (timedOut) {
        replyError(504, "Gateway timeout: backend did not answer in time\n");
    } else {
        replyError(502, "Bad gateway: backend unreachable or sent an invalid response\n");
    }
}

// =================== 6. Cache and answer the client ==========================

void Connection::onBackendResponse() {
    HttpResponse& response = responseParser_->response();
    removeHopByHopHeaders(response.headers);
    response.setHeader("Connection", "close");

    // Store a copy in Redis in the background; the client does not wait for
    // that. (The copy is taken before the X-* headers below are added.)
    if (cacheable_ && ctx_.cache.isCacheableResponse(response)) {
        ctx_.blockingPool.submit([&cache = ctx_.cache, key = cacheKey_, copy = response] {
            cache.put(key, copy);
        });
    }

    response.setHeader("X-Cache", cacheStatus_);
    response.setHeader("X-Backend", backend_->id);   // which backend answered
    reply(response);
}

void Connection::replyError(int status, const std::string& message) {
    ++ctx_.metrics.errors;
    reply(HttpResponse::make(status, message));
}

void Connection::reply(const HttpResponse& response) {
    if (!statsRequest_) {
        ++ctx_.metrics.totalRequests;
    }
    replyStatus_ = response.status;
    clientReply_ = response.serialize();

    startTimer(ctx_.config.timeouts.writeMs, Side::Client);
    auto self = shared_from_this();
    asio::async_write(client_, asio::buffer(clientReply_),
                      [this, self](const boost::system::error_code&, std::size_t) {
                          stopTimer();
                          boost::system::error_code ignored;
                          client_.shutdown(tcp::socket::shutdown_both, ignored);
                          client_.close(ignored);
                          logRequest();
                          // No further async operation is started, so the last
                          // shared_ptr goes away and the Connection is destroyed.
                      });
}

// ================================ Helpers ===================================

// Arms the (single) deadline timer. If it expires before stopTimer() or the
// next startTimer(), the socket on `side` is closed, which makes the pending
// operation on it fail immediately; its handler then sees timedOut_ == true.
//
// Why the generation number? When an operation finishes just as the timer
// expires, the timer's handler may already be queued and timer_.cancel()
// cannot take it back. Each (re)arm bumps the generation, so such a stale
// handler notices that it no longer belongs to the current deadline.
void Connection::startTimer(int milliseconds, Side side) {
    const unsigned generation = ++timerGeneration_;
    timedOut_ = false;

    auto self = shared_from_this();
    timer_.expires_after(std::chrono::milliseconds(milliseconds));
    timer_.async_wait([this, self, generation, side](const boost::system::error_code& ec) {
        if (ec || generation != timerGeneration_) {
            return;   // cancelled, or a newer deadline replaced this one
        }
        timedOut_ = true;
        boost::system::error_code ignored;
        if (side == Side::Client) {
            client_.close(ignored);
        } else {
            resolver_.cancel();
            backendSocket_.close(ignored);
        }
    });
}

void Connection::stopTimer() {
    ++timerGeneration_;
    timer_.cancel();
}

// Is it safe to send this request to another backend?
//   * If nothing was sent yet (connect failed), always.
//   * Otherwise only for idempotent methods: repeating a GET is harmless,
//     repeating a POST could e.g. place an order twice.
bool Connection::canRetry() const {
    if (retried_) {
        return false;   // only one retry per request
    }
    if (!requestSent_) {
        return true;
    }
    const std::string& method = requestParser_.request().method;
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "PUT" ||
           method == "DELETE";
}

void Connection::closeBackendSocket() {
    boost::system::error_code ignored;
    backendSocket_.close(ignored);
}

// One "access log" line per request.
void Connection::logRequest() const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime_);
    const HttpRequest& request = requestParser_.request();
    spdlog::info("{} \"{} {}\" -> {} backend={} cache={} {}ms", clientIp_,
                 request.method.empty() ? "-" : request.method,
                 request.target.empty() ? "-" : request.target, replyStatus_,
                 backend_ ? backend_->id : "-", cacheStatus_, elapsed.count());
}

} // namespace minilb
