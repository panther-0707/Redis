// Integration tests: a real miniLB process in front of real demo_backend
// processes and (where needed) a real redis-server, all on free local ports.
//
// Every test starts its own set of processes and stops them afterwards. When
// miniLB is stopped, its exit code must be 0. Under ThreadSanitizer /
// AddressSanitizer a detected problem makes the process exit non-zero, so
// these tests double as the sanitizer run for the whole request path.
//
// Tests that need Redis are skipped if `redis-server` is not installed,
// unless MINILB_REQUIRE_REDIS=1 is set (CI sets it), in which case a missing
// Redis is a failure.

#include "miniLB/health/HealthChecker.hpp"
#include "miniLB/http/HttpParser.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace std::chrono_literals;

namespace {

// ============================ Small helpers =================================

// Polls `condition` until it is true or `timeout` has passed.
bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return condition();
}

// Opens a TCP connection to 127.0.0.1:port. Returns the fd, or -1.
// Reads on the socket time out after 20 s so a hung server fails the test
// instead of hanging it.
int connectTo(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    timeval timeout{20, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}

bool canConnect(int port) {
    const int fd = connectTo(port);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
}

// Asks the kernel for a currently unused port, and never returns the same
// port twice in one run of this test binary.
//
// The kernel picks at random, and because the probe socket is closed again
// right away it can hand out the same number on a later call (about once in
// 14,000 calls on Linux). Two of our processes would then fight over one
// port: for example the load balancer fails to bind while a backend is
// already listening there, and the test ends up talking to the backend.
// Remembering what was handed out rules that out.
int freePort() {
    static std::set<int> handedOut;
    for (;;) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;   // 0 = "any free port"
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        socklen_t length = sizeof(address);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
        ::close(fd);

        const int port = ntohs(address.sin_port);
        if (port != 0 && handedOut.insert(port).second) {
            return port;   // not seen before
        }
    }
}

// Aborts the current test with a message. Used by the fixture's start-up
// helpers: a fatal ASSERT_* inside a helper function only returns from that
// helper, and the test would carry on against a half-started setup.
// An exception ends the test at once (gtest reports it and still runs
// TearDown, which prints the miniLB log).
[[noreturn]] void failSetup(const std::string& message) {
    throw std::runtime_error("test setup failed: " + message);
}

// ============================ Tiny HTTP client ==============================

// Sends `bytes` to 127.0.0.1:port and returns everything received until the
// server closes the connection. Empty string if the connection failed.
std::string rawExchange(int port, const std::string& bytes) {
    const int fd = connectTo(port);
    if (fd < 0) {
        return "";
    }
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(n);
    }
    std::string received;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        received.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return received;
}

struct Reply {
    int status = 0;   // 0 = no valid HTTP response received
    minilb::HeaderList headers;
    std::string body;

    // Header value, or "" if the header is absent.
    std::string header(const std::string& name) const {
        const std::string* value = minilb::findHeader(headers, name);
        return value ? *value : "";
    }
};

Reply parseReply(const std::string& raw) {
    Reply reply;
    minilb::HttpResponseParser parser(64 * 1024 * 1024);
    parser.feed(raw);
    if (parser.finish() == minilb::ParseStatus::Complete) {
        reply.status = parser.response().status;
        reply.headers = parser.response().headers;
        reply.body = parser.response().body;
    }
    return reply;
}

Reply httpGet(int port, const std::string& target, const std::string& extraHeaders = "") {
    return parseReply(rawExchange(
        port, "GET " + target + " HTTP/1.1\r\nHost: test.local\r\n" + extraHeaders + "\r\n"));
}

Reply httpPost(int port, const std::string& target, const std::string& body) {
    return parseReply(rawExchange(port, "POST " + target +
                                            " HTTP/1.1\r\nHost: test.local\r\nContent-Length: " +
                                            std::to_string(body.size()) + "\r\n\r\n" + body));
}

// Reads `"key": <number>` out of the /stats JSON. -1 if the key is missing.
long statNumber(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\": ";
    const std::size_t position = json.find(needle);
    if (position == std::string::npos) {
        return -1;
    }
    return std::strtol(json.c_str() + position + needle.size(), nullptr, 10);
}

