#include "miniLB/http/Headers.hpp"

#include <algorithm>
#include <cctype>

namespace minilb {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        // The cast avoids undefined behaviour for bytes >= 0x80.
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

const std::string* findHeader(const HeaderList& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (iequals(key, name)) {
            return &value;
        }
    }
    return nullptr;
}

void removeHeader(HeaderList& headers, std::string_view name) {
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [&](const auto& header) { return iequals(header.first, name); }),
                  headers.end());
}

void setHeader(HeaderList& headers, std::string_view name, std::string_view value) {
    removeHeader(headers, name);
    headers.emplace_back(std::string(name), std::string(value));
}

void removeHopByHopHeaders(HeaderList& headers) {
    // Transfer-Encoding is also hop-by-hop, but we forward bodies exactly as
    // received (we never re-frame them), so it has to stay.
    for (const char* name : {"Connection", "Keep-Alive", "Proxy-Connection", "Upgrade"}) {
        removeHeader(headers, name);
    }
}

} // namespace minilb
