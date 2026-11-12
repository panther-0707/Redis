#pragma once

#include "miniLB/http/Headers.hpp"

#include <string>
#include <string_view>

namespace minilb {

// An HTTP/1.x response: either parsed from a backend (HttpResponseParser)
// or built by the load balancer itself (error pages, /stats).
struct HttpResponse {
    std::string version = "HTTP/1.1";
    int status = 200;
    std::string reason = "OK";
    HeaderList headers;
    std::string body;   // stored exactly as received (never re-encoded)

    const std::string* header(std::string_view name) const { return findHeader(headers, name); }
    void setHeader(std::string_view name, std::string_view value) {
        minilb::setHeader(headers, name, value);
    }

    // Converts the response to wire format.
    std::string serialize() const;

    // Builds a complete small response (used for error pages and /stats).
    // Sets Content-Type, Content-Length and "Connection: close".
    static HttpResponse make(int status, std::string_view body,
                             std::string_view contentType = "text/plain");

    // Standard reason phrase for a status code, e.g. 404 -> "Not Found".
    static std::string_view reasonPhrase(int status);
};

} // namespace minilb