// A std::thread that is joined when it goes out of scope.
//
// A plain std::thread calls std::terminate() if it is destroyed while still
// joinable. That is exactly what happens when a fatal ASSERT_* returns from a
// test before the join: the whole test binary aborts and the real failure
// message (and the miniLB log printed by TearDown) is lost.
class ScopedThread {
public:
    explicit ScopedThread(std::function<void()> body) : thread_(std::move(body)) {}
    ~ScopedThread() { join(); }
    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::thread thread_;
};

// ============================ Child processes ===============================

// A child process that is killed when the object goes out of scope.
class Process {
public:
    Process() = default;
    ~Process() { kill(); }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Starts args[0] (searched on PATH) with stdout and stderr appended to
    // `logFile`. Returns false if the program could not be started.
    bool start(const std::vector<std::string>& args, const std::string& logFile) {
        kill();

        std::vector<char*> argv;
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logFile.c_str(),
                                         O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

        pid_t pid = -1;
        const int error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (error != 0) {
            return false;
        }
        pid_ = pid;
        return true;
    }

    // True while we hold a process that has not been waited for yet.
    bool running() const { return pid_ > 0; }

    // Has the process exited by itself (e.g. it could not bind its port)?
    bool exitedEarly() {
        if (pid_ <= 0) {
            return true;
        }
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;   // already reaped
            return true;
        }
        return false;
    }

    // Sends SIGTERM and waits for the process to exit.
    // Returns its exit code, -1 if it died from a signal, -2 if it had to be
    // killed because it did not exit in time.
    int terminate(std::chrono::seconds timeout = 30s) {
        if (pid_ <= 0) {
            return 0;
        }
        ::kill(pid_, SIGTERM);

        int status = 0;
        const bool exited = waitUntil([&] { return ::waitpid(pid_, &status, WNOHANG) == pid_; }, timeout);
        if (!exited) {
            kill();
            return -2;
        }
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    // SIGKILL: the process gets no chance to clean up (simulates a crash).
    void kill() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }

private:
    pid_t pid_ = -1;
};

// A "backend" that answers every connection with bytes that are not HTTP.
class GarbageServer {
public:
    GarbageServer() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listenFd_, 16);
        socklen_t length = sizeof(address);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);

        thread_ = std::thread([this] {
            for (;;) {
                const int client = ::accept(listenFd_, nullptr, nullptr);
                if (client < 0) {
                    return;   // listening socket was shut down
                }
                char buffer[1024];
                (void)!::recv(client, buffer, sizeof(buffer), 0);   // read the request
                const char reply[] = "this is definitely not HTTP\r\n\r\n";
                (void)!::send(client, reply, sizeof(reply) - 1, MSG_NOSIGNAL);
                ::close(client);
            }
        });
    }

    ~GarbageServer() {
        ::shutdown(listenFd_, SHUT_RDWR);   // makes accept() return with an error
        thread_.join();
        ::close(listenFd_);
    }

    int port() const { return port_; }

private:
    int listenFd_ = -1;
    int port_ = 0;
    std::thread thread_;
};

// ================================ Fixture ===================================

struct LbOptions {
    std::string algorithm = "round_robin";
    std::vector<int> weights;          // one per backend; empty = all 1
    bool cache = false;
    int cacheTtlSeconds = 30;
    int rateLimit = 0;                 // 0 = disabled
    int healthIntervalMs = 100;
    // High by default so that a hiccup on a busy CI machine cannot take a
    // backend out of rotation in tests that are not about health checking.
    // The failover tests lower it.
    int failThreshold = 100;
    int successThreshold = 2;
    int connectTimeoutMs = 1000;
    int readTimeoutMs = 5000;
};

class MiniLbTest : public ::testing::Test {
protected:
    void SetUp() override {
        char pattern[] = "/tmp/minilb-it-XXXXXX";
        ASSERT_NE(::mkdtemp(pattern), nullptr);
        dir_ = pattern;
        lbPort_ = freePort();
        redisPort_ = freePort();   // stays closed unless startRedis() is called
    }

