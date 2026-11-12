// demo_backend: a tiny HTTP server used as a load-balancing target.
//
//   demo_backend --port 8001 --name backend-1
//
// Routes:
//   GET  /        -> "Hello from <name>!"
//   GET  /health  -> 200 "OK"                 (used by the health checker)
//   GET  /slow    -> waits 2 seconds first    (handy for seeing the cache work)
//   GET  /work    -> blocks this server for 1 ms (a backend with limited capacity,
//                    about 1000 requests/second; used by the benchmarks)
//   POST /echo    -> sends the request body back
//   anything else -> 404
//
// It is single-threaded and asynchronous (one Boost.Asio io_context), serves
// one request per connection and then closes it. It reuses miniLB's own
// HttpParser, so it is also a small real-world test of the parser.

#include "miniLB/http/HttpParser.hpp"

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using minilb::HttpResponse;
using minilb::ParseStatus;

namespace {

// One client connection. shared_from_this() keeps the object alive for as
// long as an asynchronous operation still refers to it.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::string name)
        : socket_(std::move(socket)), timer_(socket_.get_executor()), name_(std::move(name)) {}

    void start() { readMore(); }

private:
    void readMore() {
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(readBuffer_),
            [this, self](boost::system::error_code ec, std::size_t n) {
                if (ec) {
                    return;   // client went away; the session is destroyed
                }
                switch (parser_.feed(std::string_view(readBuffer_.data(), n))) {
                    case ParseStatus::NeedMore:
                        readMore();
                        break;
                    case ParseStatus::Complete:
                        handleRequest();
                        break;
                    case ParseStatus::Error:
                        respond(HttpResponse::make(parser_.errorStatus(),
                                                   parser_.errorMessage() + "\n"));
                        break;
                }
            });
    }

    void handleRequest() {
        const minilb::HttpRequest& request = parser_.request();

        if (request.path == "/health") {
            respond(HttpResponse::make(200, "OK\n"));
        } else if (request.path == "/") {
            respond(HttpResponse::make(200, "Hello from " + name_ + "!\n"));
        } else if (request.path == "/echo") {
            respond(HttpResponse::make(200, request.body));
        } else if (request.path == "/work") {
            // Deliberately blocks the only thread, so this backend cannot
            // serve anything else for 1 ms. That caps it at roughly 1000
            // requests/second and makes "1 backend vs 3 backends" visible
            // in a benchmark.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            respond(HttpResponse::make(200, "Work done by " + name_ + "!\n"));
        } else if (request.path == "/slow") {
            // Wait without blocking the thread, so other clients (and the
            // health checker) are still served during the two seconds.
            auto self = shared_from_this();
            timer_.expires_after(std::chrono::seconds(2));
            timer_.async_wait([this, self](boost::system::error_code) {
                respond(HttpResponse::make(200, "Slow hello from " + name_ + "!\n"));
            });
        } else {
            respond(HttpResponse::make(404, "Not found\n"));
        }
    }

    void respond(HttpResponse response) {
        // A reply to HEAD has the same headers as GET but no body.
        if (parser_.request().method == "HEAD") {
            response.body.clear();
        }
        auto self = shared_from_this();
        writeBuffer_ = response.serialize();
        asio::async_write(socket_, asio::buffer(writeBuffer_),
                          [this, self](boost::system::error_code, std::size_t) {
                              // One request per connection: say goodbye properly.
                              boost::system::error_code ignored;
                              socket_.shutdown(tcp::socket::shutdown_both, ignored);
                          });
    }

    tcp::socket socket_;
    asio::steady_timer timer_;
    std::string name_;
    std::array<char, 4096> readBuffer_{};
    std::string writeBuffer_;   // must outlive the async_write
    minilb::HttpParser parser_;
};

// Accepts connections forever, creating one Session per client.
void acceptLoop(tcp::acceptor& acceptor, const std::string& name) {
    acceptor.async_accept([&acceptor, &name](boost::system::error_code ec, tcp::socket socket) {
        if (!acceptor.is_open()) {
            return;   // shutting down
        }
        if (!ec) {
            std::make_shared<Session>(std::move(socket), name)->start();
        }
        acceptLoop(acceptor, name);
    });
}

} // namespace

int main(int argc, char** argv) {
    unsigned short port = 8001;
    std::string name = "backend";

    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--port") {
            port = static_cast<unsigned short>(std::atoi(argv[i + 1]));
        } else if (flag == "--name") {
            name = argv[i + 1];
        } else {
            std::cerr << "usage: " << argv[0] << " --port <port> --name <name>\n";
            return 2;
        }
    }

    try {
        asio::io_context io;
        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), port));

        // Ctrl-C / `kill` stop the event loop so the process exits cleanly.
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](boost::system::error_code, int) { io.stop(); });

        acceptLoop(acceptor, name);
        std::cout << name << " listening on port " << port << std::endl;
        io.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
