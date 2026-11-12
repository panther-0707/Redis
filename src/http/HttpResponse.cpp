#include "miniLB/http/HttpResponse.hpp"

namespace minilb {

std::string HttpResponse::serialize() const {
    std::string out;
    out.reserve(256 + body.size());

    // Status line, e.g. "HTTP/1.1 200 OK"
    out += version;
    out += ' ';
    out += std::to_string(status);
    out += ' ';
    out += reason;
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

HttpResponse HttpResponse::make(int status, std::string_view body, std::string_view contentType) {
    HttpResponse response;
    response.status = status;
    response.reason = std::string(reasonPhrase(status));
    response.body = std::string(body);
    response.setHeader("Content-Type", contentType);
    response.setHeader("Content-Length", std::to_string(response.body.size()));
    response.setHeader("Connection", "close");
    return response;
}

std::string_view HttpResponse::reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 413: return "Content Too Large";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

} // namespace minilb