    void TearDown() override {
        // Stop miniLB first and insist on a clean exit: this is where a
        // sanitizer report (or a crash) inside miniLB fails the test.
        if (lb_.running()) {
            EXPECT_EQ(lb_.terminate(), 0) << "miniLB did not exit cleanly";
        }
        backends_.clear();
        redis_.kill();

        if (HasFailure()) {
            std::ifstream log(dir_ + "/lb.log");
            std::cerr << "---- miniLB log ----\n" << log.rdbuf() << "--------------------\n";
        }
        const std::string command = "rm -rf '" + dir_ + "'";
        (void)!std::system(command.c_str());
    }

    // Starts `count` demo backends named backend-1 .. backend-N on free ports.
    void startBackends(int count) {
        for (int i = 0; i < count; ++i) {
            backendPorts_.push_back(freePort());
            backends_.push_back(std::make_unique<Process>());
            startBackend(i);
        }
    }

    // (Re)starts backend number `index` on its port and waits until it
    // answers. Asking for "/" and checking the name in the reply makes sure
    // it really is *this* backend that is listening on the port.
    void startBackend(int index) {
        const int port = backendPorts_[index];
        const std::string name = "backend-" + std::to_string(index + 1);
        Process& process = *backends_[index];

        if (!process.start({DEMO_BACKEND_BIN, "--port", std::to_string(port), "--name", name},
                           dir_ + "/backend.log")) {
            failSetup("could not start " + name);
        }
        const bool up = waitUntil([&] {
            return process.exitedEarly() ||
                   httpGet(port, "/").body == "Hello from " + name + "!\n";
        });
        if (!up || process.exitedEarly()) {
            failSetup(name + " did not come up on port " + std::to_string(port));
        }
    }

    // Adds a backend address without starting a process there.
    void addBackendPort(int port) {
        backendPorts_.push_back(port);
        backends_.push_back(std::make_unique<Process>());
    }

    // Starts a private redis-server. Returns false if it is not installed.
    bool startRedis() {
        if (!redis_.start({"redis-server", "--port", std::to_string(redisPort_), "--save", "",
                           "--appendonly", "no", "--bind", "127.0.0.1"},
                          dir_ + "/redis.log")) {
            return false;
        }
        return waitUntil([&] { return canConnect(redisPort_); }, 5s);
    }

    void startLb(const LbOptions& options = {}) {
        std::ostringstream yaml;
        yaml << "listen_port: " << lbPort_ << "\n"
             << "algorithm: " << options.algorithm << "\n"
             << "threads: 4\n"
             << "log_level: info\n"
             << "backends:\n";
        for (std::size_t i = 0; i < backendPorts_.size(); ++i) {
            const int weight = i < options.weights.size() ? options.weights[i] : 1;
            yaml << "  - { host: 127.0.0.1, port: " << backendPorts_[i] << ", weight: " << weight
                 << " }\n";
        }
        yaml << "health_check: { path: /health, interval_ms: " << options.healthIntervalMs
             << ", timeout_ms: 500, fail_threshold: " << options.failThreshold
             << ", success_threshold: " << options.successThreshold << " }\n"
             // A one-hour window: the tests never straddle a window boundary.
             << "rate_limit: { requests: " << options.rateLimit << ", window_seconds: 3600 }\n"
             << "cache: { enabled: " << (options.cache ? "true" : "false")
             << ", ttl_seconds: " << options.cacheTtlSeconds << " }\n"
             << "timeouts_ms: { connect: " << options.connectTimeoutMs
             << ", read: " << options.readTimeoutMs << ", write: 5000 }\n"
             << "redis: { host: 127.0.0.1, port: " << redisPort_ << ", timeout_ms: 500 }\n";

        const std::string configPath = dir_ + "/config.yaml";
        {
            std::ofstream file(configPath);
            file << yaml.str();
        }
        if (!lb_.start({MINILB_BIN, "--config", configPath}, dir_ + "/lb.log")) {
            failSetup("could not start miniLB");
        }
        // Wait for /stats rather than for "the port accepts connections":
        // only miniLB answers /stats, so this cannot be fooled by some other
        // process that happens to listen on the port.
        const bool up = waitUntil(
            [&] { return lb_.exitedEarly() || httpGet(lbPort_, "/stats").status == 200; }, 30s);
        if (!up || lb_.exitedEarly()) {
            failSetup("miniLB did not come up on port " + std::to_string(lbPort_));
        }
    }

