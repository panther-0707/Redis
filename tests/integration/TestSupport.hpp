#pragma once

// Shared helpers for the integration tests: a tiny HTTP client, child-process
// management, scripted fake backends and the MiniLbTest fixture that starts a
// real miniLB in front of real demo_backend processes.
//
// This code used to live at the top of IntegrationTest.cpp. It was moved here
// unchanged (apart from `inline`) so that the tests in software-testing/ can
// use the same fixture.

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
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
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

namespace minilb_test {

using namespace std::chrono_literals;

// ============================ Small helpers =================================

// Polls `condition` until it is true or `timeout` has passed.
inline bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 10s) {
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
inline int connectTo(int port) {
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

inline bool canConnect(int port) {
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
inline int freePort() {
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
[[noreturn]] inline void failSetup(const std::string& message) {
    throw std::runtime_error("test setup failed: " + message);
}

// ============================ Tiny HTTP client ==============================

// Sends `bytes` to 127.0.0.1:port and returns everything received until the
// server closes the connection. Empty string if the connection failed.
inline std::string rawExchange(int port, const std::string& bytes) {
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

// `headRequest`: the reply answers a HEAD request, so it announces a
// Content-Length but carries no body.
inline Reply parseReply(const std::string& raw, bool headRequest = false) {
    Reply reply;
    minilb::HttpResponseParser parser(64 * 1024 * 1024, headRequest);
    parser.feed(raw);
    if (parser.finish() == minilb::ParseStatus::Complete) {
        reply.status = parser.response().status;
        reply.headers = parser.response().headers;
        reply.body = parser.response().body;
    }
    return reply;
}

inline Reply httpGet(int port, const std::string& target, const std::string& extraHeaders = "") {
    return parseReply(rawExchange(
        port, "GET " + target + " HTTP/1.1\r\nHost: test.local\r\n" + extraHeaders + "\r\n"));
}

inline Reply httpPost(int port, const std::string& target, const std::string& body) {
    return parseReply(rawExchange(port, "POST " + target +
                                            " HTTP/1.1\r\nHost: test.local\r\nContent-Length: " +
                                            std::to_string(body.size()) + "\r\n\r\n" + body));
}

// Reads `"key": <number>` out of the /stats JSON. -1 if the key is missing.
inline long statNumber(const std::string& json, const std::string& key) {
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

    // Waits for the process to exit *by itself* (no signal is sent).
    // Returns its exit code, -1 if it died from a signal, -2 if it was still
    // running after `timeout` (it is then killed).
    int waitForExit(std::chrono::seconds timeout = 10s) {
        if (pid_ <= 0) {
            return 0;
        }
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

// A "backend" that answers every connection with fixed bytes and then closes
// it. Lets a test decide exactly what miniLB receives.
//
// With several replies, connection number n (counting from 0, health probes
// included) gets replies[n % replies.size()], which scripts a backend that
// fails in a known pattern.
class CannedServer {
public:
    explicit CannedServer(std::string reply) : CannedServer(std::vector<std::string>{std::move(reply)}) {}

    explicit CannedServer(std::vector<std::string> replies) : replies_(std::move(replies)) {
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
                char buffer[4096];
                const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);   // read the request
                std::size_t connection = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    connection = requests_.size();
                    requests_.emplace_back(buffer, n > 0 ? static_cast<std::size_t>(n) : 0);
                }
                const std::string& reply = replies_[connection % replies_.size()];
                (void)!::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
                ::close(client);
            }
        });
    }

    ~CannedServer() {
        ::shutdown(listenFd_, SHUT_RDWR);   // makes accept() return with an error
        thread_.join();
        ::close(listenFd_);
    }

    CannedServer(const CannedServer&) = delete;
    CannedServer& operator=(const CannedServer&) = delete;

    int port() const { return port_; }

    // What the peers sent, one entry per connection (first read only, which
    // is the whole request for the small requests the tests send). Health
    // probes show up here too.
    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    const std::vector<std::string> replies_;
    int listenFd_ = -1;
    int port_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
    std::thread thread_;
};

// A "backend" that answers every connection with bytes that are not HTTP.
class GarbageServer : public CannedServer {
public:
    GarbageServer() : CannedServer("this is definitely not HTTP\r\n\r\n") {}
};

// A "backend" that accepts TCP connections but never reads or answers.
// The socket listens and nobody calls accept(): the kernel completes the
// handshake and buffers what the peer sends, so from outside this looks like
// a server that has hung.
class SilentServer {
public:
    SilentServer() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listenFd_, 16);
        socklen_t length = sizeof(address);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
    }
    ~SilentServer() { ::close(listenFd_); }
    SilentServer(const SilentServer&) = delete;
    SilentServer& operator=(const SilentServer&) = delete;

    int port() const { return port_; }

private:
    int listenFd_ = -1;
    int port_ = 0;
};

// A host name that can never resolve, on any machine and without any DNS
// traffic: one label is longer than the 63 characters DNS allows, so the
// resolver rejects it locally.
inline const std::string kUnresolvableHost = std::string(70, 'x') + ".invalid";

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
    int shutdownGraceMs = 5000;        // miniLB's own default
    std::string backendHost = "127.0.0.1";   // host written for every backend
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
             << "shutdown_grace_ms: " << options.shutdownGraceMs << "\n"
             << "backends:\n";
        for (std::size_t i = 0; i < backendPorts_.size(); ++i) {
            const int weight = i < options.weights.size() ? options.weights[i] : 1;
            yaml << "  - { host: " << options.backendHost << ", port: " << backendPorts_[i]
                 << ", weight: " << weight
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

    // How many requests miniLB has sent to a backend according to /stats
    // (a retry counts as a request to the second backend).
    long requestsOn(int index) const {
        const std::string json = stats();
        const std::size_t start = json.find("{\"id\": \"" + backendId(index) + "\"");
        if (start == std::string::npos) {
            return -1;
        }
        return statNumber(json.substr(start), "requests");
    }

    // Everything miniLB has written to stdout/stderr so far.
    std::string lbLog() const {
        std::ifstream log(dir_ + "/lb.log");
        std::ostringstream text;
        text << log.rdbuf();
        return text.str();
    }

    // Runs the miniLB binary with `args` and waits for it to exit by itself.
    // Returns the exit code; what it printed is stored in `output`.
    int runMiniLb(const std::vector<std::string>& args, std::string& output) {
        const std::string logFile = dir_ + "/cli.log";
        std::remove(logFile.c_str());
        std::vector<std::string> command = {MINILB_BIN};
        command.insert(command.end(), args.begin(), args.end());

        Process process;
        if (!process.start(command, logFile)) {
            failSetup("could not start miniLB");
        }
        const int exitCode = process.waitForExit();
        std::ifstream log(logFile);
        std::ostringstream text;
        text << log.rdbuf();
        output = text.str();
        return exitCode;
    }

    // Writes `yaml` to a file in the test directory and returns its path.
    std::string writeFile(const std::string& name, const std::string& yaml) const {
        const std::string path = dir_ + "/" + name;
        std::ofstream file(path);
        file << yaml;
        return path;
    }

    std::string dir_;
    int lbPort_ = 0;
    int redisPort_ = 0;
    std::vector<int> backendPorts_;
    std::vector<std::unique_ptr<Process>> backends_;
    Process redis_;
    Process lb_;
};

} // namespace minilb_test

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
