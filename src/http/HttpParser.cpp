#include "miniLB/http/HttpParser.hpp"

#include <charconv>

namespace minilb {

namespace {

// A blank line (CRLF CRLF) separates the head (start line + headers) from the body.
constexpr std::string_view kHeadEnd = "\r\n\r\n";
constexpr std::string_view kCrlf = "\r\n";

// Removes leading/trailing spaces and tabs.
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Characters allowed in a header name ("token" in RFC 9110).
bool isTokenChar(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
}

// Control characters (other than tab) are never valid inside a header value.
bool hasControlChars(std::string_view text) {
    for (const unsigned char c : text) {
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return true;
        }
    }
    return false;
}

// Parses an unsigned decimal number. Rejects empty strings, signs, spaces,
// trailing junk and values that overflow.
bool parseSize(std::string_view text, std::size_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc() && result.ptr == last;
}

// Parses the header block: zero or more "Name: value" lines separated by CRLF.
bool parseHeaderLines(std::string_view block, HeaderList& headers, std::string& error) {
    while (!block.empty()) {
        // Cut the next line off the front of the block.
        const std::size_t lineEnd = block.find(kCrlf);
        const std::string_view line = block.substr(0, lineEnd);
        block = lineEnd == std::string_view::npos ? std::string_view{}
                                                  : block.substr(lineEnd + kCrlf.size());

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            error = "header line without a name";
            return false;
        }

        const std::string_view name = line.substr(0, colon);
        for (const unsigned char c : name) {
            // This also rejects "Name : value" and obsolete folded lines
            // (lines starting with a space), both known smuggling tricks.
            if (!isTokenChar(c)) {
                error = "invalid character in header name";
                return false;
            }
        }

        const std::string_view value = trim(line.substr(colon + 1));
        if (hasControlChars(value)) {
            error = "invalid character in header value";
            return false;
        }

        headers.emplace_back(std::string(name), std::string(value));
    }
    return true;
}

// Looks up Content-Length. Sets `present` and `length`.
// Fails on non-numeric values and on several headers that disagree: if a
// proxy and a backend pick different lengths they disagree about where the
// next request starts ("request smuggling").
bool readContentLength(const HeaderList& headers, bool& present, std::size_t& length,
                       std::string& error) {
    present = false;
    for (const auto& [name, value] : headers) {
        if (!iequals(name, "Content-Length")) {
            continue;
        }
        std::size_t parsed = 0;
        if (!parseSize(value, parsed)) {
            error = "invalid Content-Length";
            return false;
        }
        if (present && parsed != length) {
            error = "conflicting Content-Length headers";
            return false;
        }
        present = true;
        length = parsed;
    }
    return true;
}

// Searches `buffer` for the blank line that ends the head. `searchFrom`
// remembers how far earlier calls already looked, so repeated calls on a
// slowly growing buffer do not rescan it from the start every time.
std::size_t findHeadEnd(const std::string& buffer, std::size_t& searchFrom) {
    const std::size_t pos = buffer.find(kHeadEnd, searchFrom);
    if (pos == std::string::npos) {
        // The terminator may be split across two reads, so step back 3 bytes.
        searchFrom = buffer.size() > 3 ? buffer.size() - 3 : 0;
    }
    return pos;
}

} // namespace

// ============================== HttpParser =================================

ParseStatus HttpParser::fail(int status, std::string message) {
    errorStatus_ = status;
    errorMessage_ = std::move(message);
    status_ = ParseStatus::Error;
    return status_;
}

ParseStatus HttpParser::feed(std::string_view data) {
    if (status_ != ParseStatus::NeedMore) {
        return status_;   // already finished, ignore extra bytes
    }
    buffer_.append(data);

    // ---- Phase 1: request line + headers --------------------------------
    if (!headDone_) {
        const std::size_t headEnd = findHeadEnd(buffer_, searchFrom_);
        if (headEnd == std::string::npos) {
            if (buffer_.size() > limits_.maxHeaderBytes) {
                return fail(431, "request headers too large");
            }
            return ParseStatus::NeedMore;
        }
        if (headEnd > limits_.maxHeaderBytes) {
            return fail(431, "request headers too large");
        }
        if (!parseHead(std::string_view(buffer_).substr(0, headEnd))) {
            return status_;   // parseHead already called fail()
        }
        headDone_ = true;
        bodyStart_ = headEnd + kHeadEnd.size();
    }

    // ---- Phase 2: body (Content-Length bytes) ----------------------------
    if (buffer_.size() - bodyStart_ < contentLength_) {
        return ParseStatus::NeedMore;
    }
    request_.body = buffer_.substr(bodyStart_, contentLength_);
    status_ = ParseStatus::Complete;
    return status_;
}