    std::string backendId(int index) const {
        return "127.0.0.1:" + std::to_string(backendPorts_[index]);
    }

    std::string stats() const { return httpGet(lbPort_, "/stats").body; }

    // Is the backend reported as healthy by /stats?
    bool reportedHealthy(int index) const {
        return stats().find("{\"id\": \"" + backendId(index) + "\", \"healthy\": true") !=
               std::string::npos;
    }

    // In-flight request count of a backend according to /stats.
    long activeOn(int index) const {
        const std::string json = stats();
        const std::size_t start = json.find("{\"id\": \"" + backendId(index) + "\"");
        if (start == std::string::npos) {
            return -1;
        }
        return statNumber(json.substr(start), "active_connections");
    }

    std::string dir_;
    int lbPort_ = 0;
    int redisPort_ = 0;
    std::vector<int> backendPorts_;
    std::vector<std::unique_ptr<Process>> backends_;
    Process redis_;
    Process lb_;
};

// Starts Redis or skips the test (fails it if MINILB_REQUIRE_REDIS=1).
#define REQUIRE_REDIS()                                                              \
    do {                                                                             \
        if (!startRedis()) {                                                         \
            if (std::getenv("MINILB_REQUIRE_REDIS") != nullptr) {                    \
                FAIL() << "redis-server is required but could not be started";       \
            }                                                                        \
            GTEST_SKIP() << "redis-server not found on PATH";                        \
        }                                                                            \
    } while (false)

// =============================== Balancing ==================================

TEST_F(MiniLbTest, RoundRobinRotatesAcrossBackends) {
    startBackends(3);
    startLb();

    std::vector<std::string> bodies;
    std::map<std::string, int> perBackend;
    for (int i = 0; i < 9; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        EXPECT_EQ(reply.header("X-Cache"), "BYPASS") << "cache is disabled in this test";
        bodies.push_back(reply.body);
        ++perBackend[reply.header("X-Backend")];
    }

    // Strict rotation: 1, 2, 3, 1, 2, 3, ...
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(bodies[i], "Hello from backend-" + std::to_string(i % 3 + 1) + "!\n");
    }
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(perBackend[backendId(i)], 3);
    }
}

TEST_F(MiniLbTest, WeightedSendsProportionalTraffic) {
    startBackends(3);
    LbOptions options;
    options.algorithm = "weighted";
    options.weights = {1, 1, 2};
    startLb(options);

    std::map<std::string, int> perBackend;
    for (int i = 0; i < 16; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        ++perBackend[reply.header("X-Backend")];
    }
    EXPECT_EQ(perBackend[backendId(0)], 4);
    EXPECT_EQ(perBackend[backendId(1)], 4);
    EXPECT_EQ(perBackend[backendId(2)], 8);
}

TEST_F(MiniLbTest, LeastConnectionsAvoidsTheBusyBackend) {
    startBackends(2);
    LbOptions options;
    options.algorithm = "least_connections";
    startLb(options);

    // Occupy one backend with a slow (2 s) request...
    Reply slowReply;
    ScopedThread slow([&] { slowReply = httpGet(lbPort_, "/slow"); });
    ASSERT_TRUE(waitUntil([&] { return activeOn(0) + activeOn(1) == 1; })) << stats();
    const int busy = activeOn(0) == 1 ? 0 : 1;

    // ...then every fast request must go to the other one.
    for (int i = 0; i < 6; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        EXPECT_EQ(reply.header("X-Backend"), backendId(1 - busy));
    }

    slow.join();
    EXPECT_EQ(slowReply.status, 200);
    EXPECT_EQ(slowReply.header("X-Backend"), backendId(busy));
}

TEST_F(MiniLbTest, ForwardsPostBodies) {
    startBackends(1);
    startLb();

    const std::string body(50000, 'x');   // larger than one read buffer
    const Reply reply = httpPost(lbPort_, "/echo", body);
    EXPECT_EQ(reply.status, 200);
    EXPECT_EQ(reply.body, body);
}

TEST_F(MiniLbTest, PassesBackendStatusCodesThrough) {
    startBackends(1);
    startLb();

    const Reply reply = httpGet(lbPort_, "/does-not-exist");
    EXPECT_EQ(reply.status, 404);
    EXPECT_EQ(reply.header("X-Backend"), backendId(0));
}

