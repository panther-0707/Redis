#pragma once

#include "miniLB/http/HttpRequest.hpp"
#include "miniLB/http/HttpResponse.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace minilb {

// Result of feeding bytes to a parser.
enum class ParseStatus {
    NeedMore,   // valid so far, but the message is not complete yet
    Complete,   // a full message has been parsed
    Error,      // the bytes are not acceptable (malformed, too large, ...)
};

// Incremental parser for one HTTP/1.x request.
//
// TCP delivers a byte stream, not messages: a request can arrive split across
// many reads (even one byte at a time). So the parser is fed whatever bytes
// have arrived and reports whether it needs more:
//
//     HttpParser parser;
//     while (parser.feed(bytesJustRead) == ParseStatus::NeedMore) { read more }
//
// Supported: request line, headers, optional Content-Length body.
// Not supported: chunked request bodies, pipelining (bytes after the first
// request are ignored because we serve one request per connection).
//
// Not thread-safe; each connection owns its own parser.
class HttpParser {
public:
    // Size caps protect the server from clients that send endless data.
    struct Limits {
        std::size_t maxHeaderBytes = 16 * 1024;     // request line + all headers
        std::size_t maxBodyBytes = 1024 * 1024;     // Content-Length cap
    };

    HttpParser() = default;
    explicit HttpParser(Limits limits) : limits_(limits) {}

    // Feeds the next bytes read from the socket. Once the result is Complete
    // or Error it stays that way; further calls are ignored.
    ParseStatus feed(std::string_view data);

    // The parsed request. Only meaningful after feed() returned Complete.
    const HttpRequest& request() const { return request_; }
    HttpRequest& request() { return request_; }

    // After an Error: the HTTP status code to answer with
    // (400 malformed, 413 body too large, 431 headers too large,
    // 501 unsupported feature) and a human-readable explanation.
    int errorStatus() const { return errorStatus_; }
    const std::string& errorMessage() const { return errorMessage_; }

private:
    ParseStatus fail(int status, std::string message);
    bool parseHead(std::string_view head);

    Limits limits_;
    ParseStatus status_ = ParseStatus::NeedMore;
    std::string buffer_;                // every byte received so far
    std::size_t searchFrom_ = 0;        // where to resume looking for the blank line
    bool headDone_ = false;             // request line + headers parsed?
    std::size_t bodyStart_ = 0;         // offset of the body inside buffer_
    std::size_t contentLength_ = 0;
    HttpRequest request_;
    int errorStatus_ = 0;
    std::string errorMessage_;
};

// Incremental parser for one HTTP/1.x response coming from a backend.
//
// How do we know where the body ends? In order of precedence:
//   1. No body at all: reply to a HEAD request, or status 1xx / 204 / 304.
//   2. Transfer-Encoding present (e.g. chunked): we do not decode it. The body
//      is everything until the backend closes the connection, and is passed
//      on to the client untouched together with the Transfer-Encoding header.
//   3. Content-Length: exactly that many bytes.
//   4. Otherwise: everything until the backend closes the connection.
//
// Cases 2 and 4 only finish when the caller reports end-of-stream via
// finish(). We always send "Connection: close" to backends, so they do close.
class HttpResponseParser {
public:
    // maxBytes caps the whole response (head + body).
    // headRequest must be true if the request was a HEAD (see case 1 above).
    explicit HttpResponseParser(std::size_t maxBytes = 8 * 1024 * 1024, bool headRequest = false)
        : maxBytes_(maxBytes), headRequest_(headRequest) {}

    ParseStatus feed(std::string_view data);

    // Call when the backend closed the connection. Returns Complete if the
    // bytes received so far form a whole response, Error if it was cut short.
    ParseStatus finish();

    const HttpResponse& response() const { return response_; }
    HttpResponse& response() { return response_; }
    const std::string& errorMessage() const { return errorMessage_; }

private:
    ParseStatus fail(std::string message);
    bool parseHead(std::string_view head);

    std::size_t maxBytes_;
    bool headRequest_;
    ParseStatus status_ = ParseStatus::NeedMore;
    std::string buffer_;
    std::size_t searchFrom_ = 0;
    bool headDone_ = false;
    std::size_t bodyStart_ = 0;
    bool hasContentLength_ = false;     // body length known up front (case 1 or 3)
    std::size_t contentLength_ = 0;
    HttpResponse response_;
    std::string errorMessage_;
};

} // namespace minilb
