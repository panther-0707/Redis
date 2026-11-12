#include "miniLB/http/HttpRequest.hpp"

namespace minilb {

std::string HttpRequest::serialize() const {
    std::string out;
    out.reserve(256 + body.size());

    // Request line, e.g. "GET /index.html HTTP/1.1"
    out += method;
    out += ' ';
    out += target;
    out += ' ';
    out += version;
    out += "\r\n";

    for (const auto& [name, value] : headers) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }

    out += "\r\n";   // blank line = end of headers
    out += body;
    return out;
}

} // namespace minilb