TEST_F(MiniLbTest, HeadRequestGetsHeadersWithoutBody) {
    startBackends(1);
    startLb();

    const std::string raw = rawExchange(lbPort_, "HEAD / HTTP/1.1\r\nHost: test.local\r\n\r\n");
    EXPECT_EQ(raw.rfind("HTTP/1.1 200 OK\r\n", 0), 0u) << raw;
    EXPECT_NE(raw.find("Content-Length: 22\r\n"), std::string::npos) << raw;
    EXPECT_EQ(raw.substr(raw.size() - 4), "\r\n\r\n") << "no body after the headers";
}

// ================================= Cache ====================================

TEST_F(MiniLbTest, CacheMissThenHit) {
    REQUIRE_REDIS();
    startBackends(3);
    LbOptions options;
    options.cache = true;
    startLb(options);

    const Reply first = httpGet(lbPort_, "/");
    ASSERT_EQ(first.status, 200);
    EXPECT_EQ(first.header("X-Cache"), "MISS");
    EXPECT_EQ(first.header("X-Backend"), backendId(0));

    // The response is written to Redis in the background right after the
    // first reply, so give that a moment before asking again.
    std::this_thread::sleep_for(300ms);

    for (int i = 0; i < 2; ++i) {
        const Reply cached = httpGet(lbPort_, "/");
        EXPECT_EQ(cached.status, 200);
        EXPECT_EQ(cached.header("X-Cache"), "HIT");
        // Round robin would have picked backend-2 and backend-3 next, so an
        // identical body proves the answer came from the cache.
        EXPECT_EQ(cached.body, first.body);
        EXPECT_EQ(cached.header("X-Backend"), "") << "no backend was contacted";
    }

    // A different query string is a different cache entry.
    EXPECT_EQ(httpGet(lbPort_, "/?page=2").header("X-Cache"), "MISS");

    const std::string json = stats();
    EXPECT_EQ(statNumber(json, "cache_hits"), 2);
    EXPECT_EQ(statNumber(json, "cache_misses"), 2);
}

TEST_F(MiniLbTest, CacheMakesSlowEndpointFast) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.cache = true;
    startLb(options);

    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(httpGet(lbPort_, "/slow").status, 200);
    const auto firstDuration = std::chrono::steady_clock::now() - start;
    EXPECT_GE(firstDuration, 1900ms) << "the backend sleeps 2 s";

    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/slow").header("X-Cache") == "HIT"; }));

    const auto again = std::chrono::steady_clock::now();
    const Reply cached = httpGet(lbPort_, "/slow");
    const auto cachedDuration = std::chrono::steady_clock::now() - again;
    EXPECT_EQ(cached.header("X-Cache"), "HIT");
    EXPECT_LT(cachedDuration, 1000ms);
}

TEST_F(MiniLbTest, CacheEntriesExpireAfterTheTtl) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.cache = true;
    options.cacheTtlSeconds = 1;
    startLb(options);

    EXPECT_EQ(httpGet(lbPort_, "/").header("X-Cache"), "MISS");
    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/").header("X-Cache") == "HIT"; }, 900ms));

    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(httpGet(lbPort_, "/").header("X-Cache"), "MISS");
}

TEST_F(MiniLbTest, OnlyCacheableRequestsUseTheCache) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.cache = true;
    startLb(options);

    // POST is never cached.
    EXPECT_EQ(httpPost(lbPort_, "/echo", "a").header("X-Cache"), "BYPASS");
    EXPECT_EQ(httpPost(lbPort_, "/echo", "b").body, "b");

    // Requests with credentials are never cached.
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(httpGet(lbPort_, "/", "Authorization: Bearer secret\r\n").header("X-Cache"),
                  "BYPASS");
    }

    // Error responses are not stored: a 404 stays a MISS.
    for (int i = 0; i < 3; ++i) {
        const Reply reply = httpGet(lbPort_, "/missing");
        EXPECT_EQ(reply.status, 404);
        EXPECT_EQ(reply.header("X-Cache"), "MISS");
        std::this_thread::sleep_for(50ms);
    }
}

// ============================== Rate limiting ===============================

