#include "miniLB/util/Metrics.hpp"

#include <sstream>

namespace minilb {

namespace {

// Escapes a string for use inside JSON double quotes.
std::string jsonEscape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';   // control characters cannot appear raw in JSON
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

std::string Metrics::toJson(const BackendPool& pool, const std::string& algorithm,
                            bool redisAvailable) const {
    std::ostringstream json;
    json << "{\n"
         << "  \"total_requests\": " << totalRequests.load() << ",\n"
         << "  \"active_connections\": " << activeConnections.load() << ",\n"
         << "  \"cache_hits\": " << cacheHits.load() << ",\n"
         << "  \"cache_misses\": " << cacheMisses.load() << ",\n"
         << "  \"rate_limited\": " << rateLimited.load() << ",\n"
         << "  \"errors\": " << errors.load() << ",\n"
         << "  \"algorithm\": \"" << jsonEscape(algorithm) << "\",\n"
         << "  \"redis_available\": " << (redisAvailable ? "true" : "false") << ",\n"
         << "  \"backends\": [\n";

    const auto& backends = pool.all();
    for (std::size_t i = 0; i < backends.size(); ++i) {
        const Backend& backend = *backends[i];
        json << "    {\"id\": \"" << jsonEscape(backend.id) << "\""
             << ", \"healthy\": " << (backend.healthy.load() ? "true" : "false")
             << ", \"weight\": " << backend.weight
             << ", \"active_connections\": " << backend.activeConnections.load()
             << ", \"requests\": " << backend.totalRequests.load() << "}"
             << (i + 1 < backends.size() ? ",\n" : "\n");
    }

    json << "  ]\n}\n";
    return json.str();
}

} // namespace minilb
