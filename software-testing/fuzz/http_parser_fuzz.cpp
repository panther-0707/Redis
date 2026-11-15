// Coverage-guided fuzzing of the HTTP parsers (libFuzzer).
//
// Technique: fuzzing. libFuzzer feeds this function millions of inputs and
// uses code coverage to keep the ones that reach new code, mutating them
// further. The build adds AddressSanitizer and UndefinedBehaviorSanitizer,
// so any memory error or undefined behaviour stops the run and the input
// that caused it is written to a file.
//
// "It did not crash" is a weak oracle, so every input is also checked
// against properties that must hold for ALL inputs:
//
//   1. Fragmentation: feeding the bytes in one piece, in three pieces and
//      one byte at a time gives the same verdict and the same parsed request
//      (requirement F14; this is what defect D1 violated).
//   2. Round trip: a request that parses can be serialised and parsed again
//      to the same request (what miniLB forwards is what it understood).
//   3. A rejected request has one of the documented status codes.
//   4. Complete requests have exactly Content-Length body bytes.
//   5. The response parser gives the same result in one piece and in pieces,
//      including after the connection closes (finish()).
//
// The first four bytes of each input choose the size limits and the split
// points, so the fuzzer explores those together with the payload.
//
// Build and run: software-testing/scripts/fuzz.sh [seconds]

#include "miniLB/http/HttpParser.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using minilb::HttpParser;
using minilb::HttpResponseParser;
using minilb::ParseStatus;

namespace {

[[noreturn]] void violated(const char* property, std::string_view input) {
    std::fprintf(stderr, "PROPERTY VIOLATED: %s\ninput (%zu bytes): ", property, input.size());
    for (const unsigned char c : input) {
        if (c >= 0x20 && c < 0x7f && c != '\\') {
            std::fputc(c, stderr);
        } else {
            std::fprintf(stderr, "\\x%02x", c);
        }
    }
    std::fputc('\n', stderr);
    std::abort();   // libFuzzer saves the input as crash-<hash>
}

std::vector<std::string_view> pieces(std::string_view text, std::size_t cutA, std::size_t cutB) {
    cutA %= text.size() + 1;
    cutB %= text.size() + 1;
    if (cutA > cutB) {
        std::swap(cutA, cutB);
    }
    return {text.substr(0, cutA), text.substr(cutA, cutB - cutA), text.substr(cutB)};
}

struct RequestOutcome {
    ParseStatus status = ParseStatus::NeedMore;
    int errorStatus = 0;
    std::string method, target, path, query, body;
    minilb::HeaderList headers;

    bool operator==(const RequestOutcome& other) const {
        return status == other.status && errorStatus == other.errorStatus && method == other.method &&
               target == other.target && path == other.path && query == other.query &&
               body == other.body && headers == other.headers;
    }
};

RequestOutcome parseRequest(const std::vector<std::string_view>& parts, HttpParser::Limits limits) {
    HttpParser parser(limits);
    RequestOutcome outcome;
    for (const std::string_view part : parts) {
        outcome.status = parser.feed(part);
    }
    if (outcome.status == ParseStatus::Error) {
        outcome.errorStatus = parser.errorStatus();
    } else if (outcome.status == ParseStatus::Complete) {
        const minilb::HttpRequest& request = parser.request();
        outcome.method = request.method;
        outcome.target = request.target;
        outcome.path = request.path;
        outcome.query = request.query;
        outcome.body = request.body;
        outcome.headers = request.headers;
    }
    return outcome;
}

struct ResponseOutcome {
    ParseStatus afterFeed = ParseStatus::NeedMore;
    ParseStatus afterClose = ParseStatus::NeedMore;
    int status = 0;
    std::string body;
    minilb::HeaderList headers;

    bool operator==(const ResponseOutcome& other) const {
        return afterFeed == other.afterFeed && afterClose == other.afterClose && status == other.status &&
               body == other.body && headers == other.headers;
    }
};

ResponseOutcome parseResponse(const std::vector<std::string_view>& parts, std::size_t maxBytes, bool head) {
    HttpResponseParser parser(maxBytes, head);
    ResponseOutcome outcome;
    for (const std::string_view part : parts) {
        outcome.afterFeed = parser.feed(part);
    }
    outcome.afterClose = parser.finish();   // the backend closed the connection
    if (outcome.afterClose == ParseStatus::Complete) {
        outcome.status = parser.response().status;
        outcome.body = parser.response().body;
        outcome.headers = parser.response().headers;
    }
    return outcome;
}

std::vector<std::string_view> byteByByte(std::string_view text) {
    std::vector<std::string_view> parts;
    for (std::size_t i = 0; i < text.size(); ++i) {
        parts.push_back(text.substr(i, 1));
    }
    return parts;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 4) {
        return 0;
    }
    const std::string_view payload(reinterpret_cast<const char*>(data) + 4, size - 4);
    const std::size_t cutA = data[2];
    const std::size_t cutB = static_cast<std::size_t>(data[2]) + data[3];

    // ------------------------------ requests --------------------------------
    HttpParser::Limits limits;
    limits.maxHeaderBytes = 8 + static_cast<std::size_t>(data[0]) * 2;   // 8 .. 518
    limits.maxBodyBytes = data[1];                                       // 0 .. 255

    const RequestOutcome whole = parseRequest({payload}, limits);
    if (!(parseRequest(pieces(payload, cutA, cutB), limits) == whole)) {
        violated("request: three pieces give a different result than one piece", payload);
    }
    if (!(parseRequest(byteByByte(payload), limits) == whole)) {
        violated("request: byte-by-byte gives a different result than one piece", payload);
    }
    if (whole.status == ParseStatus::Error && whole.errorStatus != 400 && whole.errorStatus != 413 &&
        whole.errorStatus != 431 && whole.errorStatus != 501) {
        violated("request: rejected with an undocumented status", payload);
    }
    if (whole.status == ParseStatus::Complete) {
        HttpParser first(limits);
        first.feed(payload);
        const std::string again = first.request().serialize();
        HttpParser second;   // default (large) limits: only the content matters here
        if (second.feed(again) != ParseStatus::Complete) {
            violated("request: the serialised form of a parsed request does not parse", payload);
        }
        if (second.request().method != whole.method || second.request().target != whole.target ||
            second.request().headers != whole.headers || second.request().body != whole.body) {
            violated("request: serialise + parse changed the request", payload);
        }
        const std::string* length = first.request().header("Content-Length");
        if (length != nullptr && std::to_string(whole.body.size()) != *length &&
            std::stoull(*length) != whole.body.size()) {
            violated("request: body size differs from Content-Length", payload);
        }
    }

    // ------------------------------ responses -------------------------------
    const std::size_t maxBytes = 16 + static_cast<std::size_t>(data[0]) * 4;   // 16 .. 1036
    const bool head = (data[1] & 1) != 0;

    const ResponseOutcome wholeResponse = parseResponse({payload}, maxBytes, head);
    if (!(parseResponse(pieces(payload, cutA, cutB), maxBytes, head) == wholeResponse)) {
        violated("response: three pieces give a different result than one piece", payload);
    }
    if (!(parseResponse(byteByByte(payload), maxBytes, head) == wholeResponse)) {
        violated("response: byte-by-byte gives a different result than one piece", payload);
    }
    return 0;
}