TEST_F(MiniLbTest, RateLimitReturns429AfterTheLimit) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.rateLimit = 5;
    startLb(options);

    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(httpGet(lbPort_, "/").status, 200) << "request " << i + 1;
    }
    for (int i = 0; i < 3; ++i) {
        const Reply limited = httpGet(lbPort_, "/");
        EXPECT_EQ(limited.status, 429);
        EXPECT_FALSE(limited.header("Retry-After").empty());
    }

    // /stats is exempt from rate limiting and reports the rejections.
    const Reply statsReply = httpGet(lbPort_, "/stats");
    EXPECT_EQ(statsReply.status, 200);
    EXPECT_EQ(statNumber(statsReply.body, "rate_limited"), 3);
    EXPECT_EQ(statNumber(statsReply.body, "total_requests"), 8);
}

// ========================= Health checks and failover =======================

TEST_F(MiniLbTest, FailoverWhenABackendDiesAndRecovers) {
    startBackends(3);
    LbOptions options;
    options.failThreshold = 2;   // probes every 100 ms: 2 failures -> unhealthy,
                                 // 2 successes -> healthy again
    startLb(options);
    ASSERT_TRUE(reportedHealthy(1));

    // Crash backend 2.
    backends_[1]->kill();

    // Clients must not notice: a request that hits the dead backend is
    // retried on another one.
    for (int i = 0; i < 12; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200) << "request " << i;
        EXPECT_NE(reply.header("X-Backend"), backendId(1));
    }

    // It is taken out of rotation...
    ASSERT_TRUE(waitUntil([&] { return !reportedHealthy(1); })) << stats();
    EXPECT_TRUE(reportedHealthy(0));
    EXPECT_TRUE(reportedHealthy(2));

    // ...and comes back once it answers health checks again.
    startBackend(1);
    ASSERT_TRUE(waitUntil([&] { return reportedHealthy(1); })) << stats();

    std::set<std::string> seen;
    for (int i = 0; i < 9; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        seen.insert(reply.header("X-Backend"));
    }
    EXPECT_EQ(seen.count(backendId(1)), 1u) << "recovered backend receives traffic again";
    EXPECT_EQ(seen.size(), 3u);
}

TEST_F(MiniLbTest, Returns503WhenNoBackendIsHealthy) {
    addBackendPort(freePort());   // nothing listens there
    LbOptions options;
    options.failThreshold = 2;
    startLb(options);

    // Until the health checker has seen enough failures the backend still
    // counts as healthy and requests fail with 502; afterwards it is 503.
    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/").status == 503; }));
    EXPECT_FALSE(reportedHealthy(0));
    EXPECT_GE(statNumber(stats(), "errors"), 1);
}

TEST_F(MiniLbTest, Returns502WhenTheOnlyBackendIsUnreachable) {
    addBackendPort(freePort());   // nothing listens there
    LbOptions options;
    options.healthIntervalMs = 60000;   // keep the health checker out of the picture
    options.failThreshold = 100;
    startLb(options);

    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);
}

TEST_F(MiniLbTest, Returns502OnInvalidBackendResponse) {
    GarbageServer garbage;
    addBackendPort(garbage.port());
    LbOptions options;
    options.healthIntervalMs = 60000;
    options.failThreshold = 100;
    startLb(options);

    EXPECT_EQ(httpGet(lbPort_, "/").status, 502);
}

TEST_F(MiniLbTest, Returns504WhenTheBackendIsTooSlow) {
    startBackends(1);
    LbOptions options;
    options.readTimeoutMs = 300;   // /slow takes 2 s
    options.failThreshold = 100;
    startLb(options);

    const auto start = std::chrono::steady_clock::now();
    const Reply reply = httpGet(lbPort_, "/slow");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(reply.status, 504);
    EXPECT_GE(elapsed, 300ms);
    EXPECT_LT(elapsed, 1900ms) << "the timeout must fire long before the backend answers";

    // Fast requests still work afterwards.
    EXPECT_EQ(httpGet(lbPort_, "/").status, 200);
}

TEST_F(MiniLbTest, RetriesOnceOnAnotherBackend) {
    startBackends(1);
    addBackendPort(freePort());   // second backend is dead from the start
    LbOptions options;
    options.healthIntervalMs = 60000;   // only the retry logic may save us
    options.failThreshold = 100;
    startLb(options);

    // Round robin alternates between the live and the dead backend; every
    // request must succeed anyway.
    for (int i = 0; i < 6; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200) << "request " << i;
        EXPECT_EQ(reply.header("X-Backend"), backendId(0));
    }
}

