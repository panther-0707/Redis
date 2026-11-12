#pragma once

#include "miniLB/http/Headers.hpp"

#include <string>
#include <string_view>

namespace minilb {

// A parsed HTTP/1.x request. Plain data: filled in by HttpParser.
struct HttpRequest {
    std::string method;    // "GET"
    std::string target;    // "/search?q=cats"  (exactly as sent by the client)
    std::string path;      // "/search"
    std::string query;     // "q=cats"          (without the '?', may be empty)
    std::string version;   // "HTTP/1.1"
    HeaderList headers;
    std::string body;

    // First value of the given header, or nullptr if the header is absent.
    const std::string* header(std::string_view name) const { return findHeader(headers, name); }

    // Converts the request back to wire format so it can be sent to a backend.
    std::string serialize() const;
};

} // namespace minilb