bool HttpParser::parseHead(std::string_view head) {
    // Split off the request line: "METHOD SP target SP HTTP/1.x"
    const std::size_t lineEnd = head.find(kCrlf);
    const std::string_view requestLine = head.substr(0, lineEnd);
    const std::string_view headerBlock =
        lineEnd == std::string_view::npos ? std::string_view{} : head.substr(lineEnd + kCrlf.size());

    const std::size_t firstSpace = requestLine.find(' ');
    const std::size_t lastSpace = requestLine.rfind(' ');
    if (firstSpace == std::string_view::npos || firstSpace == lastSpace) {
        fail(400, "malformed request line");
        return false;
    }
    const std::string_view method = requestLine.substr(0, firstSpace);
    const std::string_view target = requestLine.substr(firstSpace + 1, lastSpace - firstSpace - 1);
    const std::string_view version = requestLine.substr(lastSpace + 1);

    // Method: one or more upper-case letters (GET, POST, ...).
    if (method.empty()) {
        fail(400, "missing method");
        return false;
    }
    for (const char c : method) {
        if (c < 'A' || c > 'Z') {
            fail(400, "invalid method");
            return false;
        }
    }

    // Target: we only accept the usual "origin form", i.e. a path starting
    // with '/'. No spaces or control characters.
    if (target.empty() || target.front() != '/') {
        fail(400, "request target must start with '/'");
        return false;
    }
    for (const unsigned char c : target) {
        if (c <= 0x20 || c == 0x7f) {
            fail(400, "invalid character in request target");
            return false;
        }
    }

    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        fail(400, "unsupported HTTP version");
        return false;
    }

    std::string error;
    if (!parseHeaderLines(headerBlock, request_.headers, error)) {
        fail(400, error);
        return false;
    }

    // We do not implement chunked *request* bodies. Rejecting them outright is
    // safer than guessing where the body ends.
    if (request_.header("Transfer-Encoding") != nullptr) {
        fail(501, "Transfer-Encoding request bodies are not supported");
        return false;
    }

    bool hasLength = false;
    if (!readContentLength(request_.headers, hasLength, contentLength_, error)) {
        fail(400, error);
        return false;
    }
    if (contentLength_ > limits_.maxBodyBytes) {
        fail(413, "request body too large");
        return false;
    }

    request_.method = std::string(method);
    request_.target = std::string(target);
    request_.version = std::string(version);

    // Split "/path?query" into its two parts.
    const std::size_t question = target.find('?');
    request_.path = std::string(target.substr(0, question));
    request_.query =
        question == std::string_view::npos ? std::string() : std::string(target.substr(question + 1));
    return true;
}

// ========================== HttpResponseParser =============================

ParseStatus HttpResponseParser::fail(std::string message) {
    errorMessage_ = std::move(message);
    status_ = ParseStatus::Error;
    return status_;
}

ParseStatus HttpResponseParser::feed(std::string_view data) {
    if (status_ != ParseStatus::NeedMore) {
        return status_;
    }
    buffer_.append(data);
    if (buffer_.size() > maxBytes_) {
        return fail("response too large");
    }

    // ---- Phase 1: status line + headers ----------------------------------
    if (!headDone_) {
        const std::size_t headEnd = findHeadEnd(buffer_, searchFrom_);
        if (headEnd == std::string::npos) {
            return ParseStatus::NeedMore;
        }
        if (!parseHead(std::string_view(buffer_).substr(0, headEnd))) {
            return status_;
        }
        headDone_ = true;
        bodyStart_ = headEnd + kHeadEnd.size();
    }

    // ---- Phase 2: body -----------------------------------------------------
    if (!hasContentLength_) {
        return ParseStatus::NeedMore;   // body ends when the backend closes: see finish()
    }
    if (buffer_.size() - bodyStart_ < contentLength_) {
        return ParseStatus::NeedMore;
    }
    response_.body = buffer_.substr(bodyStart_, contentLength_);
    status_ = ParseStatus::Complete;
    return status_;
}

ParseStatus HttpResponseParser::finish() {
    if (status_ != ParseStatus::NeedMore) {
        return status_;
    }
    if (!headDone_) {
        return fail("backend closed the connection before sending a full response head");
    }
    if (hasContentLength_) {
        return fail("backend closed the connection before sending the full body");
    }
    // Body delimited by connection close: everything after the head.
    response_.body = buffer_.substr(bodyStart_);
    status_ = ParseStatus::Complete;
    return status_;
}

bool HttpResponseParser::parseHead(std::string_view head) {
    // Split off the status line: "HTTP/1.x SP 3-digit-code [SP reason]"
    const std::size_t lineEnd = head.find(kCrlf);
    const std::string_view statusLine = head.substr(0, lineEnd);
    const std::string_view headerBlock =
        lineEnd == std::string_view::npos ? std::string_view{} : head.substr(lineEnd + kCrlf.size());

    const std::size_t firstSpace = statusLine.find(' ');
    if (firstSpace == std::string_view::npos) {
        fail("malformed status line");
        return false;
    }
    const std::string_view version = statusLine.substr(0, firstSpace);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        fail("backend did not answer with HTTP/1.x");
        return false;
    }

    const std::string_view code = statusLine.substr(firstSpace + 1, 3);
    std::size_t status = 0;
    if (code.size() != 3 || !parseSize(code, status) || status < 100 || status > 599) {
        fail("invalid status code");
        return false;
    }
    // After the code there must be either nothing or " reason".
    const std::string_view afterCode = statusLine.substr(firstSpace + 4);
    if (!afterCode.empty() && afterCode.front() != ' ') {
        fail("invalid status code");
        return false;
    }

    std::string error;
    if (!parseHeaderLines(headerBlock, response_.headers, error)) {
        fail(error);
        return false;
    }

    response_.version = std::string(version);
    response_.status = static_cast<int>(status);
    response_.reason = std::string(trim(afterCode));

    // Decide how the body is delimited (see the class comment for the cases).
    const bool neverHasBody =
        headRequest_ || (status >= 100 && status < 200) || status == 204 || status == 304;
    if (neverHasBody) {
        hasContentLength_ = true;
        contentLength_ = 0;
    } else if (response_.header("Transfer-Encoding") != nullptr) {
        hasContentLength_ = false;   // read until the backend closes
    } else {
        if (!readContentLength(response_.headers, hasContentLength_, contentLength_, error)) {
            fail(error);
            return false;
        }
        if (hasContentLength_ && contentLength_ > maxBytes_) {
            fail("response too large");
            return false;
        }
    }
    return true;
}

} // namespace minilb