TEST_F(MiniLbTest, HealthProbeDistinguishesGoodAndBadBackends) {
    startBackends(1);
    using minilb::HealthChecker;

    EXPECT_TRUE(HealthChecker::probe("127.0.0.1", static_cast<unsigned short>(backendPorts_[0]),
                                     "/health", 2s));
    EXPECT_TRUE(HealthChecker::probe("localhost", static_cast<unsigned short>(backendPorts_[0]),
                                     "/health", 2s))
        << "host names are resolved";
    EXPECT_FALSE(HealthChecker::probe("127.0.0.1", static_cast<unsigned short>(backendPorts_[0]),
                                      "/does-not-exist", 2s))
        << "a 404 is not healthy";
    EXPECT_FALSE(
        HealthChecker::probe("127.0.0.1", static_cast<unsigned short>(freePort()), "/health", 500ms))
        << "connection refused is not healthy";

    GarbageServer garbage;
    EXPECT_FALSE(HealthChecker::probe("127.0.0.1", static_cast<unsigned short>(garbage.port()),
                                      "/health", 500ms));
}

// ============================== Bad requests ================================

TEST_F(MiniLbTest, MalformedRequestsGet400) {
    startBackends(1);
    startLb();

    EXPECT_EQ(parseReply(rawExchange(lbPort_, "THIS IS NOT HTTP\r\n\r\n")).status, 400);
    EXPECT_EQ(parseReply(rawExchange(lbPort_, "GET / HTTP/9.9\r\n\r\n")).status, 400);
    EXPECT_EQ(parseReply(rawExchange(lbPort_, "GET / HTTP/1.1\r\nBroken Header\r\n\r\n")).status, 400);

    // The server is still fine afterwards and counted the errors.
    EXPECT_EQ(httpGet(lbPort_, "/").status, 200);
    EXPECT_EQ(statNumber(stats(), "errors"), 3);
}

TEST_F(MiniLbTest, OversizedRequestsAreRejected) {
    startBackends(1);
    startLb();

    // Declared body larger than the 1 MiB cap: refused from the headers alone.
    EXPECT_EQ(parseReply(rawExchange(lbPort_,
                                     "POST /echo HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n"))
                  .status,
              413);

    // Headers larger than the 16 KiB cap.
    const std::string hugeHeader =
        "GET / HTTP/1.1\r\nX-Padding: " + std::string(17000, 'a') + "\r\n\r\n";
    EXPECT_EQ(parseReply(rawExchange(lbPort_, hugeHeader)).status, 431);

    // Chunked request bodies are not implemented.
    EXPECT_EQ(parseReply(rawExchange(lbPort_,
                                     "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"))
                  .status,
              501);
}

// ============================ Redis unavailable =============================

TEST_F(MiniLbTest, FailsOpenWhenRedisIsDown) {
    startBackends(2);
    LbOptions options;
    options.cache = true;
    options.rateLimit = 3;   // would reject the 4th request if Redis were up
    startLb(options);        // note: no startRedis()

    for (int i = 0; i < 10; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200) << "request " << i;
        EXPECT_EQ(reply.header("X-Cache"), "MISS");
    }

    const std::string json = stats();
    EXPECT_NE(json.find("\"redis_available\": false"), std::string::npos) << json;
    EXPECT_EQ(statNumber(json, "rate_limited"), 0);
    EXPECT_EQ(statNumber(json, "cache_hits"), 0);
}

TEST_F(MiniLbTest, SurvivesRedisGoingAwayAndComingBack) {
    REQUIRE_REDIS();
    startBackends(1);
    LbOptions options;
    options.cache = true;
    startLb(options);

    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/").header("X-Cache") == "HIT"; }));

    redis_.kill();
    for (int i = 0; i < 5; ++i) {
        const Reply reply = httpGet(lbPort_, "/");
        ASSERT_EQ(reply.status, 200);
        EXPECT_EQ(reply.header("X-Cache"), "MISS");
    }

    // A fresh (empty) Redis on the same port: caching resumes by itself.
    ASSERT_TRUE(startRedis());
    ASSERT_TRUE(waitUntil([&] { return httpGet(lbPort_, "/").header("X-Cache") == "HIT"; }));
}

// ================================ /stats ====================================

TEST_F(MiniLbTest, StatsReportsCountersAndBackends) {
    startBackends(2);
    startLb();

    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(httpGet(lbPort_, "/").status, 200);
    }

    const Reply reply = httpGet(lbPort_, "/stats");
    ASSERT_EQ(reply.status, 200);
    EXPECT_EQ(reply.header("Content-Type"), "application/json");

    const std::string& json = reply.body;
    EXPECT_EQ(statNumber(json, "total_requests"), 4) << "/stats itself is not counted";
    EXPECT_GE(statNumber(json, "active_connections"), 1) << "at least the /stats request itself";
    EXPECT_EQ(statNumber(json, "errors"), 0);
    EXPECT_NE(json.find("\"algorithm\": \"round_robin\""), std::string::npos);
    for (int i = 0; i < 2; ++i) {
        EXPECT_NE(json.find("{\"id\": \"" + backendId(i) +
                            "\", \"healthy\": true, \"weight\": 1, \"active_connections\": 0, "
                            "\"requests\": 2}"),
                  std::string::npos)
            << json;
    }
}

// ========================= Concurrency and shutdown =========================

TEST_F(MiniLbTest, HandlesManyConcurrentClients) {
    REQUIRE_REDIS();
    startBackends(3);
    LbOptions options;
    options.cache = true;         // exercises the Redis thread hand-off too
    options.rateLimit = 1000000;
    startLb(options);

    constexpr int kThreads = 8;
    constexpr int kRequestsPerThread = 40;
    std::atomic<int> ok{0};
    std::atomic<int> wrongBody{0};

    std::vector<std::thread> clients;
    for (int t = 0; t < kThreads; ++t) {
        clients.emplace_back([&, t] {
            for (int i = 0; i < kRequestsPerThread; ++i) {
                if (i % 2 == 0) {
                    // 10 distinct URLs shared by all threads: a mix of hits and misses.
                    const Reply reply = httpGet(lbPort_, "/?item=" + std::to_string(i % 20));
                    if (reply.status == 200) {
                        ++ok;
                    }
                } else {
                    const std::string body = "client-" + std::to_string(t) + "-" + std::to_string(i);
                    const Reply reply = httpPost(lbPort_, "/echo", body);
                    if (reply.status == 200) {
                        ++ok;
                    }
                    if (reply.body != body) {
                        ++wrongBody;   // responses must never be mixed up between clients
                    }
                }
            }
        });
    }
    for (auto& client : clients) {
        client.join();
    }

    EXPECT_EQ(ok.load(), kThreads * kRequestsPerThread);
    EXPECT_EQ(wrongBody.load(), 0);

    const std::string json = stats();
    EXPECT_EQ(statNumber(json, "total_requests"), kThreads * kRequestsPerThread);
    EXPECT_EQ(statNumber(json, "cache_hits") + statNumber(json, "cache_misses"),
              kThreads * kRequestsPerThread / 2)
        << "every GET is either a hit or a miss";
    EXPECT_EQ(statNumber(json, "errors"), 0);
}

TEST_F(MiniLbTest, GracefulShutdownLetsInFlightRequestsFinish) {
    startBackends(1);
    startLb();

    Reply slowReply;
    ScopedThread slow([&] { slowReply = httpGet(lbPort_, "/slow"); });
    ASSERT_TRUE(waitUntil([&] { return activeOn(0) == 1; })) << stats();

    // SIGTERM while the request is in flight.
    const auto start = std::chrono::steady_clock::now();
    const int exitCode = lb_.terminate();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    slow.join();

    EXPECT_EQ(exitCode, 0);
    EXPECT_EQ(slowReply.status, 200) << "the in-flight request was allowed to finish";
    EXPECT_EQ(slowReply.body, "Slow hello from backend-1!\n");
    EXPECT_GE(elapsed, 1000ms) << "shutdown waited for the request";

    // New connections are refused once shutdown has begun.
    EXPECT_FALSE(canConnect(lbPort_));
}

} // namespace
